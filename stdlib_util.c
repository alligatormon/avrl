#define _GNU_SOURCE
#include "stdlib_internal.h"
#include <stdlib.h>
#include <string.h>

int avrl_arg_str(vrl_call_args *a, const char *name, int idx,
		 const char **data, size_t *len, char **err)
{
	vrl_value *v = vrl_arg(a, name, idx);
	if (!v || v->type != VRL_BYTES) {
		if (err)
			*err = vrl_errf("expected string argument '%s'",
					name ? name : "value");
		return 0;
	}
	*data = v->u.bytes.data;
	*len = v->u.bytes.len;
	return 1;
}

void avrl_buf_init(avrl_buf *b)
{
	b->s = NULL;
	b->len = 0;
	b->cap = 0;
}

void avrl_buf_reserve(avrl_buf *b, size_t extra)
{
	if (b->len + extra + 1 <= b->cap)
		return;
	size_t nc = b->cap ? b->cap : 32;
	while (nc < b->len + extra + 1)
		nc *= 2;
	b->s = realloc(b->s, nc);
	b->cap = nc;
}

void avrl_buf_add(avrl_buf *b, const char *p, size_t n)
{
	if (!n)
		return;
	avrl_buf_reserve(b, n);
	memcpy(b->s + b->len, p, n);
	b->len += n;
}

void avrl_buf_addc(avrl_buf *b, char c)
{
	avrl_buf_reserve(b, 1);
	b->s[b->len++] = c;
}

void avrl_buf_puts(avrl_buf *b, const char *s)
{
	avrl_buf_add(b, s, strlen(s));
}

vrl_value *avrl_buf_to_bytes(avrl_buf *b)
{
	if (!b->s) {
		return vrl_bytes("", 0);
	}
	b->s[b->len] = '\0';
	vrl_value *v = vrl_bytes_take(b->s, b->len);
	b->s = NULL;
	b->len = b->cap = 0;
	return v;
}

void avrl_buf_free(avrl_buf *b)
{
	free(b->s);
	b->s = NULL;
	b->len = b->cap = 0;
}
