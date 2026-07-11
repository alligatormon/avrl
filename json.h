#pragma once
#include "value.h"
#include <jansson.h>

json_t *vrl_value_to_json(const vrl_value *v);
vrl_value *vrl_value_from_json(json_t *j);

/* Compact JSON string of a value (caller frees). */
char *vrl_json_encode(const vrl_value *v);
/* Parse JSON into a value. On error returns NULL and sets *err (owned). */
vrl_value *vrl_json_decode(const char *s, size_t len, char **err);
