#include "zip.h"

#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <zlib.h>

/* A member readied for writing: the crc, the compressed size and the
 * compressed bytes.  The local header offset is filled in as the archive is
 * laid down. */
struct ent {
	uLong crc;
	uLong csize;
	uLong usize;
	uLong lho;
	unsigned char *comp;
};

/* Deflate data at level 6 with no zlib wrapper, the same negotiation the
 * reference tool reaches with the same library, so the compressed bytes come
 * out identical.  Returns -1 and leaves *out untouched on failure. */
static int
deflate_mem(const void *data, uLong len, unsigned char **out, uLong *outlen)
{
	z_stream z;
	uLong bound = compressBound(len);
	int rc;

	memset(&z, 0, sizeof z);
	if (deflateInit2(&z, 6, Z_DEFLATED, -15, 8, Z_DEFAULT_STRATEGY) != Z_OK)
		return -1;
	*out = malloc(bound);
	if (*out == NULL) {
		deflateEnd(&z);
		return -1;
	}
	z.next_in = (Bytef *)data;
	z.avail_in = len;
	z.next_out = *out;
	z.avail_out = bound;
	rc = deflate(&z, Z_FINISH);
	*outlen = z.total_out;
	deflateEnd(&z);
	if (rc != Z_STREAM_END) {
		free(*out);
		*out = NULL;
		return -1;
	}
	return 0;
}

/* Write one little-endian 32-bit word. */
static void
w32(FILE *fp, uLong v)
{
	unsigned char b[4];

	b[0] = v & 0xff;
	b[1] = (v >> 8) & 0xff;
	b[2] = (v >> 16) & 0xff;
	b[3] = (v >> 24) & 0xff;
	fwrite(b, 1, sizeof b, fp);
}

/* And one little-endian 16-bit word. */
static void
w16(FILE *fp, unsigned v)
{
	unsigned char b[2];

	b[0] = v & 0xff;
	b[1] = (v >> 8) & 0xff;
	fwrite(b, 1, sizeof b, fp);
}

int
tu_zip_write(const char *path, const struct tu_zip_mem *mem, size_t n)
{
	time_t now = time(NULL);
	struct tm *lt = localtime(&now);
	unsigned dostime, dosdate, cdoff, cdlen;
	struct ent *e;
	FILE *fp;
	size_t i;
	uLong off;
	int rc = -1;

	if (n == 0)
		return -1;
	e = calloc(n, sizeof *e);
	if (e == NULL)
		return -1;

	dostime = (lt->tm_hour << 11) | (lt->tm_min << 5) | (lt->tm_sec / 2);
	dosdate = ((lt->tm_year - 80) << 9) | ((lt->tm_mon + 1) << 5) |
	    lt->tm_mday;

	/* Ready the members.  The deflated size is asked of the library before
	 * the archive can be drawn, and the local offsets are then the running
	 * sum of the headers and payloads before each member. */
	off = 0;
	for (i = 0; i < n; i++) {
		e[i].usize = mem[i].len;
		e[i].crc = crc32(0, mem[i].data, mem[i].len);
		if (mem[i].stored) {
			e[i].comp = malloc(mem[i].len);
			if (e[i].comp == NULL)
				goto out;
			memcpy(e[i].comp, mem[i].data, mem[i].len);
			e[i].csize = mem[i].len;
		} else if (deflate_mem(mem[i].data, mem[i].len, &e[i].comp,
		    &e[i].csize) != 0)
			goto out;
		e[i].lho = off;
		off += 30 + strlen(mem[i].name) + e[i].csize;
	}
	cdoff = off;
	cdlen = 0;
	for (i = 0; i < n; i++)
		cdlen += 46 + strlen(mem[i].name);

	fp = fopen(path, "wb");
	if (fp == NULL)
		goto out;

	/* Local headers and payloads. */
	for (i = 0; i < n; i++) {
		uLong nlen = strlen(mem[i].name);

		w32(fp, 0x04034b50);
		w16(fp, 20);			/* version needed */
		w16(fp, 0);			/* flags */
		w16(fp, mem[i].stored ? 0 : 8);
		w16(fp, dostime);
		w16(fp, dosdate);
		w32(fp, e[i].crc);
		w32(fp, e[i].csize);
		w32(fp, e[i].usize);
		w16(fp, nlen);
		w16(fp, 0);			/* extra length */
		fwrite(mem[i].name, 1, nlen, fp);
		if (e[i].csize != 0)
			fwrite(e[i].comp, 1, e[i].csize, fp);
	}

	/* Central directory. */
	for (i = 0; i < n; i++) {
		uLong nlen = strlen(mem[i].name);

		w32(fp, 0x02014b50);
		w16(fp, 20);			/* version made by */
		w16(fp, 20);			/* version needed */
		w16(fp, 0);			/* flags */
		w16(fp, mem[i].stored ? 0 : 8);
		w16(fp, dostime);
		w16(fp, dosdate);
		w32(fp, e[i].crc);
		w32(fp, e[i].csize);
		w32(fp, e[i].usize);
		w16(fp, nlen);
		w16(fp, 0);			/* extra length */
		w16(fp, 0);			/* comment length */
		w16(fp, 0);			/* disk number */
		w16(fp, 1);			/* internal attributes */
		w32(fp, 0);			/* external attributes */
		w32(fp, e[i].lho);
		fwrite(mem[i].name, 1, nlen, fp);
	}

	/* End of central directory. */
	w32(fp, 0x06054b50);
	w16(fp, 0);				/* this disk */
	w16(fp, 0);				/* disk with the directory */
	w16(fp, n);
	w16(fp, n);
	w32(fp, cdlen);
	w32(fp, cdoff);
	w16(fp, 0);				/* comment length */

	rc = 0;
	if (fclose(fp) != 0)
		rc = -1;

out:
	for (i = 0; i < n; i++)
		free(e[i].comp);
	free(e);
	return rc;
}