#include "vrl.h"
#include <stdlib.h>
#include <string.h>

vrl_program *vrl_compile(const char *src, size_t len, avrl_log_level ll)
{
	vrl_stdlib_init();
	return vrl_parse(src, len, ll);
}

vrl_value *vrl_event_from_message(const char *line, size_t len, const char *source)
{
	vrl_value *ev = vrl_object_new();
	vrl_object_set_cstr(ev, "message", vrl_bytes(line, len));
	if (source)
		vrl_object_set_cstr(ev, "source_type", vrl_bytes_cstr(source));
	return ev;
}

vrl_status vrl_run_once(vrl_program *prog, vrl_value *event, vrl_value **out_event,
			char **err, avrl_log_level ll)
{
	if (!prog || prog->err) {
		if (err)
			*err = strdup(prog && prog->err ? prog->err : "no program");
		vrl_value_unref(event);
		return VRL_ABORT;
	}
	vrl_ctx *ctx = vrl_ctx_new(ll);
	vrl_ctx_set_event(ctx, event);
	vrl_status st = vrl_exec(ctx, prog->root);
	if (st == VRL_OK && out_event) {
		*out_event = vrl_value_ref(ctx->event);
	} else if (err) {
		if (st == VRL_ABORT && ctx->abort_msg)
			*err = strdup(ctx->abort_msg);
		else if (ctx->error)
			*err = strdup(ctx->error);
	}
	vrl_ctx_free(ctx);
	return st;
}
