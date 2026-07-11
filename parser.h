#pragma once
#include "ast.h"
#include "log.h"

typedef struct vrl_program {
	vrl_ast *root;      /* AST_BLOCK of top-level statements */
	char *err;          /* NULL on success (owned) */
	uint32_t err_line;
} vrl_program;

vrl_program *vrl_parse(const char *src, size_t src_len, avrl_log_level ll);
void vrl_program_free(vrl_program *p);

/* Parse a timestamp string (RFC3339 subset) into unix seconds.
 * Returns 0 on success, -1 on failure. Exposed for the stdlib. */
int vrl_parse_timestamp_str(const char *s, size_t len, double *out);
