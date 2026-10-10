#pragma once
#include <stdbool.h>
#include <stdint.h>

typedef enum {
    LED_COLOR_OFF,
    LED_COLOR_YELLOW,
    LED_COLOR_BLUE,
    LED_COLOR_RED,
} led_color_t;

typedef struct {
    led_color_t color;
    uint32_t duration_ms;
} led_step_t;

void status_led_init(void);

// Sets the LED to blink at the given on/off durations, or solid if on_ms
// is 0 (off_ms is then ignored). Takes effect immediately (starts ON).
void status_led_set_pattern(led_color_t color, uint32_t on_ms, uint32_t off_ms);

// Sets the LED to loop indefinitely through the given steps, starting at
// step 0. steps is copied internally (up to 16 steps), so the caller's
// array need not outlive the call.
void status_led_set_sequence(const led_step_t *steps, uint8_t count);

// Bypasses the blink/sequence engines to set a fixed color immediately.
void status_led_set_immediate(led_color_t color, bool on);

// Advances any active pattern/sequence. No-op in immediate mode. Call
// every main-loop tick.
void status_led_update(void);
