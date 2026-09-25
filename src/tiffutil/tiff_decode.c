/*
 * tiffutil - strip decompression.
 *
 * Only the schemes the reference tool both reads and writes are needed:
 * uncompressed, LZW and PackBits.  Anything else is reported the way libtiff
 * reports an unsupported scheme, because that message is part of the
 * observable behaviour being matched.
 *
 * Copyright (C) 2026, LibreDarwin
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "tiffutil.h"

#define LZW_CLEAR 256
#define LZW_EOI   257

/*
 * LZW as TIFF uses it: codes start at 9 bits, grow to 12, the code width
 * increases one bit early so that the encoder and decoder stay in step
 * without an explicit table-size field, and codes are packed most
 * significant bit first.
 */
int
lzw_decode(const unsigned char *in, size_t inlen, unsigned char **out,
    size_t *outlen)
{
	/* Each entry is the full string it stands for, built once and reused,
	 * which keeps the prefix walk simple enough to be obviously right. */
	unsigned char *tab[4096];
	size_t tablen[4096];
	unsigned char *o = NULL;
	unsigned char kbuf[4097];
	size_t olen = 0, ocap = 0;
	int next = 258, width = 9, old = -1;
	uint32_t bitbuf = 0;
	int bits = 0;
	size_t i = 0;

	if (inlen == 0) {
		*out = NULL;
		*outlen = 0;
		return 0;
	}
	for (int k = 0; k < 256; k++) {
		tab[k] = malloc(1);
		if (tab[k] == NULL)
			goto fail;
		tab[k][0] = (unsigned char)k;
		tablen[k] = 1;
	}
	for (int k = 256; k < 4096; k++) {
		tab[k] = NULL;
		tablen[k] = 0;
	}
	for (;;) {
		int code;
		const unsigned char *ent;
		size_t entlen;

		while (bits < width && i < inlen) {
			bitbuf = (bitbuf << 8) | in[i++];
			bits += 8;
		}
		if (bits < width)
			break;
		code = (int)((bitbuf >> (bits - width)) & ((1u << width) - 1));
		bits -= width;
		bitbuf &= (1u << bits) - 1;

		if (code == LZW_EOI)
			break;
		if (code == LZW_CLEAR) {
			for (int k = 256; k < 4096; k++)
				free(tab[k]);
			for (int k = 256; k < 4096; k++) {
				tab[k] = NULL;
				tablen[k] = 0;
			}
			next = 258;
			width = 9;
			old = -1;
			continue;
		}
		if (old == -1) {
			if (code >= 256 || tab[code] == NULL)
				break;
			ent = tab[code];
			entlen = tablen[code];
		} else {
			if (code < next && tab[code] != NULL) {
				ent = tab[code];
				entlen = tablen[code];
			} else if (code == next && tab[old] != NULL) {
				/* The code being defined as it is read: the old
				 * string plus its own first byte, which is one
				 * byte longer than anything stored. */
				if (tablen[old] + 1 > sizeof(kbuf))
					break;
				memcpy(kbuf, tab[old], tablen[old]);
				kbuf[tablen[old]] = tab[old][0];
				ent = kbuf;
				entlen = tablen[old] + 1;
			} else {
				break;
			}
			/* The entry defined here is the old string plus the
			 * first byte of the one just read. */
			if (next < 4096) {
				unsigned char *ne = malloc(tablen[old] + 1);
				if (ne == NULL)
					goto fail;
				memcpy(ne, tab[old], tablen[old]);
				ne[tablen[old]] = ent[0];
				tab[next] = ne;
				tablen[next] = tablen[old] + 1;
				next++;
				/* One code early. */
				if (next + 1 >= (1 << width) && width < 12)
					width++;
			}
		}
		if (ocap < olen + entlen) {
			ocap = (olen + entlen) * 2;
			o = realloc(o, ocap);
			if (o == NULL)
				goto fail;
		}
		memcpy(o + olen, ent, entlen);
		olen += entlen;
		old = code;
	}
	for (int k = 256; k < 4096; k++)
		free(tab[k]);
	for (int k = 0; k < 256; k++)
		free(tab[k]);
	*out = o;
	*outlen = olen;
	return 0;
fail:
	for (int k = 0; k < 4096; k++)
		free(tab[k]);
	free(o);
	return -1;
}

/*
 * PackBits: a run is stored as (257 - count) followed by one byte, a literal
 * run as (count - 1) followed by that many bytes, and 128 is a no-op.
 */
int
packbits_decode(const unsigned char *in, size_t inlen, unsigned char **out,
    size_t *outlen)
{
	unsigned char *o;
	size_t olen = 0, i = 0, cap = inlen * 2 + 64;

	o = malloc(cap);
	if (o == NULL)
		return -1;
	while (i < inlen) {
		int n = (signed char)in[i++];
		if (n >= 0) {
			size_t cnt = (size_t)n + 1;
			if (olen + cnt + 1 > cap) {
				unsigned char *p;
				while (olen + cnt + 1 > cap)
					cap *= 2;
				p = realloc(o, cap);
				if (p == NULL) {
					free(o);
					return -1;
				}
				o = p;
			}
			if (i + cnt > inlen)
				break;
			memcpy(o + olen, in + i, cnt);
			olen += cnt;
			i += cnt;
		} else if (n != -128) {
			size_t cnt = (size_t)(1 - n);
			if (olen + cnt + 1 > cap) {
				unsigned char *p;
				while (olen + cnt + 1 > cap)
					cap *= 2;
				p = realloc(o, cap);
				if (p == NULL) {
					free(o);
					return -1;
				}
				o = p;
			}
			if (i >= inlen)
				break;
			memset(o + olen, in[i++], cnt);
			olen += cnt;
		}
	}
	*out = o;
	*outlen = olen;
	return 0;
}

const char *
tu_compression_name(uint32_t c)
{
	switch (c) {
	case 1:     return "none";
	case 2:     return "CCITT 1D";
	case 3:     return "Group 3 Fax";
	case 4:     return "Group 4 Fax";
	case 5:     return "Lempel-Ziv & Welch encoding";
	case 6:     return "JPEG (old-style)";
	case 7:     return "JPEG";
	case 8:     return "Adobe Deflate";
	case 32773: return "PackBits";
	case 32946: return "Deflate";
	default:    return "unknown";
	}
}

const char *
tu_photometric_name(uint32_t p)
{
	switch (p) {
	case 0:  return "WhiteIsZero";
	case 1:  return "BlackIsZero";
	case 2:  return "RGB";
	case 3:  return "RGB Palette";
	case 4:  return "Transparency Mask";
	case 5:  return "CMYK";
	case 6:  return "YCbCr";
	case 8:  return "CIELab";
	case 9:  return "ICCLab";
	case 10: return "ITULab";
	case 32844: return "LogL";
	case 32845: return "LogLuv";
	default: return "unknown";
	}
}
