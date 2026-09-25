/*
 * tiffutil - inspect and convert TIFF files.
 *
 * Clean-room reimplementation of Apple's /usr/bin/tiffutil, derived purely
 * from black-box observation of that tool.  See NOTES.md for the recorded
 * findings; the short version:
 *
 *   - tiffutil writes through CoreGraphics, so output is always big-endian
 *     and always carries an ICC profile the input never had.  The profile is
 *     a constant of the output colour space, so the two observed blobs are
 *     embedded verbatim (icc_profiles.h).
 *   - The emitted field set and order are fixed: ascending tag, with
 *     FillOrder, Orientation, SampleFormat and ResolutionUnit filled in, and
 *     out-of-line values placed after the IFD in ascending tag order.
 *   - Predictor 2 is applied for LZW at 8 bits per sample only.
 *
 * Tags are read and written with an explicit type so that a value can be
 * copied between files of differing endianness without changing it.
 *
 * Copyright (C) 2026, LibreDarwin
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <errno.h>

#include "tiffutil.h"
#include "buf.h"

/* ------------------------------------------------------------------ */
/* diagnostics
 *
 * The reference tool prints libtiff's own low-level complaints on stderr
 * before its own summary line, and a great many of its failure paths exit
 * 0.  Both are reproduced because the parity harness compares them.
 */

void
tu_warn(const char *fmt, ...)
{
	va_list ap;
	va_start(ap, fmt);
	vfprintf(stderr, fmt, ap);
	va_end(ap);
}

/* ------------------------------------------------------------------ */
/* little/big-endian readers */

uint16_t
rd_be16(const unsigned char *p, int be)
{
	return be ? (uint16_t)((p[0] << 8) | p[1])
	           : (uint16_t)((p[1] << 8) | p[0]);
}

uint32_t
rd_be32(const unsigned char *p, int be)
{
	return be ? ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
	             ((uint32_t)p[2] << 8) | p[3]
	           : ((uint32_t)p[3] << 24) | ((uint32_t)p[2] << 16) |
	             ((uint32_t)p[1] << 8) | p[0];
}

static size_t
type_size(int type)
{
	switch (type) {
	case 1: case 2: case 6: case 7: return 1;
	case 3: case 8:                 return 2;
	case 4: case 9: case 11:        return 4;
	case 5: case 10: case 12:       return 8;
	default:                        return 0;
	}
}

static int
type_is_signed(int type)
{
	return type == 6 || type == 8 || type == 9 || type == 10;
}

/* ------------------------------------------------------------------ */
/* reading a TIFF */

static const char *const tu_type_names[] = {
	"", "BYTE", "ASCII", "SHORT", "LONG", "RATIONAL",
	"SBYTE", "UNDEFINED", "SSHORT", "SLONG", "SRATIONAL", "FLOAT", "DOUBLE"
};

const char *
tu_type_name(int type)
{
	if (type < 1 || type > 12)
		return "UNKNOWN";
	return tu_type_names[type];
}

/* Read one tag's raw value bytes out of the file, following the offset for
 * anything wider than four bytes.  Returns a malloc'd copy, or NULL. */
unsigned char *
tu_tag_raw(tiff_t *t, int dir, uint16_t tag, int *type, uint32_t *count)
{
	uint32_t n, off;
	size_t sz;
	unsigned char *out;
	int be = t->be;

	if (dir < 0 || dir >= t->ndir)
		return NULL;
	for (uint32_t i = 0; i < t->ndirs[dir]; i++) {
		tiff_ent_t *e = &t->ents[dir][i];
		if (e->tag != tag)
			continue;
		n = e->count;
		sz = type_size(e->type) * n;
		if (sz == 0)
			return NULL;
		if (sz <= 4) {
			out = malloc(sz ? sz : 1);
			if (out == NULL)
				return NULL;
			memcpy(out, e->inl, sz);
		} else {
			off = rd_be32(e->inl, be);
			if (off > t->len || sz > t->len - off) {
				tu_warn("TIFFFetchStripThing: Invalid strip offset/length.\n");
				return NULL;
			}
			out = malloc(sz);
			if (out == NULL)
				return NULL;
			memcpy(out, t->data + off, sz);
		}
		*type = e->type;
		*count = n;
		return out;
	}
	return NULL;
}

/* Read a tag as a single unsigned integer, which is how every field the
 * reference tool reports is consumed. */
int
tu_get_uint(tiff_t *t, int dir, uint16_t tag, uint32_t *out)
{
	int type;
	uint32_t count;
	unsigned char *raw = tu_tag_raw(t, dir, tag, &type, &count);
	uint32_t v = 0;

	if (raw == NULL)
		return -1;
	if (count >= 1) {
		switch (type_size(type)) {
		case 1: v = type_is_signed(type) ? (uint32_t)(int32_t)(int8_t)raw[0]
		                                 : raw[0];
			break;
		case 2: v = type_is_signed(type) ? (uint32_t)(int32_t)(int16_t)rd_be16(raw, t->be)
		                                 : rd_be16(raw, t->be);
			break;
		case 4: v = type_is_signed(type) ? (uint32_t)(int32_t)rd_be32(raw, t->be)
		                                 : rd_be32(raw, t->be);
			break;
		case 8: v = rd_be32(raw, t->be); break;   /* RATIONAL: numerator */
		default: v = 0; break;
		}
	}
	free(raw);
	*out = v;
	return 0;
}

int
tu_has_tag(tiff_t *t, int dir, uint16_t tag)
{
	if (dir < 0 || dir >= t->ndir)
		return 0;
	for (uint32_t i = 0; i < t->ndirs[dir]; i++)
		if (t->ents[dir][i].tag == tag)
			return 1;
	return 0;
}

int
tu_get_bytes(tiff_t *t, int dir, uint16_t tag, unsigned char **out, uint32_t *n)
{
	int type;
	uint32_t count;
	unsigned char *raw = tu_tag_raw(t, dir, tag, &type, &count);

	if (raw == NULL)
		return -1;
	*out = raw;
	*n = count;
	return 0;
}

/* ------------------------------------------------------------------ */

static int
cmp_tag(const void *a, const void *b)
{
	uint16_t x = ((const tiff_ent_t *)a)->tag;
	uint16_t y = ((const tiff_ent_t *)b)->tag;
	return x < y ? -1 : x > y ? 1 : 0;
}

void
tiff_close(tiff_t *t)
{
	for (int d = 0; d < t->ndir; d++)
		free(t->ents[d]);
	free(t->ents);
	free(t->ndirs);
	free(t->ifdoff);
	free(t->strips);
	free(t->stripbc);
	free(t->data);
	memset(t, 0, sizeof(*t));
}

/*
 * Parse a TIFF held in memory.  Returns 0 on success.  On the failure paths
 * the reference tool's own libtiff warnings are reproduced, because the
 * harness compares stderr.
 */
int
tiff_open_mem(tiff_t *t, const unsigned char *data, size_t len)
{
	uint32_t off, next, i;
	int be;

	memset(t, 0, sizeof(*t));
	if (len < 8) {
		tu_warn("TIFFOpen: not a TIFF file.\n");
		return -1;
	}
	if (data[0] == 'M' && data[1] == 'M')
		be = 1;
	else if (data[0] == 'I' && data[1] == 'I')
		be = 0;
	else {
		if (tu_chatter)
			tu_warn("TIFFOpen: Unknown image file format.\n");
		return -1;
	}
	if (rd_be16(data + 2, be) != 42) {
		if (tu_chatter)
			tu_warn("TIFFOpen: Not a TIFF file, bad version number %u.\n",
			    (unsigned)rd_be16(data + 2, be));
		return -1;
	}

	t->be = be;
	t->data = malloc(len);
	if (t->data == NULL)
		return -1;
	memcpy(t->data, data, len);
	t->len = len;

	off = rd_be32(data + 4, be);
	while (off != 0) {
		uint16_t n;
		tiff_ent_t *ents;
		uint32_t *dirs;

		if (off + 2 > len) {
			tu_warn("TIFFReadDirectory: Failed to read directory at offset %u.\n", off);
			return -1;
		}
		n = rd_be16(t->data + off, be);
		if (n == 0) {
			tu_warn("TIFFFetchDirectory: Sanity check on directory count failed, "
			    "zero tag directories not supported.\n");
			tu_warn("TIFFReadDirectory: Failed to read directory at offset %u.\n", off);
			return -1;
		}
		if (off + 2 + (size_t)n * 12 + 4 > len) {
			tu_warn("TIFFReadDirectory: Failed to read directory at offset %u.\n", off);
			return -1;
		}
		ents = calloc(n, sizeof(*ents));
		if (ents == NULL)
			return -1;
		for (i = 0; i < n; i++) {
			const unsigned char *e = t->data + off + 2 + i * 12;
			ents[i].tag = rd_be16(e, be);
			ents[i].type = rd_be16(e + 2, be);
			ents[i].count = rd_be32(e + 4, be);
			memcpy(ents[i].inl, e + 8, 4);
		}
		dirs = realloc(t->ndirs, (size_t)(t->ndir + 1) * sizeof(*dirs));
		if (dirs == NULL) {
			free(ents);
			return -1;
		}
		t->ndirs = dirs;
		dirs[t->ndir] = n;
		{
			uint32_t *ofs = realloc(t->ifdoff, (size_t)(t->ndir + 1) * sizeof(*ofs));
			if (ofs == NULL) {
				free(ents);
				return -1;
			}
			t->ifdoff = ofs;
			ofs[t->ndir] = off;
		}
		{
			tiff_ent_t **slot = realloc(t->ents, (size_t)(t->ndir + 1) * sizeof(*slot));
			if (slot == NULL) {
				free(ents);
				return -1;
			}
			t->ents = slot;
			t->ents[t->ndir++] = ents;
		}
		if (t->ndir > 4096) {
			tu_warn("TIFFReadDirectory: Failed to read directory at offset %u.\n", off);
			return -1;
		}
		next = rd_be32(t->data + off + 2 + (size_t)n * 12, be);
		off = next;
	}
	if (t->ndir == 0) {
		tu_warn("TIFFOpen: Empty TIFF file.\n");
		return -1;
	}

	/* Resolve the strip table once; -info and -verboseinfo both report it.
	 * StripOffsets is SHORT for a single strip and LONG otherwise, so the
	 * type has to come from the field rather than be assumed. */
	t->nstrips = 0;
	t->strips = NULL;
	t->stripbc = NULL;
	{
		int d = 0, otype = 0, ctype = 0;
		uint32_t no = 0, nc = 0;
		unsigned char *o = NULL, *c = NULL;

		if (tu_get_bytes(t, d, TAG_STRIPOFFSETS, &o, &no) == 0 && o != NULL) {
			for (uint32_t i = 0; i < t->ndirs[0]; i++)
				if (t->ents[0][i].tag == TAG_STRIPOFFSETS)
					otype = t->ents[0][i].type;
		}
		if (tu_get_bytes(t, d, TAG_STRIPBYTECOUNTS, &c, &nc) == 0 && c != NULL) {
			for (uint32_t i = 0; i < t->ndirs[0]; i++)
				if (t->ents[0][i].tag == TAG_STRIPBYTECOUNTS)
					ctype = t->ents[0][i].type;
		}
		if (o != NULL && c != NULL && no > 0 && nc > 0) {
			uint32_t lim = no < nc ? no : nc;
			size_t osz = type_size(otype), csz = type_size(ctype);
			if (lim > 0 && osz > 0 && csz > 0) {
				t->strips = calloc(lim, sizeof(uint32_t));
				t->stripbc = calloc(lim, sizeof(uint32_t));
				for (uint32_t k = 0; k < lim; k++) {
					t->strips[k] = osz == 2 ? rd_be16(o + 2 * k, t->be)
					                        : rd_be32(o + 4 * k, t->be);
					t->stripbc[k] = csz == 2 ? rd_be16(c + 2 * k, t->be)
					                        : rd_be32(c + 4 * k, t->be);
				}
				t->nstrips = lim;
			}
		}
		free(o);
		free(c);
	}
	return 0;
}

int tu_chatter;

void
tu_set_chatter(int on)
{
	tu_chatter = on;
}

int
tiff_open_file(tiff_t *t, const char *path)
{
	/* Zero first so that a failed open still leaves something a caller
	 * can safely hand to tiff_close(). */
	memset(t, 0, sizeof(*t));

	FILE *f = fopen(path, "rb");
	unsigned char *data = NULL;
	size_t len = 0, cap = 0;
	int rc;

	if (f == NULL) {
		if (errno == ENOENT) {
			t->openerc = TUFF_ENOENT;
			return TUFF_ENOENT;
		}
		if (tu_chatter) {
			fprintf(stderr, "TIFFOpen: %s: ", path);
			fprintf(stderr, "%s.\n", strerror(errno));
			fprintf(stderr, "Error: Can't open %s. Either it isn't "
			    "readable, it isn't a TIFF file, or there are "
			    "unrecognized tags; try tiffutil -dump for more "
			    "info.\n", path);
		}
		t->openerc = TUFF_EOPEN;
		return TUFF_EOPEN;
	}
	for (;;) {
		size_t n;
		if (len == cap) {
			unsigned char *nd;
			cap = cap ? cap * 2 : 65536;
			nd = realloc(data, cap);
			if (nd == NULL) {
				free(data);
				fclose(f);
				return -1;
			}
			data = nd;
		}
		n = fread(data + len, 1, cap - len, f);
		if (n == 0)
			break;
		len += n;
	}
	fclose(f);
	if (len == 0) {
		free(data);
		fprintf(stderr, "TIFFOpen: %s: ", path);
		fprintf(stderr, "%s.\n", "cannot read file");
		fprintf(stderr, "Error: Can't open %s. Either it isn't readable, "
		    "it isn't a TIFF file, or there are unrecognized tags; "
		    "try tiffutil -dump for more info.\n", path);
		return -1;
	}
	rc = tiff_open_mem(t, data, len);
	free(data);
	return rc;
}

void
tiff_sort_entries(tiff_t *t)
{
	for (int d = 0; d < t->ndir; d++)
		qsort(t->ents[d], t->ndirs[d], sizeof(t->ents[d][0]), cmp_tag);
}
