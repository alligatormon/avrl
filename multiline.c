#include "multiline.h"
#include "pcre_wrap.h"
#include <stdlib.h>
#include <string.h>

struct vrl_multiline {
	regex_match *re;
	vrl_multiline_mode mode;
	avrl_log_level ll;
	char *buf;      /* accumulated record */
	size_t len;
	size_t cap;
	int have;       /* buffer holds an in-progress record */
};

int vrl_multiline_mode_from_str(const char *s, vrl_multiline_mode *out)
{
	if (!s)
		return -1;
	if (!strcmp(s, "halt_before")) { *out = VRL_ML_HALT_BEFORE; return 0; }
	if (!strcmp(s, "halt_with")) { *out = VRL_ML_HALT_WITH; return 0; }
	if (!strcmp(s, "continue_through")) { *out = VRL_ML_CONTINUE_THROUGH; return 0; }
	if (!strcmp(s, "continue_past")) { *out = VRL_ML_CONTINUE_PAST; return 0; }
	return -1;
}

vrl_multiline *vrl_multiline_new(const char *pattern, vrl_multiline_mode mode,
				 avrl_log_level ll, char **err)
{
	vrl_multiline *ml = calloc(1, sizeof(*ml));
	if (!ml)
		return NULL;
	ml->re = avrl_regex_compile((char *)pattern);
	if (!ml->re) {
		if (err)
			*err = strdup("multiline: invalid pattern");
		free(ml);
		return NULL;
	}
	ml->mode = mode;
	ml->ll = ll;
	return ml;
}

void vrl_multiline_free(vrl_multiline *ml)
{
	if (!ml)
		return;
	avrl_regex_free(ml->re);
	free(ml->buf);
	free(ml);
}

static void ml_reserve(vrl_multiline *ml, size_t extra)
{
	if (ml->len + extra + 1 > ml->cap) {
		size_t cap = ml->cap ? ml->cap * 2 : 256;
		while (cap < ml->len + extra + 1)
			cap *= 2;
		ml->buf = realloc(ml->buf, cap);
		ml->cap = cap;
	}
}

static void ml_append(vrl_multiline *ml, const char *line, size_t len)
{
	ml_reserve(ml, len + 1);
	if (ml->have && ml->len) {
		ml->buf[ml->len++] = '\n';
	}
	memcpy(ml->buf + ml->len, line, len);
	ml->len += len;
	ml->buf[ml->len] = '\0';
	ml->have = 1;
}

static void ml_emit(vrl_multiline *ml, vrl_line_cb cb, void *ud)
{
	if (!ml->have)
		return;
	cb(ud, ml->buf ? ml->buf : "", ml->len);
	ml->len = 0;
	ml->have = 0;
	if (ml->buf)
		ml->buf[0] = '\0';
}

void vrl_multiline_feed(vrl_multiline *ml, const char *line, size_t len,
			vrl_line_cb cb, void *ud)
{
	int ov[AVRL_OVECCOUNT];
	int matched = avrl_regex_exec(ml->re, line, len, ov, AVRL_OVECCOUNT, ml->ll) > 0;

	switch (ml->mode) {
	case VRL_ML_HALT_BEFORE:
		/* a matching line begins a new record */
		if (matched && ml->have)
			ml_emit(ml, cb, ud);
		ml_append(ml, line, len);
		break;
	case VRL_ML_HALT_WITH:
		/* a matching line ends the current record (inclusive) */
		ml_append(ml, line, len);
		if (matched)
			ml_emit(ml, cb, ud);
		break;
	case VRL_ML_CONTINUE_THROUGH:
		/* keep grouping while lines match; first non-match ends record */
		ml_append(ml, line, len);
		if (!matched)
			ml_emit(ml, cb, ud);
		break;
	case VRL_ML_CONTINUE_PAST:
		/* a matching line means the following line continues this record */
		ml_append(ml, line, len);
		if (!matched)
			ml_emit(ml, cb, ud);
		break;
	}
}

void vrl_multiline_flush(vrl_multiline *ml, vrl_line_cb cb, void *ud)
{
	ml_emit(ml, cb, ud);
}
