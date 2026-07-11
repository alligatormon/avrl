#pragma once
#include "common/pcre_parser.h"
#include "log.h"

/*
 * Thin PCRE wrapper for avrl. Reuses alligator's `regex_match` struct (already
 * used by amtail/grok) so no new dependency beyond libpcre, which alligator
 * already links.
 */

#define AVRL_OVECCOUNT 240

regex_match *avrl_regex_compile(char *pattern);
void avrl_regex_free(regex_match *re);

/* Returns capture count (>0 on match, 0 on no-match/error).
 * ovector must have room for ovecsize ints. Group i span is
 * [ovector[2*i], ovector[2*i+1]). */
int avrl_regex_exec(regex_match *re, const char *subject, size_t subject_len,
		    int *ovector, int ovecsize, avrl_log_level ll);
