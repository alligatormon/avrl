#include "pcre_wrap.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

regex_match *avrl_regex_compile(char *pattern)
{
	regex_match *re = calloc(1, sizeof(*re));
	if (!re)
		return NULL;
	re->jstack = pcre_jit_stack_alloc(1000, 10000);
	re->pattern = strdup(pattern ? pattern : "");

	int err_off;
	const char *err_str;
	re->regex_compiled = pcre_compile(pattern, 0, &err_str, &err_off, NULL);
	if (!re->regex_compiled) {
		fprintf(stderr, "avrl: could not compile regex '%s': %s\n",
			pattern ? pattern : "", err_str);
		if (re->jstack)
			pcre_jit_stack_free(re->jstack);
		free(re->pattern);
		free(re);
		return NULL;
	}

	re->pcreExtra = pcre_study(re->regex_compiled, PCRE_STUDY_JIT_COMPILE, &err_str);
	if (err_str) {
		fprintf(stderr, "avrl: could not study regex '%s': %s\n",
			pattern ? pattern : "", err_str);
		if (re->jstack)
			pcre_jit_stack_free(re->jstack);
		pcre_free(re->regex_compiled);
		free(re->pattern);
		free(re);
		return NULL;
	}

	if (re->pcreExtra && re->jstack)
		pcre_assign_jit_stack(re->pcreExtra, NULL, re->jstack);

	int nc = 0, esz = 0;
	const unsigned char *ntab = NULL;
	if (pcre_fullinfo(re->regex_compiled, re->pcreExtra, PCRE_INFO_NAMECOUNT, &nc) >= 0 &&
	    pcre_fullinfo(re->regex_compiled, re->pcreExtra, PCRE_INFO_NAMEENTRYSIZE, &esz) >= 0 &&
	    pcre_fullinfo(re->regex_compiled, re->pcreExtra, PCRE_INFO_NAMETABLE, &ntab) >= 0 &&
	    nc > 0 && esz > 0 && ntab) {
		re->pcre_name_count = nc;
		re->pcre_name_entry_size = esz;
		re->pcre_name_table = ntab;
	}

	return re;
}

void avrl_regex_free(regex_match *re)
{
	if (!re)
		return;
	if (re->pattern)
		free(re->pattern);
	if (re->pcreExtra)
		pcre_free_study(re->pcreExtra);
	if (re->regex_compiled)
		pcre_free(re->regex_compiled);
	if (re->jstack)
		pcre_jit_stack_free(re->jstack);
	free(re);
}

int avrl_regex_exec(regex_match *re, const char *subject, size_t subject_len,
		    int *ovector, int ovecsize, avrl_log_level ll)
{
	if (!re || !ovector || ovecsize <= 0)
		return 0;
	int count = pcre_exec(re->regex_compiled, re->pcreExtra, subject,
			      (int)subject_len, 0, 0, ovector, ovecsize);
	if (count < 0) {
		if (count != PCRE_ERROR_NOMATCH && ll.pcre > 0)
			fprintf(stderr, "avrl: pcre_exec error %d for /%s/\n",
				count, re->pattern ? re->pattern : "");
		return 0;
	}
	if (count == 0) /* ovector too small; groups truncated */
		count = ovecsize / 3;
	return count;
}
