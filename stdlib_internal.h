#pragma once
#include "interp.h"
#include "pcre_wrap.h"
#include <stdlib.h>
#include <string.h>

/*
 * Shared helpers for the split stdlib_*.c modules.
 * Keep these dependency-free (libc only) except pcre_wrap (already a dep).
 */

/* Convenience: fetch a string argument. Returns 1 on success. */
int avrl_arg_str(vrl_call_args *a, const char *name, int idx,
		 const char **data, size_t *len, char **err);

/* --- dynamic byte buffer --- */
typedef struct {
	char *s;
	size_t len;
	size_t cap;
} avrl_buf;

void avrl_buf_init(avrl_buf *b);
void avrl_buf_reserve(avrl_buf *b, size_t extra);
void avrl_buf_add(avrl_buf *b, const char *p, size_t n);
void avrl_buf_addc(avrl_buf *b, char c);
void avrl_buf_puts(avrl_buf *b, const char *s);
/* Consume the buffer into a bytes value (buffer becomes empty). */
vrl_value *avrl_buf_to_bytes(avrl_buf *b);
void avrl_buf_free(avrl_buf *b);

/* Return a NUL-terminated copy of a value that must be bytes; NULL if not. */
/* (helpers kept minimal on purpose) */

/* --- module registration entry points --- */
void vrl_reg_type(void);
void vrl_reg_string(void);
void vrl_reg_collection(void);
void vrl_reg_codec(void);
void vrl_reg_number(void);
void vrl_reg_random(void);
void vrl_reg_path(void);
void vrl_reg_parse(void);
#ifdef AVRL_WITH_OPENSSL
void vrl_reg_crypto(void);
#endif
