#pragma once

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/** Initialize AudioEngine and the WakeWord pipeline. */
bool audio_engine_init(void);

/** Start microphone capture for WakeWord processing. */
bool audio_engine_start_wakeword(void);

/** Stop microphone capture used by WakeWord processing. */
void audio_engine_stop_wakeword(void);

/**
 * Read one PCM16 block from Audio HAL and feed it to WakeNet.
 * Returns true when the wake word is detected.
 */
bool audio_engine_process_wakeword(void);

/** Return true when the latest processing detected the wake word. */
bool audio_engine_wakeword_detected(void);

/** Clear the current wake-word detection flag. */
void audio_engine_clear_wakeword(void);

/** Stop the current AudioEngine capture path. */
void audio_engine_stop(void);

#ifdef __cplusplus
}
#endif
