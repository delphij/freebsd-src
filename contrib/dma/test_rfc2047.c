/*
 * Test program for RFC 2047 encoding functions
 *
 * Compile with:
 *   cc -o test_rfc2047 test_rfc2047.c rfc2047.c -I.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "rfc2047.h"

static void
test_case(const char *name, const char *header_name, const char *value,
    const char *expected)
{
	char *result;

	printf("Test: %s\n", name);
	printf("  Input: %s: %s\n", header_name, value);

	result = rfc2047_encode_header(header_name, value);
	if (result == NULL) {
		printf("  ERROR: rfc2047_encode_header returned NULL\n");
		return;
	}

	printf("  Output: %s\n", result);

	if (expected != NULL) {
		if (strcmp(result, expected) == 0) {
			printf("  PASS\n");
		} else {
			printf("  FAIL: Expected:\n    %s\n", expected);
		}
	}

	free(result);
	printf("\n");
}

int
main(void)
{
	printf("RFC 2047 Encoding Test Suite\n");
	printf("============================\n\n");

	/* Test 1: Short ASCII subject */
	test_case("Short ASCII subject",
	    "Subject",
	    "Test message",
	    "Subject: Test message");

	/* Test 2: Short subject with non-ASCII */
	test_case("Short subject with Japanese characters",
	    "Subject",
	    "Test テスト",
	    NULL); /* Don't check exact encoding, just verify it works */

	/* Test 3: Long subject with arrow (the actual bug case) */
	test_case("Long subject with arrow (bug case)",
	    "Subject",
	    "git: 1181d23c283c - main - textproc/riffdiff: update 3.4.1 → 3.4.2; update pager dependency to textproc/moor",
	    NULL);

	/* Test 4: Very long ASCII subject */
	test_case("Very long ASCII subject",
	    "Subject",
	    "This is a very long subject line that exceeds the maximum line length of 78 characters and should be folded properly according to RFC 5322 rules",
	    NULL);

	/* Test 5: Subject with emoji */
	test_case("Subject with emoji",
	    "Subject",
	    "Test message with emoji 🎉 and more text",
	    NULL);

	/* Test 6: Multiple non-ASCII words */
	test_case("Multiple non-ASCII words",
	    "Subject",
	    "français español 中文 日本語",
	    NULL);

	/* Test 7: Mixed ASCII and non-ASCII */
	test_case("Mixed ASCII and non-ASCII",
	    "Subject",
	    "Update to version 3.4.1 → 3.4.2",
	    NULL);

	/* Test encoding detection */
	printf("Encoding Detection Tests\n");
	printf("========================\n\n");

	printf("needs_rfc2047_encoding(\"ASCII text\"): %d (expected: 0)\n",
	    needs_rfc2047_encoding("ASCII text"));

	printf("needs_rfc2047_encoding(\"Non-ASCII →\"): %d (expected: 1)\n",
	    needs_rfc2047_encoding("Non-ASCII →"));

	printf("\n");

	/* Test Q-encoding */
	printf("Q-Encoding Tests\n");
	printf("================\n\n");

	char *qencoded = rfc2047_qencode("Test → Test", "UTF-8");
	if (qencoded != NULL) {
		printf("Q-encode \"Test → Test\": %s\n", qencoded);
		free(qencoded);
	}

	qencoded = rfc2047_qencode("Hello World", "UTF-8");
	if (qencoded != NULL) {
		printf("Q-encode \"Hello World\": %s\n", qencoded);
		free(qencoded);
	}

	printf("\n");

	/* Test header folding */
	printf("Header Folding Tests\n");
	printf("====================\n\n");

	char *folded = rfc2047_fold_header("Subject: This is a test");
	if (folded != NULL) {
		printf("Fold \"Subject: This is a test\":\n%s\n", folded);
		free(folded);
	}

	folded = rfc2047_fold_header("Subject: git: 1181d23c283c - main - textproc/riffdiff: update 3.4.1 → 3.4.2; update pager dependency to textproc/moor");
	if (folded != NULL) {
		printf("\nFold long subject with arrow:\n%s\n", folded);
		free(folded);
	}

	printf("\nAll tests completed!\n");

	return 0;
}
