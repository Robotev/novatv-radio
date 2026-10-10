#pragma once
#include <stdbool.h>
#include <stdint.h>

void sensor_init(void);
void sensor_update(int64_t now); // call every main-loop tick

// True while an object is in range (output HIGH), with a short debounce in
// both directions. Used for leaving/returning while audio is playing.
bool sensor_occupied(void);

// Time the raw output went HIGH (object in range) and has stayed HIGH
// without a single LOW reading since, or -1 while LOW. Used for the 2s
// "in range straight" arrival rule, which deliberately ignores the short
// debounce above.
int64_t sensor_raw_present_since(void);
