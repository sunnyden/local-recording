#pragma once
#include "../../host/freertos/FreeRTOS.h"
#include <assert.h>
#include <stdatomic.h>
#include <stdint.h>
typedef atomic_flag gui_status_mux_t;
#define portMUX_TYPE gui_status_mux_t
#undef portMUX_INITIALIZER_UNLOCKED
#define portMUX_INITIALIZER_UNLOCKED ATOMIC_FLAG_INIT
static _Thread_local unsigned gui_status_lock_depth;
static inline void gui_status_enter(portMUX_TYPE *lock)
{
    assert(!gui_status_lock_depth);
    while (atomic_flag_test_and_set_explicit(lock, memory_order_acquire)) {}
    ++gui_status_lock_depth;
}
static inline void gui_status_exit(portMUX_TYPE *lock)
{
    assert(gui_status_lock_depth == 1);
    --gui_status_lock_depth;
    atomic_flag_clear_explicit(lock, memory_order_release);
}
#undef portENTER_CRITICAL
#undef portEXIT_CRITICAL
#define portENTER_CRITICAL(lock) gui_status_enter(lock)
#define portEXIT_CRITICAL(lock) gui_status_exit(lock)
