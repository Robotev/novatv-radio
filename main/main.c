#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "mp3_player.h"
#include "sensor.h"
#include "status_led.h"

static const char *TAG = "main";

// The only file on the module. QUERY_TRACK reports a file's 1-based position
// in flash storage order, so a lone file is always track 1.
#define TRACK_PATH "/00000*MP3"
#define TRACK_SEQ_INDEX 1

#define ARRIVE_MS 2000 // object in range straight for this long before playing
#define GRACE_MS 4000  // keep playing this long after the object leaves
#define GAP_MS 3000    // minimum silence after the file ends before replaying
#define ERROR_LED_MS 1000

#define LOOP_MS 10

typedef enum {
    STATE_IDLE,    // silent, waiting for an object to arrive
    STATE_PLAYING, // file playing, object in range
    STATE_GRACE,   // file playing, object has left - stops at grace deadline
    STATE_GAP,     // file ended with object in range - replays after the gap
    STATE_ERROR,   // MP3 module retries exhausted - red, then back to idle
} state_t;

typedef enum {
    LED_NONE, // nothing set yet
    LED_IDLE,
    LED_ARRIVING,
    LED_PLAYING,
    LED_WAITING,
    LED_ERROR,
} led_mode_t;

static led_mode_t s_led_mode = LED_NONE;

// Only touches the LED on a mode change, so blink phases aren't restarted
// every tick.
static void set_led(led_mode_t mode)
{
    if (mode == s_led_mode) {
        return;
    }
    s_led_mode = mode;
    switch (mode) {
    case LED_NONE:
        break;
    case LED_IDLE:
        status_led_set_pattern(LED_COLOR_YELLOW, 25, 500);
        break;
    case LED_ARRIVING:
        status_led_set_pattern(LED_COLOR_YELLOW, 0, 0);
        break;
    case LED_PLAYING:
        status_led_set_pattern(LED_COLOR_BLUE, 50, 500);
        break;
    case LED_WAITING:
        status_led_set_pattern(LED_COLOR_BLUE, 0, 0);
        break;
    case LED_ERROR:
        status_led_set_pattern(LED_COLOR_RED, 0, 0);
        break;
    }
}

// True once the sensor has seen an object without interruption for ARRIVE_MS,
// counting no earlier than not_before_us.
static bool arrived(int64_t now, int64_t not_before_us)
{
    int64_t since = sensor_raw_present_since();
    if (since < 0) {
        return false;
    }
    if (since < not_before_us) {
        since = not_before_us;
    }
    return now - since >= (int64_t)ARRIVE_MS * 1000;
}

void app_main(void)
{
    sensor_init();
    mp3_player_init();
    status_led_init();

    state_t state = STATE_IDLE;
    int64_t deadline_us = 0;
    // After an error, an object still in range must be seen for a fresh
    // ARRIVE_MS rather than replaying the instant the red LED ends.
    int64_t arrive_not_before_us = 0;

    while (1) {
        int64_t now = esp_timer_get_time();
        sensor_update(now);
        mp3_player_update();
        status_led_update();

        // A failed STOP is reported after the state has already moved to
        // idle, so check in every state but ERROR itself.
        if (state != STATE_ERROR && mp3_player_last_command_failed()) {
            ESP_LOGE(TAG, "MP3 module communication failed after all retries");
            state = STATE_ERROR;
            deadline_us = now + ERROR_LED_MS * 1000;
        }

        switch (state) {
        case STATE_IDLE:
            if (arrived(now, arrive_not_before_us)) {
                ESP_LOGI(TAG, "Object arrived, starting playback");
                mp3_player_play(TRACK_PATH, TRACK_SEQ_INDEX);
                state = STATE_PLAYING;
            }
            break;

        case STATE_PLAYING:
            if (mp3_player_busy_just_went_idle()) {
                ESP_LOGI(TAG, "Playback finished, object in range - waiting %dms", GAP_MS);
                state = STATE_GAP;
                deadline_us = now + GAP_MS * 1000;
            } else if (!sensor_occupied()) {
                ESP_LOGI(TAG, "Object left - stopping in %dms unless it returns", GRACE_MS);
                state = STATE_GRACE;
                deadline_us = now + GRACE_MS * 1000;
            }
            break;

        case STATE_GRACE:
            if (mp3_player_busy_just_went_idle()) {
                ESP_LOGI(TAG, "Playback finished during grace period");
                state = STATE_IDLE;
            } else if (sensor_occupied()) {
                ESP_LOGI(TAG, "Object returned, continuing playback");
                state = STATE_PLAYING;
            } else if (now >= deadline_us) {
                ESP_LOGI(TAG, "Grace period over, stopping playback");
                mp3_player_stop();
                state = STATE_IDLE;
            }
            break;

        case STATE_GAP:
            // Leaving and returning during the gap is a new arrival, but it
            // never plays before the gap is over - STATE_IDLE then plays as
            // soon as the object has also been in range for ARRIVE_MS.
            if (now >= deadline_us) {
                arrive_not_before_us = 0;
                state = STATE_IDLE;
            }
            break;

        case STATE_ERROR:
            if (now >= deadline_us) {
                arrive_not_before_us = now;
                state = STATE_IDLE;
            }
            break;
        }

        switch (state) {
        case STATE_IDLE:
            set_led(sensor_occupied() ? LED_ARRIVING : LED_IDLE);
            break;
        case STATE_PLAYING:
            set_led(LED_PLAYING);
            break;
        case STATE_GRACE:
            set_led(LED_WAITING);
            break;
        case STATE_GAP:
            // An object that left and came back during the gap shows as
            // arriving until it has been in range for ARRIVE_MS.
            set_led(sensor_occupied() && !arrived(now, 0) ? LED_ARRIVING : LED_WAITING);
            break;
        case STATE_ERROR:
            set_led(LED_ERROR);
            break;
        }

        vTaskDelay(pdMS_TO_TICKS(LOOP_MS));
    }
}
