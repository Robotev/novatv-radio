idf_component_register(
    SRCS "main.c" "debounce.c" "sensor.c" "mp3_player.c" "status_led.c" "led_strip_encoder.c"
    INCLUDE_DIRS "."
    PRIV_REQUIRES driver esp_timer
)
