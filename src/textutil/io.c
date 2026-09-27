/*
 * textutil - shared output file handling.
 *
 * The writers do not each decide how to report a bad destination.  Before a
 * file is written the destination is checked, because the reference tool
 * distinguishes a missing folder, a folder that may not be written to, and a
 * destination that is already a directory, and words each differently.  The
 * last of those quotes the destination's own name and the name of the folder
 * holding it.
 *
 * The check looks at the destination itself with lstat, so a symbolic link
 * passes even when it points at a directory.  The reference tool replaces such
 * a link with a regular file rather than writing through it, which is a
 * difference this port does not reproduce; see src/textutil/NOTES.md.
 *
 * The RTFD writer skips the check.  Its destination is a bundle directory, so
 * an existing directory is the normal case rather than an error, and a missing
 * parent is created.
 *
 * Copyright (C) 2026, LibreDarwin
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#include "textutil.h"

/* The name after the last slash. */
static const char *
last_component(const char *path)
{
	const char *slash = strrchr(path, '/');

	return slash != NULL ? slash + 1 : path;
}

/* The name of the folder holding path, which is what the "couldn't be saved"
 * wording quotes.  A destination with no slash is held by the working
 * directory, so that is the folder named rather than a bare dot. */
static const char *
holding_folder(const char *path, char *buf, size_t buflen)
{
	const char *slash = strrchr(path, '/');
	size_t n;

	if (slash == NULL) {
		if (getcwd(buf, buflen) == NULL)
			snprintf(buf, buflen, ".");
	} else if (slash == path) {
		snprintf(buf, buflen, "/");
	} else {
		n = (size_t)(slash - path);
		if (n >= buflen)
			n = buflen - 1;
		memcpy(buf, path, n);
		buf[n] = '\0';
	}
	return last_component(buf);
}

int
tu_check_output(const char *path)
{
	char dir[PATH_MAX];
	const char *slash = strrchr(path, '/');
	struct stat st;

	if (slash == NULL) {
		snprintf(dir, sizeof(dir), ".");
	} else if (slash == path) {
		snprintf(dir, sizeof(dir), "/");
	} else {
		size_t n = (size_t)(slash - path);

		if (n >= sizeof(dir))
			n = sizeof(dir) - 1;
		memcpy(dir, path, n);
		dir[n] = '\0';
	}
	/* A folder that is not there at all, and a folder that may not be written
	 * to, each have their own wording. */
	if (stat(dir, &st) != 0)
		return errno == EACCES || errno == EPERM ? TU_WRITE_DENIED :
		    TU_WRITE_NOFOLDER;
	if (access(dir, W_OK) != 0)
		return TU_WRITE_DENIED;
	/* Everything else that stops the write is reported as the file not being
	 * savable in that folder.  That covers a destination that is already a
	 * directory, and a parent that is a file rather than a folder, neither of
	 * which can hold the output. */
	if (!S_ISDIR(st.st_mode))
		return TU_WRITE_ISDIR;
	/* lstat, so that a link to a directory is not mistaken for one. */
	if (lstat(path, &st) == 0 && S_ISDIR(st.st_mode))
		return TU_WRITE_ISDIR;
	return TU_WRITE_OK;
}

void
tu_write_error(const char *path, int why)
{
	char folder[PATH_MAX];
	const char *name;

	switch (why) {
	case TU_WRITE_DENIED:
		fprintf(stderr,
		    "Error writing %s.  You don\xe2\x80\x99t have permission.\n",
		    path);
		break;
	case TU_WRITE_ISDIR:
		name = last_component(path);
		fprintf(stderr, "Error writing %s.  The file "
		    "\xe2\x80\x9c%s\xe2\x80\x9d couldn\xe2\x80\x99t be saved in "
		    "the folder \xe2\x80\x9c%s\xe2\x80\x9d.\n", path, name,
		    holding_folder(path, folder, sizeof(folder)));
		break;
	default:
		fprintf(stderr, "Error writing %s.  The folder doesn\xe2\x80\x99t"
		    " exist.\n", path);
		break;
	}
}

/* The bare diagnostic, for a write that was allowed and then failed. */
void
tu_write_failed(const char *path)
{
	fprintf(stderr, "Error writing %s.\n", path);
}
