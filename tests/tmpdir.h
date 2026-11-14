/* SPDX-License-Identifier: MIT */
/* A mkdtemp/mkstemp template under $TMPDIR (or /tmp), so that every suite of `make check` works in
 * the scratch directory its runner gave it (tests/run-suite.sh) and suites can run side by side. */
#ifndef GS_TEST_TMPDIR_H
#define GS_TEST_TMPDIR_H

#include <stdio.h>
#include <stdlib.h>

/* buf = "$TMPDIR/<name>"; <name> ends in XXXXXX. Returns buf. */
static inline char *gs_test_template(char *buf, size_t size, const char *name)
{
	const char *dir = getenv("TMPDIR");

	snprintf(buf, size, "%s/%s", dir && *dir ? dir : "/tmp", name);
	return buf;
}

#endif
