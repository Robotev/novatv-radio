#include "sensor.h"

#include "debounce.h"
#include "driver/gpio.h"

// Sharp GP2Y0D02YK0F digital output - HIGH while an object is in range. The
// output is open-collector: it sinks LOW with no object and releases the
// line with one. The datasheet's block diagram shows an external 12k
// pull-up to VCC; the internal pull-up stands in for it, pulling to 3.3V
// rather than the sensor's 5V so the pin stays within its rating.
#define SENSOR_GPIO GPIO_NUM_9
#define SENSOR_PRESENT_LEVEL 1
#define SENSOR_DEBOUNCE_US 50000

static debounced_input_t s_debounce;
static int64_t s_raw_present_since = -1;

void sensor_init(void)
{
    gpio_config_t conf = {
        .pin_bit_mask = 1ULL << SENSOR_GPIO,
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    gpio_config(&conf);
    // Start "absent" so an object present at boot still has to arrive.
    debounced_input_init(&s_debounce, !SENSOR_PRESENT_LEVEL);
}

void sensor_update(int64_t now)
{
    int raw = gpio_get_level(SENSOR_GPIO);
    if (raw == SENSOR_PRESENT_LEVEL) {
        if (s_raw_present_since < 0) {
            s_raw_present_since = now;
        }
    } else {
        s_raw_present_since = -1;
    }

    int level;
    debounced_input_update(&s_debounce, raw, now, SENSOR_DEBOUNCE_US, &level);
}

bool sensor_occupied(void)
{
    return s_debounce.stable_level == SENSOR_PRESENT_LEVEL;
}

int64_t sensor_raw_present_since(void)
{
    return s_raw_present_since;
}
