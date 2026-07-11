#pragma once
#include <stdint.h>

/* Per-subsystem debug verbosity, mirroring amtail's amtail_log_level.
 * 0 = silent, higher = more verbose. */
typedef struct avrl_log_level {
	uint8_t lexer;
	uint8_t parser;
	uint8_t compiler;
	uint8_t vm;      /* interpreter */
	uint8_t stdlib;
	uint8_t pcre;
} avrl_log_level;
