#pragma once
#include <stdbool.h>
#include <stdint.h>

void mp3_player_init(void);
void mp3_player_update(void); // call every main-loop tick

// Starts (or restarts, abandoning any in-flight retry) a play-and-confirm
// sequence for the given file. seq_index is the expected 0x0D track number.
void mp3_player_play(const char *path, uint16_t seq_index);

// Abandons any in-flight confirm/retry without registering an error, then
// starts a stop-and-confirm sequence (same retry machinery as PLAY -
// QUERY_PLAY_STATE must actually report "not playing"). Does not start any
// new file; the caller is responsible for that once the rotary switch has
// settled on a position.
void mp3_player_stop(void);

// True once, the first time the BUSY line is read after transitioning
// HIGH->LOW (i.e. a file has actually started playing). Clears on read.
bool mp3_player_busy_just_went_playing(void);

// True once, the first time the BUSY line is read after transitioning
// LOW->HIGH (i.e. a file finished playing on its own). Clears on read.
bool mp3_player_busy_just_went_idle(void);

// True once, after a PLAY or STOP command has failed to be confirmed after
// MAX_ATTEMPTS retries. Clears on read.
bool mp3_player_last_command_failed(void);
