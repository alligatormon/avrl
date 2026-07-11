#define _GNU_SOURCE
#include "json.h"
#include <stdlib.h>
#include <string.h>
#include <time.h>

json_t *vrl_value_to_json(const vrl_value *v)
{
	if (!v)
		return json_null();
	switch (v->type) {
	case VRL_NULL:    return json_null();
	case VRL_BOOLEAN: return json_boolean(v->u.boolean);
	case VRL_INTEGER: return json_integer(v->u.integer);
	case VRL_FLOAT:   return json_real(v->u.flt);
	case VRL_BYTES:   return json_stringn(v->u.bytes.data, v->u.bytes.len);
	case VRL_TIMESTAMP: {
		time_t secs = (time_t)v->u.timestamp;
		struct tm tmv;
		gmtime_r(&secs, &tmv);
		char ts[40];
		strftime(ts, sizeof(ts), "%Y-%m-%dT%H:%M:%SZ", &tmv);
		return json_string(ts);
	}
	case VRL_REGEX:
		return json_string(v->u.regex && v->u.regex->pattern ? v->u.regex->pattern : "");
	case VRL_ARRAY: {
		json_t *arr = json_array();
		for (size_t i = 0; i < v->u.array.len; i++)
			json_array_append_new(arr, vrl_value_to_json(v->u.array.items[i]));
		return arr;
	}
	case VRL_OBJECT: {
		json_t *obj = json_object();
		for (size_t i = 0; i < v->u.object.len; i++)
			json_object_set_new(obj, v->u.object.entries[i].key,
					    vrl_value_to_json(v->u.object.entries[i].val));
		return obj;
	}
	}
	return json_null();
}

vrl_value *vrl_value_from_json(json_t *j)
{
	if (!j)
		return vrl_null();
	switch (json_typeof(j)) {
	case JSON_NULL:    return vrl_null();
	case JSON_TRUE:    return vrl_boolean(1);
	case JSON_FALSE:   return vrl_boolean(0);
	case JSON_INTEGER: return vrl_integer(json_integer_value(j));
	case JSON_REAL:    return vrl_float(json_real_value(j));
	case JSON_STRING:  return vrl_bytes(json_string_value(j), json_string_length(j));
	case JSON_ARRAY: {
		vrl_value *arr = vrl_array_new();
		size_t idx;
		json_t *val;
		json_array_foreach(j, idx, val)
			vrl_array_push(arr, vrl_value_from_json(val));
		return arr;
	}
	case JSON_OBJECT: {
		vrl_value *obj = vrl_object_new();
		const char *key;
		json_t *val;
		json_object_foreach(j, key, val)
			vrl_object_set_cstr(obj, key, vrl_value_from_json(val));
		return obj;
	}
	}
	return vrl_null();
}

char *vrl_json_encode(const vrl_value *v)
{
	json_t *j = vrl_value_to_json(v);
	char *s = json_dumps(j, JSON_COMPACT | JSON_ENCODE_ANY);
	json_decref(j);
	return s;
}

vrl_value *vrl_json_decode(const char *s, size_t len, char **err)
{
	json_error_t jerr;
	json_t *j = json_loadb(s, len, 0, &jerr);
	if (!j) {
		if (err) {
			char buf[256];
			snprintf(buf, sizeof(buf), "%s (line %d)", jerr.text, jerr.line);
			*err = strdup(buf);
		}
		return NULL;
	}
	vrl_value *v = vrl_value_from_json(j);
	json_decref(j);
	return v;
}
