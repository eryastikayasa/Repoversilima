#pragma once

#include "display_engine.h"
#include "display_face.h"
#include "display_text.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Compatibility facade for the existing Repo5 application layer.
 * All calls are routed into the Repo6 display engine APIs. */
static inline void oled_init(void)
{
    display_engine_init();
    display_engine_start();
}

static inline void face_animation_start(void)
{
    display_engine_start();
}

static inline void face_set_state(face_state_t state)
{
    display_face_set_state(state);
}

static inline void face_show_for_ms(face_state_t state, uint32_t duration_ms)
{
    display_face_show_for_ms(state, duration_ms);
}

static inline void display_status(const char *text)
{
    display_set_system_state(FACE_IDLE, text ? text : "");
}

#ifdef __cplusplus
}
#endif
