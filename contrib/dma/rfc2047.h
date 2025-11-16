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

/*
 * RFC 2047 MIME Header Encoding/Decoding Support
 *
 * This module provides functions to encode and decode email headers
 * containing non-ASCII characters according to RFC 2047.
 *
 * References:
 *   - RFC 2047: MIME Part Three: Message Header Extensions for Non-ASCII Text
 *   - RFC 5322: Internet Message Format
 */

#ifndef RFC2047_H
#define RFC2047_H

#include <sys/types.h>

/*
 * Maximum length of an RFC 2047 encoded-word (per RFC 2047 Section 2)
 * The spec says 75 characters, but we use 76 to account for potential
 * off-by-one issues in various implementations.
 */
#define RFC2047_MAX_ENCODED_WORD_LEN	75

/*
 * Maximum line length for email headers (RFC 5322)
 * SHOULD be no more than 78 characters (excluding CRLF)
 */
#define RFC2047_MAX_LINE_LEN		78

/*
 * Check if a string contains non-ASCII characters that require encoding.
 *
 * Returns:
 *   1 if the string contains non-ASCII characters (bytes with high bit set)
 *   0 if the string is pure ASCII
 */
int needs_rfc2047_encoding(const char *str);

/*
 * Encode a string using RFC 2047 Q-encoding (Quoted-Printable for headers).
 *
 * This encodes non-ASCII bytes as =XX where XX is hexadecimal.
 * Spaces are encoded as underscores.
 * Special characters =, ?, _, and control chars are encoded.
 *
 * Parameters:
 *   str     - The string to encode (UTF-8)
 *   charset - The character set name (typically "UTF-8")
 *
 * Returns:
 *   Newly allocated string containing the encoded-word (=?charset?Q?...?=)
 *   NULL on error
 *
 * Note: The caller must free() the returned string.
 */
char *rfc2047_qencode(const char *str, const char *charset);

/*
 * Encode a header value, splitting into multiple encoded-words if necessary.
 *
 * This function intelligently encodes a header value:
 * - If all ASCII and short enough: returns as-is
 * - If contains non-ASCII: encodes using Q-encoding
 * - If too long: splits into multiple encoded-words
 *
 * Parameters:
 *   header_name - The header field name (e.g., "Subject")
 *   value       - The header value to encode
 *
 * Returns:
 *   Newly allocated string containing the properly encoded and folded value
 *   NULL on error
 *
 * Note: The caller must free() the returned string.
 */
char *rfc2047_encode_header(const char *header_name, const char *value);

/*
 * Fold a long header line according to RFC 5322 rules.
 *
 * This respects RFC 2047 encoded-word boundaries and inserts proper
 * folding (CRLF + whitespace) to keep lines under the maximum length.
 *
 * Parameters:
 *   header_line - The complete header line (e.g., "Subject: foo")
 *
 * Returns:
 *   Newly allocated string with proper folding
 *   NULL on error
 *
 * Note: The caller must free() the returned string.
 */
char *rfc2047_fold_header(const char *header_line);

/*
 * Decode RFC 2047 encoded-words in a header value.
 *
 * This decodes all =?charset?encoding?text?= sequences in the string.
 *
 * Parameters:
 *   header - The header value possibly containing encoded-words
 *
 * Returns:
 *   Newly allocated string with decoded text (UTF-8)
 *   NULL on error
 *
 * Note: The caller must free() the returned string.
 * Note: This is provided for completeness but not required for the fix.
 */
char *rfc2047_decode_header(const char *header);

#endif /* RFC2047_H */
