#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif
typedef enum { AUDIO_ENGINE_IDLE=0, AUDIO_ENGINE_LISTENING, AUDIO_ENGINE_PLAYING, AUDIO_ENGINE_ERROR } audio_engine_state_t;
typedef void (*audio_engine_mic_frame_cb_t)(const uint8_t *pcm, size_t len, void *ctx);
bool audio_engine_init(void);
audio_engine_state_t audio_engine_get_state(void);
const char *audio_engine_state_name(audio_engine_state_t state);
bool audio_engine_set_mic_listener(audio_engine_mic_frame_cb_t cb, void *ctx);
bool audio_engine_set_mic_sink(audio_engine_mic_frame_cb_t cb, void *ctx);
bool audio_engine_start_capture(void);
void audio_engine_stop_capture(void);
bool audio_engine_capture_active(void);
bool audio_engine_push_model_audio(const uint8_t *pcm, size_t len, uint32_t generation);
bool audio_engine_push_model_audio_base64(const char *b64, size_t len, uint32_t generation);
void audio_engine_start_input_session(void);
void audio_engine_stop_input_session(void);
bool audio_engine_input_session_active(void);
#ifdef __cplusplus
}
#endif
