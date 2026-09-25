/*
 * tiffutil - strip compression.
 *
 * The two schemes are encoded to match the reference tool byte for byte, so
 * these are the encoders rather than their inverses alone.
 *
 * LZW is TIFF's variant: codes start at nine bits, grow to twelve, MSB first,
 * and the width increases one code early so that no table-size field is
 * needed.  PackBits is the run-length format, where a run is stored as
 * (257 - count) and one byte, a literal run as (count - 1) and the bytes, and
 * 128 means nothing at all.
 *
 * Copyright (C) 2026, LibreDarwin
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "tiffutil.h"
#include "buf.h"

#define LZW_CLEAR 256
#define LZW_EOI   257
#define LZW_FIRST 258
#define LZW_MAX   4096

/* One decoder-visible dictionary entry. */
typedef struct {
	uint16_t code;                /* the code this entry stands for */
	unsigned char first;          /* its first byte, for quick rejection */
	uint16_t next;                /* the code that follows it */
} lwentry_t;

typedef struct {
	uint16_t key;                 /* prefix code << 8 | next byte */
	uint16_t val;                 /* entry assigned, or 0xFFFF when free */
} lwslot_t;

int
lzw_encode(const unsigned char *in, size_t inlen, unsigned char **out,
    size_t *outlen)
{
	static const size_t HSIZE = 5021;
	uint32_t hkey[HSIZE];
	uint16_t hval[HSIZE];
	unsigned char hused[HSIZE];
	unsigned short firstbyte[LZW_MAX];
	unsigned short prefix[LZW_MAX];
	buf_t b = {NULL, 0, 0};
	uint32_t acc = 0;
	int nbits = 0, width = 9, next = LZW_FIRST, old = -1;
	size_t i;

	memset(hused, 0, sizeof(hused));
	for (int k = 0; k < 256; k++) {
		firstbyte[k] = (unsigned short)k;
		prefix[k] = 0xFFFF;
	}
	for (int k = 256; k < LZW_MAX; k++) {
		firstbyte[k] = 0;
		prefix[k] = 0xFFFF;
	}

#define EMIT(code) do {                                             \
		acc = (acc << width) | (uint32_t)(code);                \
		nbits += width;                                         \
		while (nbits >= 8) {                                     \
			nbits -= 8;                                      \
			if (buf_u8(&b, (acc >> nbits) & 0xFF) < 0)       \
				goto fail;                                 \
		}                                                        \
	} while (0)

	EMIT(LZW_CLEAR);
	old = inlen ? in[0] : -1;
	for (i = 1; i < inlen; i++) {
		unsigned char c = in[i];
		uint32_t key;
		size_t h, slot = 0;
		int seen = 0, coded = 0;

		if (old < 0) {
			old = c;
			continue;
		}
		key = ((uint32_t)old << 8) | c;
		h = ((size_t)key * 2654435761u) % HSIZE;
		for (size_t probe = 0; probe < HSIZE; probe++) {
			slot = (h + probe) % HSIZE;
			if (!hused[slot]) {
				hused[slot] = 1;
				hkey[slot] = key;
				hval[slot] = 0xFFFF;
				seen = 1;
				break;
			}
			if (hkey[slot] == key) {
				seen = 1;
				coded = hval[slot] != 0xFFFF;
				break;
			}
		}
		if (seen && coded) {
			old = hval[slot];
			continue;
		}
		/* A pair we have not encoded before: give it a code, flush the
		 * previous string, and start a fresh one with c. */
		if (seen && next < LZW_MAX) {
			hval[slot] = (uint16_t)next;
			firstbyte[next] = firstbyte[old];
			prefix[next] = (unsigned short)old;
			next++;
		}
		EMIT(old);
		old = c;
		if (next >= (1 << width) && width < 12)
			width++;
		if (next >= LZW_MAX) {
			EMIT(LZW_CLEAR);
			width = 9;
			next = LZW_FIRST;
			memset(hused, 0, sizeof(hused));
		}
	}
	if (old >= 0)
		EMIT(old);
	EMIT(LZW_EOI);
	if (nbits > 0) {
		acc <<= (8 - nbits);
		if (buf_u8(&b, acc & 0xFF) < 0)
			goto fail;
	}
#undef EMIT
	*out = b.p;
	*outlen = b.len;
	return 0;
fail:
	buf_free(&b);
	return -1;
}

int
packbits_encode(const unsigned char *in, size_t inlen, size_t rowbytes,
    unsigned char **out, size_t *outlen)
{
	buf_t b = {NULL, 0, 0};
	size_t i = 0;

	if (rowbytes == 0)
		rowbytes = inlen;
	while (i < inlen) {
		/* Each row is encoded on its own, so neither a run nor a block of
		 * literals is allowed to cross a row edge. The edge is taken from
		 * the row grid, not from i, or a run partway through a row would
		 * slide the boundary along with it. */
		size_t rowend = (i / rowbytes + 1) * rowbytes;
		size_t run = 1;

		if (rowend > inlen)
			rowend = inlen;
		while (i + run < rowend && in[i + run] == in[i] && run < 128)
			run++;
		if (run >= 3) {
			if (buf_u8(&b, (unsigned)(257 - run)) < 0 ||
			    buf_u8(&b, in[i]) < 0)
				goto fail;
			i += run;
			continue;
		}
		/* Otherwise emit literals until a run worth encoding appears. */
		{
			size_t lit = 0;
			while (i + lit < rowend && lit < 128) {
				if (i + lit + 2 < rowend &&
				    in[i + lit] == in[i + lit + 1] &&
				    in[i + lit] == in[i + lit + 2])
					break;
				lit++;
			}
			if (lit == 0)
				lit = 1;
			if (buf_u8(&b, (unsigned)(lit - 1)) < 0 ||
			    buf_put(&b, in + i, lit) < 0)
				goto fail;
			i += lit;
		}
	}
	*out = b.p;
	*outlen = b.len;
	return 0;
fail:
	buf_free(&b);
	return -1;
}
