#ifndef TEXTUTIL_ZIP_H
#define TEXTUTIL_ZIP_H

#include <stddef.h>

/* One member of the archive the docx and odt writers lay out.  name is the
 * member path, data its raw bytes and len its length.  stored marks the
 * mimetype member of odt, which the format insists be held compressed by
 * nothing; every other member is deflated, and must be by the same zlib
 * level and strategy the reference tool uses or the bytes do not come out
 * identical. */
struct tu_zip_mem {
	const char *name;
	const void *data;
	unsigned long len;
	int stored;
};

/* Writes the members in order as an archive at path, reporting nothing on
 * failure; the caller names the destination instead.  The member timestamps
 * are the current local time, as the reference stamps its own, and they are
 * the one part the parity harness cannot hold the output to: the reference
 * and this writer run a breath apart. */
int tu_zip_write(const char *path, const struct tu_zip_mem *mem, size_t n);

#endif