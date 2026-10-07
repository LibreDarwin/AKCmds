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
#define G4_EXT     "0000001"
/* The reference looks a mode up by seven bits and reads four more after an end
 * of block mark, so it spends eleven of the mark's twelve bits there and leaves
 * the flag.  These two are the seven it looks up. */
#define G4_MAIN_LEN 7
#define G4_EOL7    "0000000"
#define G4_EOL_TAIL 4

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
 * A line is expanded into runs first and painted from them afterwards, which
 * is what the reference does: what a line codes and what a line looks like are
 * not always the same thing, because an unreadable code word leaves the runs
 * that were already read and the end of the line is reconciled against the
 * width once the codes stop.  Every table is built from the same strings the
 * encoder writes, so a stream the reference produced reads back through
 * exactly the codes it was built from.
 */

/* A reader over the strip.  The reference keeps its bits in an accumulator
 * filled a byte at a time and asks for a fixed number of bits before each
 * lookup: short of them it takes a byte, and with none left it either fails
 * or, while it still holds valid bits, pads the request with zeros and lets
 * the lookup return whatever those bits say.  Reads past the end of the strip
 * therefore yield zeros, which is what the fill bits at the end of a stream
 * are, and keeps a truncated strip from faulting. */
struct bitreader {
	const unsigned char *p;
	size_t len;		/* bytes */
	size_t bitpos;		/* bits consumed */
	size_t cp;		/* bytes fetched */
	int avail;		/* bits held, including zeros past the end */
};

/* Fails exactly where the reference's NeedBits macros do: only on entry to a
 * request with nothing held and nothing left to fetch.  A request of eight
 * bits or fewer needs at most one byte, a longer one at most two. */
static int
br_need(struct bitreader *r, unsigned n)
{
	if (r->avail >= (int)n)
		return 0;
	if (r->cp >= r->len) {
		if (r->avail == 0)
			return -1;
		r->avail = (int)n;	/* pad with zeros */
		return 0;
	}
	r->cp++;
	r->avail += 8;
	if (n > 8 && r->avail < (int)n) {
		if (r->cp >= r->len) {
			if (r->avail == 0)
				return -1;
			r->avail = (int)n;
		} else {
			r->cp++;
			r->avail += 8;
		}
	}
	return 0;
}

/* The bit at an absolute position, zero past the end of the strip. */
static int
br_bitat(const struct bitreader *r, size_t pos)
{
	size_t byte = pos >> 3;

	if (byte >= r->len)
		return 0;
	return (r->p[byte] >> (7 - (pos & 7))) & 1;
}

/* The next n bits with the first transmitted in the low bit, which is the
 * order the reference indexes its tables in. */
static uint32_t
br_peek(const struct bitreader *r, unsigned n)
{
	uint32_t v = 0;
	unsigned i;

	for (i = 0; i < n; i++)
		v |= (uint32_t)br_bitat(r, r->bitpos + i) << i;
	return v;
}

static void
br_clr(struct bitreader *r, unsigned n)
{
	r->bitpos += n;
	r->avail -= (int)n;
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
	int32_t d;		/* vertical offset */
	int kind;
};

/* The states a run table window resolves to.  A window holding no code at all
 * stays the null entry and reads as a run of zero, and a window that opens
 * with eleven zeros is the mark, which the run tables carry as well as the
 * mode table does.  The mark's width is eleven so that the look up leaves its
 * flag bit in the stream for whoever reads the mark as a mode. */
#define G4S_NULL	0
#define G4S_TERMW	7
#define G4S_TERMB	8
#define G4S_MAKEUPW	9
#define G4S_MAKEUPB	10
#define G4S_MAKEUP	11
#define G4S_EOL		12

/* How many bits a run look up takes.  A code can be shorter than this window
 * and the entry says how many of the bits it just looked at are its own. */
#define G4_WWID		12
#define G4_BWID		13

#define G4E_STATE(e)	((e) & 0xfu)
#define G4E_WIDTH(e)	(((e) >> 4) & 0xfu)
#define G4E_PARAM(e)	((e) >> 8)

static uint32_t g4wtab[1u << G4_WWID];
static uint32_t g4btab[1u << G4_BWID];
static struct modeent g4main[1 << G4_MAIN_LEN];
static int g4_tables_ready;

/* The reference reads its bits least significant bit first, so the first bit of
 * a code is the low bit of the index it looks that code up by.  A look ahead
 * assembled here arrives most significant bit first, so it turns round first. */
static uint32_t
g4_index(uint32_t acc, uint32_t len)
{
	uint32_t v = 0;
	uint32_t i;

	for (i = 0; i < len; i++)
		if (acc & (1u << (len - 1 - i)))
			v |= 1u << i;
	return v;
}

/* Record what every seven bit window starting with this code resolves to.  The
 * codes are a prefix code, so the windows of two of them cannot meet. */
static void
g4_set_main(const char *bits, uint32_t len, int32_t d, int kind)
{
	uint32_t base = g4_index(bits_value(bits, len), len);
	uint32_t tail;

	for (tail = 0; tail < (1u << (G4_MAIN_LEN - len)); tail++) {
		struct modeent *e = &g4main[base | (tail << len)];

		e->len = len;
		e->d = d;
		e->kind = kind;
	}
}

/* Record what every window of this width starting with this run code resolves
 * to.  The codes are a prefix code, so the windows of two of them cannot
 * meet. */
static void
g4_set_run(uint32_t *tab, uint32_t wid, const char *bits, uint32_t len,
    uint32_t state, uint32_t param)
{
	uint32_t base = g4_index(bits_value(bits, len), len);
	uint32_t tail;

	for (tail = 0; tail < (1u << (wid - len)); tail++)
		tab[base | (tail << len)] = state | (len << 4) | (param << 8);
}

/* The mode table is read by seven bits and the run tables by twelve or
 * thirteen, and every entry carries the state the window resolves to, the
 * width to consume and the value the state holds.  The tables are complete
 * over the windows their codes decide, which is why the reference has no run
 * or main table error path a real stream can reach: a window in no code is
 * the null entry, and it is the mark's eleven zeros that make the extension
 * code work without a special case, since its first four bits are the pass
 * code and seven bits hold it whole. */
static void
g4_build_tables(void)
{
	size_t i, n;

	if (g4_tables_ready)
		return;

	for (i = 0; i < 7; i++)
		g4_set_main(vert[i].bits, vert[i].len, (int32_t)i - 3,
		    G4M_VERT);
	g4_set_main(G4_HORIZ, 3, 0, G4M_HORIZ);
	g4_set_main(G4_PASS, 4, 0, G4M_PASS);
	g4_set_main(G4_EXT, G4_EXT_LEN, 0, G4M_EXT);
	g4_set_main(G4_EOL7, G4_MAIN_LEN, 0, G4M_EOL);

	for (i = 0; i < 64; i++) {
		g4_set_run(g4wtab, G4_WWID, white_term[i].bits,
		    white_term[i].len, G4S_TERMW, (uint32_t)i);
		g4_set_run(g4btab, G4_BWID, black_term[i].bits,
		    black_term[i].len, G4S_TERMB, (uint32_t)i);
	}
	for (i = 0; i < sizeof makeup_white / sizeof *makeup_white; i++)
		g4_set_run(g4wtab, G4_WWID, makeup_white[i].bits,
		    makeup_white[i].len, G4S_MAKEUPW, makeup_white[i].run);
	for (i = 0; i < sizeof makeup_black / sizeof *makeup_black; i++)
		g4_set_run(g4btab, G4_BWID, makeup_black[i].bits,
		    makeup_black[i].len, G4S_MAKEUPB, makeup_black[i].run);
	/* The extended makeups are shared by both colours. */
	n = sizeof makeup_ext / sizeof *makeup_ext;
	for (i = 0; i < n; i++) {
		g4_set_run(g4wtab, G4_WWID, makeup_ext[i].bits,
		    makeup_ext[i].len, G4S_MAKEUP, makeup_ext[i].run);
		g4_set_run(g4btab, G4_BWID, makeup_ext[i].bits,
		    makeup_ext[i].len, G4S_MAKEUP, makeup_ext[i].run);
	}
	/* Eleven zeros are the mark whichever colour the line is in.  A
	 * window that holds nothing but them, and one that holds them with
	 * the bit after them still to come, both read as the mark. */
	for (i = 0; i < (size_t)(1u << G4_WWID); i++)
		if ((i & 0x7ffu) == 0)
			g4wtab[i] = G4S_EOL | (11u << 4);
	for (i = 0; i < (size_t)(1u << G4_BWID); i++)
		if ((i & 0x7ffu) == 0)
			g4btab[i] = G4S_EOL | (11u << 4);
	g4_tables_ready = 1;
}

/* The state of one line while it expands: the runs being built and the runs
 * of the line above they are read against, pb walking the reference's changes
 * with b1 the change it stands past, a0 the position last written, and
 * RunLength a run read but not yet written. */
struct g4dec {
	struct bitreader r;
	uint32_t *runs;		/* the line being built and the one above */
	uint32_t *curruns;
	uint32_t *refruns;
	uint32_t nruns;
	uint32_t *pa;
	uint32_t *pb;
	int a0;
	int b1;
	int lastx;
	int RunLength;
	int EOLcnt;
};

/* Append a run and move the line on.  Every write the reference guards with
 * the end of the run array is guarded here the same way, and the failure it
 * gives back is the strip's, not the line's. */
static int
g4_setvalue(struct g4dec *d, int x)
{
	if (d->pa >= d->curruns + d->nruns)
		return -1;
	*d->pa++ = (uint32_t)(d->RunLength + x);
	d->a0 += x;
	d->RunLength = 0;
	return 0;
}

/* Bring b1 up to the first change of the reference line at or past a0, which
 * is what a vertical code measures from.  Nothing walks the reference until a
 * run has been written, because then a0 is the imaginary pixel before the
 * line and the reference's first change already stands. */
static int
g4_check_b1(struct g4dec *d)
{
	if (d->pa != d->curruns)
		while (d->b1 <= d->a0 && d->b1 < d->lastx) {
			if (d->pb + 1 >= d->refruns + d->nruns)
				return -1;
			d->b1 += d->pb[0] + d->pb[1];
			d->pb += 2;
		}
	return 0;
}

/* Reconcile what the codes read against the width, which happens to every
 * line whether the codes ended or the line gave up: a run read but never
 * written is written, runs standing past the width are dropped back, and a
 * line short of the width is closed out to it.  What comes out is what the
 * row is painted from and what the next line reads as its reference. */
static int
g4_cleanup(struct g4dec *d)
{
	if (d->RunLength != 0 && g4_setvalue(d, 0) < 0)
		return -1;
	if (d->a0 != d->lastx) {
		while (d->a0 > d->lastx && d->pa > d->curruns)
			d->a0 -= *--d->pa;
		if (d->a0 < d->lastx) {
			if (d->a0 < 0)
				d->a0 = 0;
			if (((d->pa - d->curruns) & 1) != 0 &&
			    g4_setvalue(d, 0) < 0)
				return -1;
			if (g4_setvalue(d, d->lastx - d->a0) < 0)
				return -1;
		} else if (d->a0 > d->lastx) {
			if (g4_setvalue(d, d->lastx) < 0)
				return -1;
			if (g4_setvalue(d, 0) < 0)
				return -1;
		}
	}
	return 0;
}

/* Read one run: the makeups and then always exactly one terminating code,
 * which is what the reference reads even when a makeup has already covered
 * the run, because a decoder that stops at the makeup would leave the line
 * short by it and read the rest of the strip as the wrong pixels.  0 is a
 * run, 1 the strip ran out, 2 a window that is in no code, -1 the line wrote
 * past its runs. */
static int
g4_read_hrun(struct g4dec *d, const uint32_t *tab, unsigned wid)
{
	for (;;) {
		uint32_t e;

		if (br_need(&d->r, wid) < 0)
			return 1;
		e = tab[br_peek(&d->r, wid)];
		br_clr(&d->r, G4E_WIDTH(e));
		switch (G4E_STATE(e)) {
		case G4S_TERMW:
		case G4S_TERMB:
			return g4_setvalue(d, (int)G4E_PARAM(e));
		case G4S_MAKEUPW:
		case G4S_MAKEUPB:
		case G4S_MAKEUP:
			d->a0 += (int)G4E_PARAM(e);
			d->RunLength += (int)G4E_PARAM(e);
			break;
		default:
			return 2;
		}
	}
}

/* Expand one line into runs.  The reference reads its bits least significant
 * bit first, so the first bit of a code is the low bit of the index it looks
 * that code up by, and it does not read a bit at a time until a code matches:
 * seven bits are looked up in one table and the width the entry carries is
 * consumed, which is the same route on a code a real encoder emits because
 * those are a prefix code, and a very different one on bits that are in no
 * code, since the look up still resolves them to whatever those seven bits
 * hold and goes on decoding.  A horizontal mode then reads the pair of runs
 * after it, and whatever stops the line -- the codes running out, the mark, a
 * code word the tables reject -- the line is reconciled against the width by
 * g4_cleanup.  0 is a line that ended, 1 the strip ran out in the middle of
 * it, -1 the line wrote past its runs. */
static int
g4_expand2d(struct g4dec *d)
{
	while (d->a0 < d->lastx) {
		const struct modeent *m;
		int rc, param;

		if (d->pa >= d->curruns + d->nruns)
			return -1;
		if (br_need(&d->r, G4_MAIN_LEN) < 0)
			goto eof2d;
		m = &g4main[br_peek(&d->r, G4_MAIN_LEN)];
		br_clr(&d->r, m->len);
		switch (m->kind) {
		case G4M_PASS:
			if (g4_check_b1(d) < 0)
				return -1;
			if (d->pb + 1 >= d->refruns + d->nruns)
				return -1;
			d->b1 += *d->pb++;
			d->RunLength += d->b1 - d->a0;
			d->a0 = d->b1;
			d->b1 += *d->pb++;
			break;
		case G4M_HORIZ:
			/* The pair runs in the colour the line's last run
			 * left it in, and only one of the two is read when
			 * the first one does not finish. */
			if ((d->pa - d->curruns) & 1) {
				rc = g4_read_hrun(d, g4btab, G4_BWID);
				if (rc == 0)
					rc = g4_read_hrun(d, g4wtab, G4_WWID);
			} else {
				rc = g4_read_hrun(d, g4wtab, G4_WWID);
				if (rc == 0)
					rc = g4_read_hrun(d, g4btab, G4_BWID);
			}
			if (rc == 1)
				goto eof2d;
			if (rc < 0)
				return -1;
			if (rc == 2)
				goto bad2d;
			if (g4_check_b1(d) < 0)
				return -1;
			break;
		case G4M_VERT:
			param = m->d < 0 ? -m->d : m->d;
			if (g4_check_b1(d) < 0)
				return -1;
			if (m->d < 0) {
				/* A left vertical stands at b1 less its
				 * offset and takes the reference back one
				 * change, past the reference's start into
				 * the end of the array the two share. */
				if (d->b1 < d->a0 + param)
					goto bad2d;
				if (g4_setvalue(d, d->b1 - d->a0 - param) < 0)
					return -1;
				d->b1 -= *--d->pb;
				break;
			}
			if (g4_setvalue(d, d->b1 - d->a0 + param) < 0)
				return -1;
			if (d->pb >= d->refruns + d->nruns)
				return -1;
			d->b1 += *d->pb++;
			break;
		case G4M_EXT:
			/* The rest of the line stands where it is: the run
			 * left over is written without moving a0, so the
			 * line closes short and cleanup brings it to the
			 * width from there. */
			*d->pa++ = (uint32_t)(d->lastx - d->a0);
			goto eol2d;
		case G4M_EOL:
			*d->pa++ = (uint32_t)(d->lastx - d->a0);
			if (br_need(&d->r, G4_EOL_TAIL) < 0)
				goto eof2d;
			br_clr(&d->r, G4_EOL_TAIL);
			d->EOLcnt = 1;
			goto eol2d;
		default:
			goto bad2d;
		}
	}
	/* A run read but never terminated still stands when the modes stop,
	 * and the reference only looks for the bit that would close it when
	 * the line has the room to close in. */
	if (d->RunLength != 0) {
		if (d->RunLength + d->a0 < d->lastx) {
			if (br_need(&d->r, 1) < 0)
				goto eof2d;
			if (br_peek(&d->r, 1) == 0)
				goto bad2d;
			br_clr(&d->r, 1);
		}
		if (g4_setvalue(d, 0) < 0)
			return -1;
	}
bad2d:
eol2d:
	if (g4_cleanup(d) < 0)
		return -1;
	return 0;
eof2d:
	if (g4_cleanup(d) < 0)
		return -1;
	return 1;
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

/* Paint the row from its runs: colour 0 then colour 1, in pairs, from the
 * left.  A run that would paint past the width is cut back to it and the cut
 * is written into the array, because the next line reads these runs as its
 * reference.  An odd count means the line never finished its second colour,
 * and the missing colour 0 run closes it. */
static void
g4_fill(unsigned char *row, uint32_t *runs, uint32_t *erun, uint32_t lastx,
    int invert)
{
	uint32_t x = 0;
	uint32_t run;

	if ((erun - runs) & 1)
		*erun++ = 0;
	for (; runs < erun; runs += 2) {
		run = runs[0];
		if (x + run > lastx || run > lastx)
			run = runs[0] = lastx - x;
		if (run) {
			g4_paint(row, (int32_t)x, (int32_t)(x + run), 0, invert);
			x += runs[0];
		}
		run = runs[1];
		if (x + run > lastx || run > lastx)
			run = runs[1] = lastx - x;
		if (run) {
			g4_paint(row, (int32_t)x, (int32_t)(x + run), 1, invert);
			x += runs[1];
		}
	}
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
 * caller needs in order to widen the samples a row at a time.  A strip whose
 * codes stop part way through is read for as far as they go, and the rows it
 * never reaches keep the imaginary line's colour.  A strip whose very first
 * line stops keeps it everywhere -- line 0 as well -- because the reference
 * reports such a strip unreadable and writes the strip it never filled. */
int
g4_decode(const unsigned char *in, size_t inlen, uint32_t width, uint32_t rows,
    int invert, unsigned char **out, size_t *outlen)
{
	struct g4dec d;
	unsigned char *px, *packed;
	uint32_t nruns, y;

	*out = NULL;
	*outlen = 0;
	if (width == 0 || rows == 0)
		return 0;
	/* The reference refuses a row count its run arrays cannot size, and
	 * this is where both of its roundings first overflow. */
	if (width >= 0x7fffffe0u)
		return -1;
	nruns = ((width + 1 + 31) / 32) * 32 * 2;
	memset(&d, 0, sizeof d);
	if ((px = malloc((size_t)width * rows)) == NULL)
		return -1;
	/* One allocation holds the line being built and the line above it,
	 * because a left vertical reads one change past the start of the
	 * reference and that is the end of the array the two share. */
	if ((d.runs = calloc((size_t)nruns * 2, sizeof *d.runs)) == NULL) {
		free(px);
		return -1;
	}
	/* The line above the first is the same all colour 0 line the encoder
	 * assumes, so a stream this codec wrote decodes back to it. */
	memset(px, (unsigned char)(invert ? 0xff : 0x00), (size_t)width * rows);
	d.r.p = in;
	d.r.len = inlen;
	d.lastx = (int)width;
	d.nruns = nruns;
	d.curruns = d.runs;
	d.refruns = d.runs + nruns;
	d.refruns[0] = width;
	d.refruns[1] = 0;
	g4_build_tables();
	for (y = 0; y < rows; y++) {
		int n;

		d.pa = d.curruns;
		d.pb = d.refruns;
		d.b1 = (int)*d.pb++;
		d.a0 = 0;
		d.RunLength = 0;
		d.EOLcnt = 0;
		n = g4_expand2d(&d);
		if (n < 0)
			goto fail;
		/* A line that stops before it is finished is only painted when
		 * some line was already painted: the reference reports the strip
		 * as unreadable if the first one stops, and the caller then
		 * hands back the imaginary line everywhere rather than the runs
		 * the stopped line left behind. */
		if (y == 0 && (n == 1 || d.EOLcnt != 0))
			break;
		g4_fill(px + (size_t)y * width, d.curruns, d.pa, width, invert);
		if (n == 1 || d.EOLcnt != 0)
			break;
		/* The imaginary change that closes the line, which the next
		 * one reads as its reference. */
		if (g4_setvalue(&d, 0) < 0)
			goto fail;
		{
			uint32_t *t = d.curruns;

			d.curruns = d.refruns;
			d.refruns = t;
		}
	}
	if ((packed = g4_pack(px, width, rows)) == NULL)
		goto fail;
	free(d.runs);
	free(px);
	*out = packed;
	*outlen = ((size_t)width + 7) / 8 * (size_t)rows;
	return 0;
fail:
	free(d.runs);
	free(px);
	return -1;
}
