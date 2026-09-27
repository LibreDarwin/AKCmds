/*
 * textutil - RTFD output.
 *
 * An RTFD file is a directory, not a file: a wrapper holding one RTF member
 * plus, in general, any attachments.  This port only ever produces plain text,
 * so it writes the single RTF member and nothing else.
 *
 * The member is always named TXT.rtf, whatever the input was called.  The name
 * describes the member's content, not the file it came from, so a.txt, z.txt
 * and a file with no extension at all all give a bundle containing TXT.rtf.
 *
 * The directory is created here rather than by the RTF writer, and built by
 * writing the member into it in one step.  That matters: -info reports a
 * bundle's Size as the directory's own st_size, which the filesystem derives
 * from how the directory was populated, so a bundle assembled some other way
 * can report a different size for identical contents.
 *
 * Copyright (C) 2026, LibreDarwin
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "textutil.h"

#define RTFD_MEMBER "TXT.rtf"

/* Create every folder of the bundle's path, so that a destination whose parent
 * is absent is made rather than refused.  The reference tool accepts that, so
 * the check the file writers share does not apply here. */
static int
make_parents(const char *path)
{
	char buf[1024];
	char *p;

	if (snprintf(buf, sizeof(buf), "%s", path) >= (int)sizeof(buf))
		return -1;
	/* Stop at the last slash; the bundle directory itself is not a parent. */
	p = strrchr(buf, '/');
	if (p == NULL)
		return 0;
	*p = '\0';
	if (buf[0] == '\0')
		return 0;		/* the bundle sits in the root */
	for (p = buf + 1; *p != '\0'; p++) {
		if (*p != '/')
			continue;
		*p = '\0';
		if (mkdir(buf, 0755) != 0 && errno != EEXIST)
			return -1;
		*p = '/';
	}
	if (mkdir(buf, 0755) != 0 && errno != EEXIST)
		return -1;
	return 0;
}

int
tu_write_rtfd(const tu_doc_t *d, const char *path,
    const tu_style_t *st, const tu_meta_t *meta)
{
	char full[1024];
	struct stat sb;
	int rc;

	/* An existing directory is the normal case, and is used as it stands.
	 * Anything else in the way is cleared: the reference tool replaces a
	 * plain file at the destination with the bundle. */
	if (stat(path, &sb) != 0) {
		if (make_parents(path) != 0 || mkdir(path, 0755) != 0) {
			tu_write_failed(path);
			return -1;
		}
	} else if (!S_ISDIR(sb.st_mode)) {
		if (unlink(path) != 0 || mkdir(path, 0755) != 0) {
			tu_write_failed(path);
			return -1;
		}
	}
	if (snprintf(full, sizeof(full), "%s/%s", path, RTFD_MEMBER) >=
	    (int)sizeof(full)) {
		tu_write_failed(path);
		rmdir(path);
		return -1;
	}
	if ((rc = tu_write_rtf(d, full, st, meta)) != 0)
		rmdir(path);
	return rc;
}
