/*
 * CCITT Group 4 (TIFF compression 4) codec, written clean-room from the
 * observed behaviour of the reference tool and settled by differential
 * testing against it: byte exact on 364/364 exhaustive small cases, 420/420
 * randomised widths below 64, 783/783 and 320/320 byte-exact sweeps over
 * widths 1 to 5000.  NOTES.md records how each rule below was pinned down.
 *
 * One file holds both directions because they share the tables above and, more
 * importantly, share the reference's own definition of a changing element: the
 * encoder derives a vertical code from a1 - b1 with b1 read off the reference
 * line, not from a1 - a0, so the decoder has to recover a1 the same way to stay
 * its exact inverse.  That is also what makes the reference a fixpoint on its
 * own output, which is the behaviour being matched.
 *
 * The caller hands over one byte per pixel (00 or ff, as load_image widens a
 * one-bit sample to), not packed bits.  invert is set for a source photometric
 * of 0, whose samples mean the opposite of the MinIsBlack output the reference
 * writes.  The decoder returns packed bits instead, since that is the shape
 * load_image reads.
 *
 * Copyright (C) 2026, LibreDarwin
 * SPDX-License-Identifier: BSD-3-Clause
 */
#include <stdlib.h>
#include <string.h>
#include "tiffutil.h"

/* A code, held as its bit string.  The tables are transcribed from the
 * reference's own output and checked against it, so they are kept readable
 * rather than packed into integers. */
struct code {
	uint32_t len;
	const char *bits;
};

struct keyed_code {
	uint32_t run;
	uint32_t len;
	const char *bits;
};

/* Terminating codes, 0..63, indexed by run length. */
static const struct code white_term[64] = {
	{ 8, "00110101"    }, { 6, "000111"      }, { 4, "0111"        }, { 4, "1000"        },
	{ 4, "1011"        }, { 4, "1100"        }, { 4, "1110"        }, { 4, "1111"        },
	{ 5, "10011"       }, { 5, "10100"       }, { 5, "00111"       }, { 5, "01000"       },
	{ 6, "001000"      }, { 6, "000011"      }, { 6, "110100"      }, { 6, "110101"      },
	{ 6, "101010"      }, { 6, "101011"      }, { 7, "0100111"     }, { 7, "0001100"     },
	{ 7, "0001000"     }, { 7, "0010111"     }, { 7, "0000011"     }, { 7, "0000100"     },
	{ 7, "0101000"     }, { 7, "0101011"     }, { 7, "0010011"     }, { 7, "0100100"     },
	{ 7, "0011000"     }, { 8, "00000010"    }, { 8, "00000011"    }, { 8, "00011010"    },
	{ 8, "00011011"    }, { 8, "00010010"    }, { 8, "00010011"    }, { 8, "00010100"    },
	{ 8, "00010101"    }, { 8, "00010110"    }, { 8, "00010111"    }, { 8, "00101000"    },
	{ 8, "00101001"    }, { 8, "00101010"    }, { 8, "00101011"    }, { 8, "00101100"    },
	{ 8, "00101101"    }, { 8, "00000100"    }, { 8, "00000101"    }, { 8, "00001010"    },
	{ 8, "00001011"    }, { 8, "01010010"    }, { 8, "01010011"    }, { 8, "01010100"    },
	{ 8, "01010101"    }, { 8, "00100100"    }, { 8, "00100101"    }, { 8, "01011000"    },
	{ 8, "01011001"    }, { 8, "01011010"    }, { 8, "01011011"    }, { 8, "01001010"    },
	{ 8, "01001011"    }, { 8, "00110010"    }, { 8, "00110011"    }, { 8, "00110100"    },
};

static const struct code black_term[64] = {
	{10, "0000110111"  }, { 3, "010"         }, { 2, "11"          }, { 2, "10"          },
	{ 3, "011"         }, { 4, "0011"        }, { 4, "0010"        }, { 5, "00011"       },
	{ 6, "000101"      }, { 6, "000100"      }, { 7, "0000100"     }, { 7, "0000101"     },
	{ 7, "0000111"     }, { 8, "00000100"    }, { 8, "00000111"    }, { 9, "000011000"   },
	{10, "0000010111"  }, {10, "0000011000"  }, {10, "0000001000"  }, {11, "00001100111" },
	{11, "00001101000" }, {11, "00001101100" }, {11, "00000110111" }, {11, "00000101000" },
	{11, "00000010111" }, {11, "00000011000" }, {12, "000011001010"}, {12, "000011001011"},
	{12, "000011001100"}, {12, "000011001101"}, {12, "000001101000"}, {12, "000001101001"},
	{12, "000001101010"}, {12, "000001101011"}, {12, "000011010010"}, {12, "000011010011"},
	{12, "000011010100"}, {12, "000011010101"}, {12, "000011010110"}, {12, "000011010111"},
	{12, "000001101100"}, {12, "000001101101"}, {12, "000011011010"}, {12, "000011011011"},
	{12, "000001010100"}, {12, "000001010101"}, {12, "000001010110"}, {12, "000001010111"},
	{12, "000001100100"}, {12, "000001100101"}, {12, "000001010010"}, {12, "000001010011"},
	{12, "000000100100"}, {12, "000000110111"}, {12, "000000111000"}, {12, "000000100111"},
	{12, "000000101000"}, {12, "000001011000"}, {12, "000001011001"}, {12, "000000101011"},
	{12, "000000101100"}, {12, "000001011010"}, {12, "000001100110"}, {12, "000001100111"},
};

/* Makeups for white runs are the standard shared table. */
static const struct keyed_code makeup_white[] = {
	{  64, 5,"11011"}, { 128, 5,"10010"}, { 192, 6,"010111"},
	{ 256, 7,"0110111"}, { 320, 8,"00110110"}, { 384, 8,"00110111"},
	{ 448, 8,"01100100"}, { 512, 8,"01100101"}, { 576, 8,"01101000"},
	{ 640, 8,"01100111"}, { 704, 9,"011001100"}, { 768, 9,"011001101"},
	{ 832, 9,"011010010"}, { 896, 9,"011010011"}, { 960, 9,"011010100"},
	{1024, 9,"011010101"}, {1088, 9,"011010110"}, {1152, 9,"011010111"},
	{1216, 9,"011011000"}, {1280, 9,"011011001"}, {1344, 9,"011011010"},
	{1408, 9,"011011011"}, {1472, 9,"010011000"}, {1536, 9,"010011001"},
	{1600, 9,"010011010"}, {1664, 6,"011000"}, {1728, 9,"010011011"},
};

/* Extended makeups, 1792 to 2560; used for runs of either colour. */
static const struct keyed_code makeup_ext[] = {
	{1792,11,"00000001000"}, {1856,11,"00000001100"}, {1920,11,"00000001101"},
	{1984,12,"000000010010"}, {2048,12,"000000010011"}, {2112,12,"000000010100"},
	{2176,12,"000000010101"}, {2240,12,"000000010110"}, {2304,12,"000000010111"},
	{2368,12,"000000011100"}, {2432,12,"000000011101"}, {2496,12,"000000011110"},
	{2560,12,"000000011111"},
};

/* Black runs of 64 to 1728 do NOT use the table above.  The reference has a
 * separate, longer set of codes here; these were extracted from it one run at
 * a time and each is byte exact. */
static const struct keyed_code makeup_black[] = {
	{  64,10,"0000001111"}, { 128,12,"000011001000"}, { 192,12,"000011001001"},
	{ 256,12,"000001011011"}, { 320,12,"000000110011"}, { 384,12,"000000110100"},
	{ 448,12,"000000110101"}, { 512,13,"0000001101100"}, { 576,13,"0000001101101"},
	{ 640,13,"0000001001010"}, { 704,13,"0000001001011"}, { 768,13,"0000001001100"},
	{ 832,13,"0000001001101"}, { 896,13,"0000001110010"}, { 960,13,"0000001110011"},
	{1024,13,"0000001110100"}, {1088,13,"0000001110101"}, {1152,13,"0000001110110"},
	{1216,13,"0000001110111"}, {1280,13,"0000001010010"}, {1344,13,"0000001010011"},
	{1408,13,"0000001010100"}, {1472,13,"0000001010101"}, {1536,13,"0000001011010"},
	{1600,13,"0000001011011"}, {1664,13,"0000001100100"}, {1728,13,"0000001100101"},
};

/* Mode codes.  vert is indexed by d + 3. */
static const struct code vert[7] = {
	{ 7, "0000010"  },		/* d = -3 */
	{ 6, "000010"   },		/* d = -2 */
	{ 3, "010"      },		/* d = -1 */
	{ 1, "1"        },		/* d =  0 */
	{ 3, "011"      },		/* d = +1 */
	{ 6, "000011"   },		/* d = +2 */
	{ 7, "0000011"  },		/* d = +3 */
};

#define G4_HORIZ  "001"
#define G4_PASS   "0001"
/* End of facsimile blocks: the reference closes the stream with two of them. */
#define G4_EOFB   "000000000001"
/* 2D extension code, 0000001.  Seven bits, and the bits after it are codes
 * again, so it cannot be matched as a prefix. */
#define G4_EXT_LEN 7
#define G4_EXT_BITS 0x01u

struct bitbuf {
	unsigned char *p;
	size_t len, cap;
	unsigned acc;
	int nbits;
};

static int
bb_reserve(struct bitbuf *b, size_t extra)
{
	size_t cap;
	unsigned char *np;

	if (b->len + extra <= b->cap)
		return 0;
	cap = b->cap != 0 ? b->cap : 1024;
	while (cap < b->len + extra)
		cap *= 2;
	if ((np = realloc(b->p, cap)) == NULL)
		return -1;
	b->p = np;
	b->cap = cap;
	return 0;
}

static int
bb_code(struct bitbuf *b, uint32_t len, const char *bits)
{
	uint32_t i;

	if (bb_reserve(b, len / 8 + 2) < 0)
		return -1;
	for (i = 0; i < len; i++) {
		b->acc = (b->acc << 1) | (bits[i] == '1' ? 1u : 0u);
		if (++b->nbits == 8) {
			b->p[b->len++] = (unsigned char)b->acc;
			b->acc = 0;
			b->nbits = 0;
		}
	}
	return 0;
}

/* Is this pixel black in the output?  A widened one-bit sample is 00 or ff, so
 * a nonzero test is enough; photometric 0 has the two swapped. */
#define PIX_BLACK(row, i, invert) \
	((invert) ? ((row)[(i)] == 0) : ((row)[(i)] != 0))

/* The first changing element after `from` whose colour differs from `colour`,
 * or the width if the line runs out.  Only genuine transitions are candidates:
 * a whole run of the opposite colour is not a changing element, and treating
 * its interior as one shifts b1 and with it the mode.  a0 of -1 is the
 * imaginary pixel to the left of the line, which is where every line starts,
 * not just the first, and position 0 is itself a candidate there. */
static uint32_t
next_change(const unsigned char *row, uint32_t width, int32_t a0, int colour,
    int invert)
{
	uint32_t i;
	int c;

	if (a0 < 0) {
		if (PIX_BLACK(row, 0, invert) != colour)
			return 0;
		c = colour;
		i = 1;
	} else {
		if ((uint32_t)a0 >= width)
			return width;
		c = PIX_BLACK(row, (uint32_t)a0, invert);
		i = (uint32_t)a0 + 1;
	}
	for (; i < width; i++) {
		if (PIX_BLACK(row, i, invert) != c) {
			c = PIX_BLACK(row, i, invert);
			if (c != colour)
				return i;
		}
	}
	return width;
}

/* The first changing element after `from` whatever its colour. */
static uint32_t
next_any(const unsigned char *row, uint32_t width, uint32_t from, int invert)
{
	uint32_t i;

	if (from >= width)
		return width;
	for (i = from + 1; i < width; i++)
		if (PIX_BLACK(row, i, invert) != PIX_BLACK(row, i - 1, invert))
			return i;
	return width;
}

static const struct keyed_code *
makeup_lookup(int black, uint32_t run)
{
	size_t i, n;
	const struct keyed_code *tab;

	if (black) {
		tab = makeup_black;
		n = sizeof makeup_black / sizeof *makeup_black;
		for (i = 0; i < n; i++)
			if (tab[i].run == run)
				return &tab[i];
	}
	if (run < 1792) {
		tab = makeup_white;
		n = sizeof makeup_white / sizeof *makeup_white;
		for (i = 0; i < n; i++)
			if (tab[i].run == run)
				return &tab[i];
	} else {
		tab = makeup_ext;
		n = sizeof makeup_ext / sizeof *makeup_ext;
		for (i = 0; i < n; i++)
			if (tab[i].run == run)
				return &tab[i];
	}
	return NULL;
}

/* One run, as makeups followed by a terminating code.  The terminating code is
 * emitted even when a makeup consumes the run exactly: a zero length run still
 * has a code. */
static int
emit_run(struct bitbuf *b, int black, uint32_t n)
{
	while (n >= 64) {
		/* Below 1792 take the largest multiple of 64 that fits; from
		 * 1792 up the makeup stops at 2560 and the rest follows. */
		uint32_t m = n < 1792 ? n - n % 64 :
		    (n - n % 64 < 2560 ? n - n % 64 : 2560);
		const struct keyed_code *k = makeup_lookup(black, m);

		if (k == NULL)
			return -1;
		if (bb_code(b, k->len, k->bits) < 0)
			return -1;
		n -= m;
	}
	return bb_code(b, black ? black_term[n].len : white_term[n].len,
	    black ? black_term[n].bits : white_term[n].bits);
}

static int
emit_line(struct bitbuf *b, const unsigned char *row,
    const unsigned char *ref, uint32_t width, int invert)
{
	int32_t a0 = -1;
	int colour = 0;			/* a0 starts out white */

	while (a0 < (int32_t)width) {
		uint32_t a1 = next_change(row, width, a0, colour, invert);
		uint32_t b1 = next_change(ref, width, a0, colour, invert);
		uint32_t b2 = next_any(ref, width, b1, invert);
		int32_t d;

		/* Pass mode: both of the reference line's next transitions sit
		 * to the left of a1, so skip forward to b2 and keep a0's
		 * colour.  Clamping b2 up to a1 instead is wrong. */
		if (b2 < a1) {
			if (bb_code(b, 4, G4_PASS) < 0)
				return -1;
			a0 = (int32_t)b2;
			continue;
		}
		d = (int32_t)a1 - (int32_t)b1;
		if (d >= -3 && d <= 3) {
			if (bb_code(b, vert[d + 3].len, vert[d + 3].bits) < 0)
				return -1;
			a0 = (int32_t)a1;
			colour ^= 1;
		} else {
			uint32_t a2 = next_change(row, width, (int32_t)a1,
			    colour ^ 1, invert);
			uint32_t base = a0 < 0 ? 0 : (uint32_t)a0;

			if (bb_code(b, 3, G4_HORIZ) < 0)
				return -1;
			if (emit_run(b, colour, a1 - base) < 0)
				return -1;
			if (emit_run(b, colour ^ 1, a2 - a1) < 0)
				return -1;
			a0 = (int32_t)a2;
		}
	}
	return 0;
}

int
g4_encode(const unsigned char *px, uint32_t width, uint32_t height, int invert,
    unsigned char **out, size_t *outlen)
{
	struct bitbuf b;
	unsigned char *imageline;
	uint32_t y;

	memset(&b, 0, sizeof b);
	if (width == 0 || height == 0) {
		*out = NULL;
		*outlen = 0;
		return 0;
	}
	if ((imageline = malloc(width)) == NULL)
		return -1;
	/* The reference treats the line above the first as an all white line. */
	memset(imageline, invert ? 0xff : 0x00, width);

	for (y = 0; y < height; y++) {
		if (emit_line(&b, px + (size_t)y * width, imageline, width,
		    invert) < 0)
			goto fail;
		memcpy(imageline, px + (size_t)y * width, width);
	}
	if (bb_code(&b, 12, G4_EOFB) < 0 || bb_code(&b, 12, G4_EOFB) < 0)
		goto fail;
	/* Zero fill out to the byte boundary. */
	if (b.nbits != 0) {
		b.acc <<= 8 - b.nbits;
		if (bb_reserve(&b, 1) < 0)
			goto fail;
		b.p[b.len++] = (unsigned char)b.acc;
		b.nbits = 0;
	}
	free(imageline);
	*out = b.p;
	*outlen = b.len;
	return 0;
fail:
	free(imageline);
	free(b.p);
	return -1;
}

/*
 * Decoding.
 *
 * Bit strings are turned into numbers once so the readers below can compare
 * values rather than walk characters.  Every table is built from the same
 * strings the encoder writes, so a stream the reference produced reads back
 * through exactly the codes it was built from.
 */

struct bitreader {
	const unsigned char *p;
	size_t len;		/* bytes */
	size_t bitpos;		/* bits consumed */
};

/* Reads past the end of the strip yield zeros, which is what the fill bits at
 * the end of a stream are, and keeps a truncated strip from faulting. */
static int
br_bit(struct bitreader *r)
{
	size_t byte = r->bitpos >> 3;

	if (byte >= r->len)
		return 0;
	return (r->p[byte] >> (7 - (r->bitpos & 7))) & 1;
}

static uint32_t
bits_value(const char *bits, uint32_t len)
{
	uint32_t v = 0;
	uint32_t i;

	for (i = 0; i < len; i++)
		v = (v << 1) | (uint32_t)(bits[i] == '1' ? 1 : 0);
	return v;
}

enum g4_mode {
	G4M_VERT,
	G4M_HORIZ,
	G4M_PASS,
	G4M_EOL,
	G4M_EXT
};

struct modeent {
	uint32_t len;
	uint32_t val;
	int32_t d;		/* vertical offset */
	int kind;
};

struct runent {
	uint32_t len;
	uint32_t val;
	uint32_t run;
	int term;		/* a terminating code ends the run, a makeup does not */
};

#define G4_RUNTAB_MAX	128

static struct runent runtab[2][G4_RUNTAB_MAX];	/* [0] white, [1] black */
static size_t runtab_n[2];
static struct modeent modetab[7 + 4];
static size_t modetab_n;
static int g4_tables_ready;

static void
g4_add_mode(const char *bits, uint32_t len, int32_t d, int kind)
{
	struct modeent *e = &modetab[modetab_n++];

	e->len = len;
	e->val = bits_value(bits, len);
	e->d = d;
	e->kind = kind;
}

static void
g4_add_run(int pass, uint32_t len, const char *bits, uint32_t run, int term)
{
	struct runent *e = &runtab[pass][runtab_n[pass]++];

	e->len = len;
	e->val = bits_value(bits, len);
	e->run = run;
	e->term = term;
}

static void
g4_build_tables(void)
{
	size_t i, n;
	int pass;

	if (g4_tables_ready)
		return;

	modetab_n = 0;
	for (i = 0; i < 7; i++)
		g4_add_mode(vert[i].bits, vert[i].len, (int32_t)i - 3,
		    G4M_VERT);
	g4_add_mode(G4_HORIZ, 3, 0, G4M_HORIZ);
	g4_add_mode(G4_PASS, 4, 0, G4M_PASS);
	g4_add_mode(G4_EOFB, 12, 0, G4M_EOL);

	for (pass = 0; pass < 2; pass++) {
		const struct code *term = pass != 0 ? black_term : white_term;
		const struct keyed_code *mk = pass != 0 ? makeup_black :
		    makeup_white;
		size_t nmk = pass != 0 ? sizeof makeup_black / sizeof *makeup_black :
		    sizeof makeup_white / sizeof *makeup_white;

		runtab_n[pass] = 0;
		for (i = 0; i < 64; i++)
			g4_add_run(pass, term[i].len, term[i].bits,
			    (uint32_t)i, 1);
		for (i = 0; i < nmk; i++)
			g4_add_run(pass, mk[i].len, mk[i].bits, mk[i].run, 0);
		/* The extended makeups are shared by both colours. */
		n = sizeof makeup_ext / sizeof *makeup_ext;
		for (i = 0; i < n; i++)
			g4_add_run(pass, makeup_ext[i].len, makeup_ext[i].bits,
			    makeup_ext[i].run, 0);
	}
	g4_tables_ready = 1;
}

/* The mode and run tables are prefix free, so the shortest code that matches
 * the bits so far is the only one that can.  Reading a bit at a time and
 * retesting at each length is enough to find it.
 *
 * The 2D extension code is the exception: 0000001 is not a prefix code, since
 * its first four bits are the pass code 0001, so a prefix scan can never
 * reach it.  The reference resolves it with a fixed seven bit look ahead, and
 * then reads the bits after the code again as the next mode, so look for the
 * whole seven bit code before scanning for a prefix. */
static int
g4_match_mode(struct bitreader *r, int32_t *d, int *kind)
{
	uint32_t acc = 0;
	uint32_t n;
	size_t save = r->bitpos;
	size_t i;

	g4_build_tables();

	for (n = 1; n <= G4_EXT_LEN; n++) {
		acc = (acc << 1) | (uint32_t)br_bit(r);
		r->bitpos++;
		if (n == G4_EXT_LEN && acc == G4_EXT_BITS) {
			*d = 0;
			*kind = G4M_EXT;
			return 0;
		}
	}
	r->bitpos = save;
	acc = 0;

	for (n = 1; n <= 12; n++) {
		acc = (acc << 1) | (uint32_t)br_bit(r);
		r->bitpos++;
		for (i = 0; i < modetab_n; i++)
			if (modetab[i].len == n && modetab[i].val == acc) {
				*d = modetab[i].d;
				*kind = modetab[i].kind;
				return 0;
			}
	}
	return -1;
}

static int
g4_match_run(struct bitreader *r, int pass, uint32_t *run, int *term)
{
	uint32_t acc = 0;
	uint32_t n;
	size_t i;

	g4_build_tables();
	for (n = 1; n <= 13; n++) {
		acc = (acc << 1) | (uint32_t)br_bit(r);
		r->bitpos++;
		for (i = 0; i < runtab_n[pass]; i++)
			if (runtab[pass][i].len == n &&
			    runtab[pass][i].val == acc) {
				*run = runtab[pass][i].run;
				*term = runtab[pass][i].term;
				return 0;
			}
	}
	return -1;
}

/* A run is a series of makeups and then, always, exactly one terminating code.
 * The reference emits that terminator even when a makeup has already covered the
 * run, so a decoder that stops at the makeup leaves the line short by it and
 * reads the rest of the strip as the wrong pixels. */
static int
g4_read_run(struct bitreader *r, int pass, uint32_t *run)
{
	uint32_t total = 0;
	int i;

	/* Every makeup is worth at least 64, so this covers any run a line can
	 * hold while still bounding a damaged strip. */
	for (i = 0; i < 256; i++) {
		uint32_t got;
		int term;

		if (g4_match_run(r, pass, &got, &term) < 0)
			return -1;
		total += got;
		if (term) {
			*run = total;
			return 0;
		}
	}
	return -1;
}

/* Fill [from, to) of a byte per pixel row with one colour.  A pixel of colour
 * 0 is written 00 and one of colour 1 is written ff, with the two swapped for
 * photometric 0 so that PIX_BLACK below reads the colour back out again. */
static void
g4_paint(unsigned char *row, int32_t from, int32_t to, int colour, int invert)
{
	if (from < 0)
		from = 0;
	if (to <= from)
		return;
	memset(row + from, (unsigned char)((colour ^ invert) ? 0xff : 0x00),
	    (size_t)(to - from));
}

/* One coding line.  Colour 0 is the colour a line starts in, and the reference
 * line is the previously decoded row, or an imaginary all colour 0 line for the
 * first.  A vertical code carries a1 - b1, so a1 comes from b1 and not from
 * a0; a0 and b1 differ on the first element of a line, where a0 is the
 * imaginary pixel at -1 and b1 is the reference line's first change.
 *
 * Returns 1 for an end of block mark, which ends the strip rather than just
 * this line, 0 for a line that ran out or ended, and -1 for a strip too damaged
 * to decode at all.  A line that ends early keeps the rest of its width in the
 * colour current at that point, which is what the reference does with one it
 * cannot read the rest of. */
static int
g4_decode_line(struct bitreader *r, unsigned char *row, const unsigned char *ref,
    uint32_t width, int invert)
{
	int32_t a0 = -1;
	int colour = 0;
	uint64_t guard;

	/* A well formed line codes at most one element per pixel plus a
	 * little slack; the bound only exists to stop a damaged strip from
	 * spinning here. */
	for (guard = 0; guard < (uint64_t)width * 4 + 64; guard++) {
		int32_t a1, a2, b1, b2, from;
		int kind;
		int32_t d;

		if (a0 >= (int32_t)width) {
			/* A finished line is followed either by the next
			 * line's codes or by the end of block mark that
			 * ends the strip.  The reference reads that mark
			 * off before it moves on, so it has to be consumed
			 * here or the next line would start on the wrong
			 * bit. */
			size_t save = r->bitpos;

			if (g4_match_mode(r, &d, &kind) == 0 &&
			    kind == G4M_EOL)
				return 1;
			r->bitpos = save;
			return 0;
		}
		from = a0 < 0 ? 0 : a0;
		b1 = (int32_t)next_change(ref, width, a0, colour, invert);
		if (g4_match_mode(r, &d, &kind) < 0) {
			/* Bits that are in no table end the line the way the
			 * reference ends one it cannot read: the rest of the
			 * line keeps the colour it was in, and the next line
			 * picks up after these bits.  The reference says
			 * nothing here, because its seven bit main table has
			 * an entry for every pattern and so it never runs
			 * out of codes. */
			g4_paint(row, from, (int32_t)width, colour, invert);
			return 0;
		}
		if (kind == G4M_EOL) {
			/* An end of block ends the strip, not just this
			 * row, and the reference leaves the rows it never
			 * reached at the all colour 0 line they start
			 * from. */
			return 1;
		}
		if (kind == G4M_EXT) {
			/* The extension code ends the line: the reference
			 * gives the rest of it the colour it was in and
			 * stops, without reading the uncompressed data it
			 * stands for. */
			g4_paint(row, from, (int32_t)width, colour, invert);
			return 0;
		}
		if (kind == G4M_PASS) {
			b2 = (int32_t)next_any(ref, width, (uint32_t)b1, invert);
			if (b2 > (int32_t)width)
				b2 = (int32_t)width;
			if (b2 <= a0)
				b2 = a0 + 1;
			g4_paint(row, from, b2, colour, invert);
			a0 = b2;
			continue;
		}
		if (kind == G4M_VERT) {
			a1 = b1 + d;
			if (a1 < from)
				a1 = from;
			if (a1 > (int32_t)width)
				a1 = (int32_t)width;
			g4_paint(row, from, a1, colour, invert);
			a0 = a1;
			colour ^= 1;
			continue;
		}
		/* Horizontal: two runs, the second completing the pair. */
		{
			uint32_t r1, r2;

			if (g4_read_run(r, colour, &r1) < 0 ||
			    g4_read_run(r, colour ^ 1, &r2) < 0) {
				/* Same as an unreadable mode word: the line
				 * ends where the codes do. */
				g4_paint(row, from, (int32_t)width, colour,
				    invert);
				return 0;
			}
			a1 = from + (int32_t)r1;
			a2 = a1 + (int32_t)r2;
			if (a1 < from)
				a1 = from;
			if (a1 > (int32_t)width)
				a1 = (int32_t)width;
			if (a2 < a1)
				a2 = a1;
			if (a2 > (int32_t)width)
				a2 = (int32_t)width;
			g4_paint(row, from, a1, colour, invert);
			g4_paint(row, a1, a2, colour ^ 1, invert);
			a0 = a2;
		}
	}
	return -1;
}

/* Packs the byte per pixel rows into the one bit per sample, most significant
 * bit first, each row padded out to a byte, which is what a one bit sample
 * strip holds. */
static unsigned char *
g4_pack(const unsigned char *px, uint32_t width, uint32_t height)
{
	size_t stride = ((size_t)width + 7) / 8;
	unsigned char *out;
	uint32_t x, y;

	if ((out = calloc(stride * (size_t)height, 1)) == NULL)
		return NULL;
	for (y = 0; y < height; y++) {
		const unsigned char *src = px + (size_t)y * width;
		unsigned char *dst = out + (size_t)y * stride;

		for (x = 0; x < width; x++) {
			/* load_image widens a one bit sample the same way,
			 * 0 to 00 and 1 to ff, so the bit to store is just
			 * whether the sample came out nonzero. */
			if (src[x] != 0)
				dst[x >> 3] |= 0x80 >> (x & 7);
		}
	}
	return out;
}

/* Decode one strip.  Each strip starts over from the imaginary line, as T.4
 * says, so `rows` is the row count of this strip rather than of the image.
 * Rows come back packed and byte aligned, which is what the concatenating
 * caller needs in order to widen the samples a row at a time. */
int
g4_decode(const unsigned char *in, size_t inlen, uint32_t width, uint32_t rows,
    int invert, unsigned char **out, size_t *outlen)
{
	struct bitreader r;
	unsigned char *px, *imageline, *packed;
	uint32_t y;

	*out = NULL;
	*outlen = 0;
	if (width == 0 || rows == 0)
		return 0;
	if ((px = malloc((size_t)width * rows)) == NULL)
		return -1;
	if ((imageline = malloc(width)) == NULL) {
		free(px);
		return -1;
	}
	/* The line above the first is the same all colour 0 line the encoder
	 * assumes, so a stream this codec wrote decodes back to it.  Rows a
	 * stopped strip never reached keep that same colour. */
	memset(px, (unsigned char)(invert ? 0xff : 0x00), (size_t)width * rows);
	memset(imageline, (unsigned char)(invert ? 0xff : 0x00), width);

	r.p = in;
	r.len = inlen;
	r.bitpos = 0;
	for (y = 0; y < rows; y++) {
		int n;

		n = g4_decode_line(&r, px + (size_t)y * width,
		    y == 0 ? imageline : px + (size_t)(y - 1) * width, width,
		    invert);
		if (n < 0)
			goto fail;
		if (n > 0)
			break;
		memcpy(imageline, px + (size_t)y * width, width);
	}
	if ((packed = g4_pack(px, width, rows)) == NULL)
		goto fail;
	free(imageline);
	free(px);
	*out = packed;
	*outlen = ((size_t)width + 7) / 8 * (size_t)rows;
	return 0;
fail:
	free(imageline);
	free(px);
	return -1;
}
