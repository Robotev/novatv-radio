#include "mp3_player.h"

#include <string.h>

#include "debounce.h"
#include "driver/gpio.h"
#include "driver/uart.h"
#include "esp_log.h"
#include "esp_timer.h"

#define MP3_UART_NUM UART_NUM_1
#define MP3_UART_TX_GPIO 13 // to the module's RX
#define MP3_UART_RX_GPIO 14 // from the module's TX
#define MP3_UART_BAUD 9600
#define MP3_UART_RX_BUF_SIZE 256

#define MP3_BUSY_GPIO GPIO_NUM_12 // LOW while a file is playing

// Placeholder pending bench verification: some of these modules pulse BUSY
// briefly right when a play command is accepted, before real playback
// actually starts. Without filtering, that pulse's own falling edge looks
// identical to a genuine end-of-file and fires it near-instantly. This must
// be shorter than any real audio clip but longer than that startup pulse.
#define BUSY_DEBOUNCE_US 150000

#define MP3_DEVICE_FLASH 0x02

#define MP3_CMD_PLAY 0x08
#define MP3_CMD_STOP 0x04
#define MP3_CMD_QUERY_PLAY_STATE 0x01
#define MP3_CMD_QUERY_TRACK 0x0D
#define MP3_CMD_SET_VOLUME 0x13 // per tutrakan-storyteller, which drives the same module

// Module's volume range is 0-30 (per tutrakan-storyteller); 30*0.95 = 28.5,
// rounded to 29 for 95%.
#define MP3_VOLUME_95_PERCENT 29

// Reused unchanged from the tutrakan-storyteller firmware, which drives the
// same module. SETTLE_MS there was raised from tutrakan-teleprojector's
// original 250ms after BUSY was observed taking up to ~280ms to assert
// after PLAY - querying at exactly 250ms read "not playing yet" as a
// failure and needlessly resent PLAY, restarting the file.
#define SETTLE_MS 400
#define REPLY_TIMEOUT_MS 150
#define MAX_ATTEMPTS 4

static const char *TAG = "mp3_player";

typedef enum { CONFIRM_IDLE, CONFIRM_WAIT_SETTLE, CONFIRM_WAIT_STATE, CONFIRM_WAIT_TRACK } confirm_state_t;
typedef enum { RX_WAIT_START, RX_CMD, RX_LEN, RX_DATA, RX_CHK } rx_state_t;
typedef enum { TARGET_PLAY, TARGET_STOP } target_kind_t;

// RX frame parser
static rx_state_t s_rx_state = RX_WAIT_START;
static uint8_t s_rx_cmd, s_rx_len, s_rx_sum, s_rx_data_count;
static uint8_t s_rx_data[8];
static bool s_rx_frame_ready;
static uint8_t s_rx_frame_cmd;
static uint8_t s_rx_frame_data[8];

// Confirm/retry state
static confirm_state_t s_confirm_state = CONFIRM_IDLE;
static target_kind_t s_target_kind;
static int64_t s_action_time_us;
static uint8_t s_attempts_made;
static uint16_t s_target_seq_index;
static char s_target_path[16];
static bool s_last_command_failed;

// BUSY edge tracking
static debounced_input_t s_busy_debounce;
static bool s_busy_idle_event;
static bool s_busy_playing_event;

static void send_frame(uint8_t cmd, const uint8_t *data, uint8_t len)
{
    uint8_t sum = (uint8_t)(0xAA + cmd + len);
    uint8_t header[3] = {0xAA, cmd, len};
    uart_write_bytes(MP3_UART_NUM, (const char *)header, sizeof(header));
    if (len > 0 && data != NULL) {
        uart_write_bytes(MP3_UART_NUM, (const char *)data, len);
        for (uint8_t i = 0; i < len; i++) {
            sum = (uint8_t)(sum + data[i]);
        }
    }
    uart_write_bytes(MP3_UART_NUM, (const char *)&sum, 1);
}

static void send_attempt(void)
{
    // Discard any reply not yet consumed - it belongs to whatever request
    // (possibly an abandoned one from a different command entirely, given
    // this protocol has no per-request ID) preceded this attempt, and must
    // not be mistaken for a reply to what we're about to send.
    s_rx_frame_ready = false;

    if (s_target_kind == TARGET_PLAY) {
        uint8_t data[1 + sizeof(s_target_path)];
        data[0] = MP3_DEVICE_FLASH;
        size_t plen = strlen(s_target_path);
        memcpy(&data[1], s_target_path, plen);
        send_frame(MP3_CMD_PLAY, data, (uint8_t)(1 + plen));
    } else {
        send_frame(MP3_CMD_STOP, NULL, 0);
    }

    s_attempts_made++;
    s_action_time_us = esp_timer_get_time();
    s_confirm_state = CONFIRM_WAIT_SETTLE;
}

static void on_attempt_failed(void)
{
    if (s_attempts_made >= MAX_ATTEMPTS) {
        ESP_LOGE(TAG, "%s command failed after %d attempts",
                 s_target_kind == TARGET_PLAY ? "play" : "stop", s_attempts_made);
        s_confirm_state = CONFIRM_IDLE;
        s_last_command_failed = true;
    } else {
        send_attempt();
    }
}

static void poll_uart(void)
{
    uint8_t buf[32];
    int n = uart_read_bytes(MP3_UART_NUM, buf, sizeof(buf), 0);
    for (int i = 0; i < n; i++) {
        uint8_t b = buf[i];
        switch (s_rx_state) {
        case RX_WAIT_START:
            if (b == 0xAA) {
                s_rx_state = RX_CMD;
                s_rx_sum = b;
            }
            break;
        case RX_CMD:
            s_rx_cmd = b;
            s_rx_sum = (uint8_t)(s_rx_sum + b);
            s_rx_state = RX_LEN;
            break;
        case RX_LEN:
            s_rx_len = b;
            s_rx_sum = (uint8_t)(s_rx_sum + b);
            s_rx_data_count = 0;
            s_rx_state = (s_rx_len == 0) ? RX_CHK : RX_DATA;
            break;
        case RX_DATA:
            if (s_rx_data_count < sizeof(s_rx_data)) {
                s_rx_data[s_rx_data_count] = b;
            }
            s_rx_data_count++;
            s_rx_sum = (uint8_t)(s_rx_sum + b);
            if (s_rx_data_count >= s_rx_len) {
                s_rx_state = RX_CHK;
            }
            break;
        case RX_CHK:
            if (b == s_rx_sum && s_rx_len <= sizeof(s_rx_frame_data)) {
                s_rx_frame_cmd = s_rx_cmd;
                memcpy(s_rx_frame_data, s_rx_data, s_rx_len);
                s_rx_frame_ready = true;
            }
            s_rx_state = RX_WAIT_START;
            break;
        }
    }
}

static void update_busy(void)
{
    int level;
    if (debounced_input_update(&s_busy_debounce, gpio_get_level(MP3_BUSY_GPIO), esp_timer_get_time(),
                                BUSY_DEBOUNCE_US, &level)) {
        if (level == 1) {
            s_busy_idle_event = true;
        } else {
            s_busy_playing_event = true;
        }
    }
}

static void update_confirm(void)
{
    switch (s_confirm_state) {
    case CONFIRM_IDLE:
        return;

    case CONFIRM_WAIT_SETTLE:
        if (esp_timer_get_time() - s_action_time_us >= SETTLE_MS * 1000) {
            s_rx_frame_ready = false; // discard anything not yet consumed before waiting on this query's own reply
            send_frame(MP3_CMD_QUERY_PLAY_STATE, NULL, 0);
            s_action_time_us = esp_timer_get_time();
            s_confirm_state = CONFIRM_WAIT_STATE;
        }
        return;

    case CONFIRM_WAIT_STATE:
        if (s_rx_frame_ready && s_rx_frame_cmd == MP3_CMD_QUERY_PLAY_STATE) {
            uint8_t play_state = s_rx_frame_data[0];
            s_rx_frame_ready = false;
            bool ok = (s_target_kind == TARGET_PLAY) ? (play_state == 0x01) : (play_state != 0x01);
            if (!ok) {
                on_attempt_failed();
            } else if (s_target_kind == TARGET_PLAY) {
                s_rx_frame_ready = false; // discard anything not yet consumed before waiting on this query's own reply
                send_frame(MP3_CMD_QUERY_TRACK, NULL, 0);
                s_action_time_us = esp_timer_get_time();
                s_confirm_state = CONFIRM_WAIT_TRACK;
            } else {
                s_confirm_state = CONFIRM_IDLE; // stop confirmed
            }
        } else if (esp_timer_get_time() - s_action_time_us >= REPLY_TIMEOUT_MS * 1000) {
            on_attempt_failed();
        }
        return;

    case CONFIRM_WAIT_TRACK:
        if (s_rx_frame_ready && s_rx_frame_cmd == MP3_CMD_QUERY_TRACK) {
            uint16_t track = (uint16_t)((s_rx_frame_data[0] << 8) | s_rx_frame_data[1]);
            s_rx_frame_ready = false;
            if (track == s_target_seq_index) {
                s_confirm_state = CONFIRM_IDLE;
            } else {
                on_attempt_failed();
            }
        } else if (esp_timer_get_time() - s_action_time_us >= REPLY_TIMEOUT_MS * 1000) {
            on_attempt_failed();
        }
        return;
    }
}

void mp3_player_init(void)
{
    gpio_config_t busy_conf = {
        .pin_bit_mask = 1ULL << MP3_BUSY_GPIO,
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE, // actively driven by the module
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    gpio_config(&busy_conf);
    debounced_input_init(&s_busy_debounce, gpio_get_level(MP3_BUSY_GPIO));

    uart_config_t uart_conf = {
        .baud_rate = MP3_UART_BAUD,
        .data_bits = UART_DATA_8_BITS,
        .parity = UART_PARITY_DISABLE,
        .stop_bits = UART_STOP_BITS_1,
        .flow_ctrl = UART_HW_FLOWCTRL_DISABLE,
        .source_clk = UART_SCLK_DEFAULT,
    };
    uart_driver_install(MP3_UART_NUM, MP3_UART_RX_BUF_SIZE, 0, 0, NULL, 0);
    uart_param_config(MP3_UART_NUM, &uart_conf);
    uart_set_pin(MP3_UART_NUM, MP3_UART_TX_GPIO, MP3_UART_RX_GPIO, UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE);

    uint8_t volume_data[1] = {MP3_VOLUME_95_PERCENT};
    send_frame(MP3_CMD_SET_VOLUME, volume_data, 1);
}

void mp3_player_update(void)
{
    poll_uart();
    update_busy();
    update_confirm();
}

void mp3_player_play(const char *path, uint16_t seq_index)
{
    strncpy(s_target_path, path, sizeof(s_target_path) - 1);
    s_target_path[sizeof(s_target_path) - 1] = 0;
    s_target_seq_index = seq_index;
    s_target_kind = TARGET_PLAY;
    s_attempts_made = 0;
    // Discard any not-yet-consumed BUSY edge from the previous file - otherwise
    // it's mistaken for this new file's start/end the instant one of the
    // mp3_player_busy_just_*() functions is next polled.
    s_busy_idle_event = false;
    s_busy_playing_event = false;
    send_attempt();
}

void mp3_player_stop(void)
{
    s_confirm_state = CONFIRM_IDLE; // abandon any in-flight confirm, no error registered
    s_target_kind = TARGET_STOP;
    s_attempts_made = 0;
    send_attempt();
}

bool mp3_player_busy_just_went_playing(void)
{
    if (s_busy_playing_event) {
        s_busy_playing_event = false;
        return true;
    }
    return false;
}

bool mp3_player_busy_just_went_idle(void)
{
    if (s_busy_idle_event) {
        s_busy_idle_event = false;
        return true;
    }
    return false;
}

bool mp3_player_last_command_failed(void)
{
    if (s_last_command_failed) {
        s_last_command_failed = false;
        return true;
    }
    return false;
}
