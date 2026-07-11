#pragma once
#include "value.h"
#include "parser.h"
#include "interp.h"
#include "multiline.h"
#include "log.h"

/*
 * avrl public API.
 *
 * Typical embedding (mirrors amtail usage in alligator):
 *
 *   vrl_stdlib_init();                              // once
 *   vrl_program *p = vrl_compile(src, len, ll);     // at config push
 *   if (p->err) { ... }
 *   vrl_ctx *ctx = vrl_ctx_new(ll);                 // per stream (reusable)
 *   ...
 *   vrl_ctx_reset(ctx);
 *   vrl_ctx_set_event(ctx, vrl_event_from_message(line, len, source));
 *   vrl_status st = vrl_exec(ctx, p->root);
 *   // ctx->event now holds the transformed event
 */

/* Parse + prepare a program. Returns a program; check ->err (NULL on ok). */
vrl_program *vrl_compile(const char *src, size_t len, avrl_log_level ll);

/* Build a default event object { "message": <line> [, "source_type": src] }. */
vrl_value *vrl_event_from_message(const char *line, size_t len, const char *source);

/* One-shot convenience: run `prog` against `event` (owned/consumed), returning
 * the resulting event (owned) in *out_event. For tests / non-hot paths. */
vrl_status vrl_run_once(vrl_program *prog, vrl_value *event, vrl_value **out_event,
			char **err, avrl_log_level ll);
