/*
 * tiffutil - growable byte buffer.
 *
 * Copyright (C) 2026, LibreDarwin
 * SPDX-License-Identifier: BSD-3-Clause
 */

#ifndef TIFFUTIL_BUF_H
#define TIFFUTIL_BUF_H

#include <stddef.h>
#include <stdint.h>

typedef struct {
	unsigned char *p;
	size_t len;
	size_t cap;
} buf_t;

int buf_put(buf_t *b, const void *d, size_t n);
int buf_u8(buf_t *b, unsigned v);
int buf_u16(buf_t *b, unsigned v);
int buf_u32(buf_t *b, uint32_t v);
void buf_free(buf_t *b);

#endif
