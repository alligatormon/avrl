#pragma once
#include <stddef.h>
#include "log.h"

/*
 * Multiline log record assembler, modelled on Vector's `multiline` source
 * option. Physical lines are fed in one at a time; assembled logical records
 * are delivered via a callback. This is the piece amtail lacks: it lets a
 * single VRL program see a whole multi-line record (stack traces, pretty
 * printed JSON, etc.) as one event.
 */

typedef enum {
	VRL_ML_HALT_BEFORE,     /* a line matching `pattern` starts a new record */
	VRL_ML_HALT_WITH,       /* a line matching `pattern` ends the current record */
	VRL_ML_CONTINUE_THROUGH,/* record continues while lines match; first non-match ends it */
	VRL_ML_CONTINUE_PAST,   /* a matching line means the next line continues this record */
} vrl_multiline_mode;

typedef struct vrl_multiline vrl_multiline;

/* callback receives one assembled record (NOT NUL-terminated guarantee: it is,
 * but use len). */
typedef void (*vrl_line_cb)(void *ud, const char *record, size_t len);

vrl_multiline *vrl_multiline_new(const char *pattern, vrl_multiline_mode mode,
				 avrl_log_level ll, char **err);
void vrl_multiline_free(vrl_multiline *ml);

/* Feed one physical line (without trailing newline). */
void vrl_multiline_feed(vrl_multiline *ml, const char *line, size_t len,
			vrl_line_cb cb, void *ud);
/* Flush any buffered partial record (e.g. on EOF / idle timeout). */
void vrl_multiline_flush(vrl_multiline *ml, vrl_line_cb cb, void *ud);

int vrl_multiline_mode_from_str(const char *s, vrl_multiline_mode *out);
