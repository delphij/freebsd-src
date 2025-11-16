/*
 * Copyright (c) 2025 FreeBSD Project
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 *
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in
 *    the documentation and/or other materials provided with the
 *    distribution.
 * 3. Neither the name of The FreeBSD Project nor the names of its
 *    contributors may be used to endorse or promote products derived
 *    from this software without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
 * ``AS IS'' AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
 * LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS
 * FOR A PARTICULAR PURPOSE ARE DISCLAIMED.  IN NO EVENT SHALL THE
 * COPYRIGHT HOLDERS OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT,
 * INCIDENTAL, SPECIAL, EXEMPLARY OR CONSEQUENTIAL DAMAGES (INCLUDING,
 * BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES;
 * LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED
 * AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY,
 * OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT
 * OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF
 * SUCH DAMAGE.
 */

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/param.h>

#include "rfc2047.h"

/* Characters that must be encoded in Q-encoding */
static int
must_encode_qp(unsigned char c)
{
	/* Control characters and DEL */
	if (c < 32 || c == 127)
		return 1;

	/* Special RFC 2047 characters */
	if (c == '=' || c == '?' || c == '_')
		return 1;

	/* Non-ASCII (high bit set) */
	if (c > 127)
		return 1;

	return 0;
}

int
needs_rfc2047_encoding(const char *str)
{
	const unsigned char *p = (const unsigned char *)str;

	while (*p) {
		if (*p > 127) /* Non-ASCII character */
			return 1;
		p++;
	}
	return 0;
}

char *
rfc2047_qencode(const char *str, const char *charset)
{
	const unsigned char *p = (const unsigned char *)str;
	char *result, *out;
	size_t result_len;
	int prefix_len;

	if (str == NULL || charset == NULL)
		return NULL;

	/* Calculate maximum possible size */
	/* Format: =?charset?Q?encoded_text?= */
	prefix_len = strlen(charset) + 7; /* =??Q??= + NUL */
	result_len = prefix_len + (strlen(str) * 3) + 1;

	result = malloc(result_len);
	if (result == NULL)
		return NULL;

	/* Write prefix: =?charset?Q? */
	out = result;
	out += sprintf(out, "=?%s?Q?", charset);

	/* Encode the string */
	while (*p) {
		if (*p == ' ') {
			/* Space becomes underscore */
			*out++ = '_';
		} else if (must_encode_qp(*p)) {
			/* Encode as =XX */
			sprintf(out, "=%02X", *p);
			out += 3;
		} else {
			/* Pass through as-is */
			*out++ = *p;
		}
		p++;
	}

	/* Write suffix: ?= */
	*out++ = '?';
	*out++ = '=';
	*out = '\0';

	return result;
}

/*
 * Split a string into words at spaces, encoding each word separately.
 * This allows for more efficient encoding and better line breaking.
 */
static char *
encode_with_word_splitting(const char *str, const char *charset,
    const char *header_name)
{
	char *result = NULL;
	char *word, *encoded_word;
	const char *p, *word_start;
	size_t result_size = 0, result_used = 0;
	size_t header_name_len = strlen(header_name);
	size_t current_line_len = header_name_len + 2; /* "Header: " */
	int first_word = 1;
	int needs_encoding;

	result_size = strlen(str) * 4 + 1024; /* Initial allocation */
	result = malloc(result_size);
	if (result == NULL)
		return NULL;
	result[0] = '\0';

	p = str;
	while (*p) {
		/* Skip leading spaces */
		while (*p == ' ' || *p == '\t')
			p++;

		if (*p == '\0')
			break;

		/* Find end of word */
		word_start = p;
		while (*p && *p != ' ' && *p != '\t')
			p++;

		/* Extract word */
		word = strndup(word_start, p - word_start);
		if (word == NULL) {
			free(result);
			return NULL;
		}

		/* Check if this word needs encoding */
		needs_encoding = needs_rfc2047_encoding(word);

		if (needs_encoding) {
			encoded_word = rfc2047_qencode(word, charset);
			if (encoded_word == NULL) {
				free(word);
				free(result);
				return NULL;
			}
		} else {
			encoded_word = word;
			word = NULL; /* Don't free twice */
		}

		/* Check if we need to fold */
		if (!first_word &&
		    current_line_len + strlen(encoded_word) + 1 > RFC2047_MAX_LINE_LEN) {
			/* Fold here: add CRLF + space */
			if (result_used + 3 >= result_size) {
				result_size *= 2;
				result = realloc(result, result_size);
				if (result == NULL) {
					free(encoded_word);
					free(word);
					return NULL;
				}
			}
			strcat(result, "\n ");
			result_used += 2;
			current_line_len = 1; /* Space after fold */
		}

		/* Add space before word (except first) */
		if (!first_word) {
			if (result_used + 1 >= result_size) {
				result_size *= 2;
				result = realloc(result, result_size);
				if (result == NULL) {
					free(encoded_word);
					free(word);
					return NULL;
				}
			}
			strcat(result, " ");
			result_used += 1;
			current_line_len += 1;
		}

		/* Add the encoded word */
		if (result_used + strlen(encoded_word) + 1 >= result_size) {
			result_size *= 2;
			result = realloc(result, result_size);
			if (result == NULL) {
				free(encoded_word);
				free(word);
				return NULL;
			}
		}
		strcat(result, encoded_word);
		result_used += strlen(encoded_word);
		current_line_len += strlen(encoded_word);

		if (needs_encoding)
			free(encoded_word);
		else
			free(word);

		first_word = 0;
	}

	return result;
}

char *
rfc2047_encode_header(const char *header_name, const char *value)
{
	char *encoded;
	size_t header_name_len, value_len, total_len;

	if (header_name == NULL || value == NULL)
		return NULL;

	header_name_len = strlen(header_name);
	value_len = strlen(value);

	/* If value is all ASCII and fits on one line, return as-is */
	if (!needs_rfc2047_encoding(value) &&
	    header_name_len + 2 + value_len <= RFC2047_MAX_LINE_LEN) {
		total_len = header_name_len + value_len + 3; /* ": " + NUL */
		encoded = malloc(total_len);
		if (encoded == NULL)
			return NULL;
		snprintf(encoded, total_len, "%s: %s", header_name, value);
		return encoded;
	}

	/* Need encoding or folding */
	encoded = encode_with_word_splitting(value, "UTF-8", header_name);
	if (encoded == NULL)
		return NULL;

	/* Prepend header name */
	total_len = header_name_len + strlen(encoded) + 3;
	char *full_header = malloc(total_len);
	if (full_header == NULL) {
		free(encoded);
		return NULL;
	}
	snprintf(full_header, total_len, "%s: %s", header_name, encoded);
	free(encoded);

	return full_header;
}

char *
rfc2047_fold_header(const char *header_line)
{
	const char *colon;
	char *header_name, *value, *result;

	if (header_line == NULL)
		return NULL;

	/* Find the colon separating header name from value */
	colon = strchr(header_line, ':');
	if (colon == NULL)
		return strdup(header_line); /* Not a valid header, return as-is */

	/* Extract header name */
	header_name = strndup(header_line, colon - header_line);
	if (header_name == NULL)
		return NULL;

	/* Extract value (skip ': ' or ':') */
	value = strdup(colon + 1);
	if (value == NULL) {
		free(header_name);
		return NULL;
	}

	/* Trim leading whitespace from value */
	char *value_start = value;
	while (*value_start == ' ' || *value_start == '\t')
		value_start++;

	/* Encode the header */
	result = rfc2047_encode_header(header_name, value_start);

	free(header_name);
	free(value);

	return result;
}

/*
 * Decode a single RFC 2047 encoded-word.
 * This is a simplified implementation supporting only Q-encoding.
 */
static char *
decode_encoded_word(const char *encoded_word)
{
	const char *p, *q_start, *text_start, *text_end;
	char *charset, *result, *out;
	size_t text_len, result_len;
	unsigned int hex_val;

	/* Format: =?charset?Q?encoded_text?= */
	if (strncmp(encoded_word, "=?", 2) != 0)
		return NULL;

	p = encoded_word + 2;

	/* Find charset */
	q_start = strchr(p, '?');
	if (q_start == NULL)
		return NULL;

	charset = strndup(p, q_start - p);
	if (charset == NULL)
		return NULL;

	/* Check encoding (must be 'Q' or 'q') */
	p = q_start + 1;
	if ((*p != 'Q' && *p != 'q') || *(p + 1) != '?') {
		free(charset);
		return NULL;
	}

	p += 2; /* Skip Q? */
	text_start = p;

	/* Find end of encoded text */
	text_end = strstr(p, "?=");
	if (text_end == NULL) {
		free(charset);
		return NULL;
	}

	text_len = text_end - text_start;
	result_len = text_len + 1; /* Worst case: same length */

	result = malloc(result_len);
	if (result == NULL) {
		free(charset);
		return NULL;
	}

	/* Decode Q-encoding */
	out = result;
	p = text_start;
	while (p < text_end) {
		if (*p == '_') {
			/* Underscore becomes space */
			*out++ = ' ';
			p++;
		} else if (*p == '=' && p + 2 < text_end) {
			/* =XX becomes byte with value XX */
			if (sscanf(p + 1, "%02X", &hex_val) == 1) {
				*out++ = (char)hex_val;
				p += 3;
			} else {
				/* Invalid encoding, copy as-is */
				*out++ = *p++;
			}
		} else {
			/* Copy as-is */
			*out++ = *p++;
		}
	}
	*out = '\0';

	free(charset);
	return result;
}

char *
rfc2047_decode_header(const char *header)
{
	char *result, *new_result;
	const char *p, *encoded_start, *encoded_end;
	char *decoded_word;
	size_t result_size, result_used;

	if (header == NULL)
		return NULL;

	/* Check if there are any encoded words */
	if (strstr(header, "=?") == NULL)
		return strdup(header);

	result_size = strlen(header) + 1024;
	result = malloc(result_size);
	if (result == NULL)
		return NULL;
	result[0] = '\0';
	result_used = 0;

	p = header;
	while (*p) {
		/* Find next encoded word */
		encoded_start = strstr(p, "=?");
		if (encoded_start == NULL) {
			/* No more encoded words, copy rest */
			if (result_used + strlen(p) + 1 >= result_size) {
				result_size = result_used + strlen(p) + 1024;
				new_result = realloc(result, result_size);
				if (new_result == NULL) {
					free(result);
					return NULL;
				}
				result = new_result;
			}
			strcat(result, p);
			break;
		}

		/* Copy everything before encoded word */
		if (encoded_start > p) {
			size_t len = encoded_start - p;
			if (result_used + len + 1 >= result_size) {
				result_size = result_used + len + 1024;
				new_result = realloc(result, result_size);
				if (new_result == NULL) {
					free(result);
					return NULL;
				}
				result = new_result;
			}
			strncat(result, p, len);
			result_used += len;
		}

		/* Find end of encoded word */
		encoded_end = strstr(encoded_start, "?=");
		if (encoded_end == NULL) {
			/* Malformed, copy as-is */
			if (result_used + strlen(encoded_start) + 1 >= result_size) {
				result_size = result_used + strlen(encoded_start) + 1024;
				new_result = realloc(result, result_size);
				if (new_result == NULL) {
					free(result);
					return NULL;
				}
				result = new_result;
			}
			strcat(result, encoded_start);
			break;
		}

		/* Extract and decode the encoded word */
		size_t encoded_len = encoded_end - encoded_start + 2; /* Include ?= */
		char *encoded_word = strndup(encoded_start, encoded_len);
		if (encoded_word == NULL) {
			free(result);
			return NULL;
		}

		decoded_word = decode_encoded_word(encoded_word);
		free(encoded_word);

		if (decoded_word != NULL) {
			if (result_used + strlen(decoded_word) + 1 >= result_size) {
				result_size = result_used + strlen(decoded_word) + 1024;
				new_result = realloc(result, result_size);
				if (new_result == NULL) {
					free(decoded_word);
					free(result);
					return NULL;
				}
				result = new_result;
			}
			strcat(result, decoded_word);
			result_used += strlen(decoded_word);
			free(decoded_word);
		}

		p = encoded_end + 2; /* Skip ?= */
	}

	return result;
}
