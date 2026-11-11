/*
 * ambre_log.hpp — logging macros of the archive process.
 *
 * Every process of the recorder reports through the portability layer
 * (target/port/ambre_port.h): slog2 on QNX 8.0, stderr on the PC, and the
 * AMBRE_LOG=<0..3> filter is honoured in both cases (OBS-01).
 */

#ifndef AMBRE_LOG_HPP
#define AMBRE_LOG_HPP

#define _Static_assert static_assert
extern "C" {
#include "../port/ambre_port.h"
}
#undef _Static_assert

#define log_info(tag, ...)  ambre_port_log(AMBRE_LOG_INFO, (tag), __VA_ARGS__)
#define log_warn(tag, ...)  ambre_port_log(AMBRE_LOG_WARN, (tag), __VA_ARGS__)
#define log_error(tag, ...) ambre_port_log(AMBRE_LOG_ERROR, (tag), __VA_ARGS__)
#define log_debug(tag, ...) ambre_port_log(AMBRE_LOG_DEBUG, (tag), __VA_ARGS__)

#endif /* AMBRE_LOG_HPP */
