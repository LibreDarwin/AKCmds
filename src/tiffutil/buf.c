/*
 * tiffutil - growable byte buffer.
 *
 * Copyright (C) 2026, LibreDarwin
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include <stdlib.h>
#include <string.h>

#include "buf.h"

int
buf_reserve(buf_t *b, size_t extra)
{
	size_t need = b->len + extra;
	unsigned char *n;

	if (need <= b->cap)
		return 0;
	while (b->cap < need)
		b->cap = b->cap ? b->cap * 2 : 1024;
	n = realloc(b->p, b->cap);
	if (n == NULL)
		return -1;
	b->p = n;
	return 0;
}

int
buf_put(buf_t *b, const void *d, size_t n)
{
	if (buf_reserve(b, n) < 0)
		return -1;
	memcpy(b->p + b->len, d, n);
	b->len += n;
	return 0;
}

int
buf_u8(buf_t *b, unsigned v)
{
	unsigned char c = (unsigned char)v;
	return buf_put(b, &c, 1);
}

int
buf_u16(buf_t *b, unsigned v)
{
	unsigned char c[2];
	c[0] = (unsigned char)(v >> 8);
	c[1] = (unsigned char)v;
	return buf_put(b, c, 2);
}

int
buf_u32(buf_t *b, uint32_t v)
{
	unsigned char c[4];
	c[0] = (unsigned char)(v >> 24);
	c[1] = (unsigned char)(v >> 16);
	c[2] = (unsigned char)(v >> 8);
	c[3] = (unsigned char)v;
	return buf_put(b, c, 4);
}

void
buf_free(buf_t *b)
{
	free(b->p);
	b->p = NULL;
	b->len = b->cap = 0;
}
