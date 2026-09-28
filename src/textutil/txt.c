/*
 * textutil - plain text input and output.
 *
 * Input is decoded to UTF-8.  The reference tool detects a leading byte order
 * mark and honours it: a UTF-8 mark is dropped and the rest read as UTF-8, and
 * a UTF-16 or UTF-32 mark selects that encoding.  Absent a mark, the buffer is
 * read as UTF-8 if every byte of it is well formed, and as Mac OS Roman if any
 * byte is not: it does not repair a buffer that is partly UTF-8 but reads all
 * of it as the fallback.  The mark itself is consumed, so it never reaches
 * -info's length or contents, and it is not written back out.
 *
 * Output is the decoded text verbatim, so a round trip of unmarked UTF-8 is
 * the identity.  -encoding names one of the Unicode encodings and is applied
 * here, re-encoding the decoded text just before it is written; the reference
 * tool's single byte encodings need conversion tables this port does not
 * carry, and -encoding rejects those rather than writing UTF-8 in their place.
 * See src/textutil/NOTES.md.
 *
 * Copyright (C) 2026, LibreDarwin
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include <stdio.h>
#include <stdlib.h>
#include <errno.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>

#include "textutil.h"

static void *
xmalloc(size_t n)
{
	void *p = malloc(n ? n : 1);

	if (p == NULL) {
		fprintf(stderr, "textutil: out of memory\n");
		exit(1);
	}
	return p;
}

void
tu_doc_free(tu_doc_t *d)
{
	free(d->text);
	d->text = NULL;
	d->len = 0;
	d->nchars = 0;
}

/* How many bytes the character starting at p occupies, assuming valid UTF-8.
 * Malformed input is consumed one byte at a time so that a stray byte never
 * swallows its successor. */
size_t
tu_utf8_seq(const unsigned char *p, size_t avail)
{
	unsigned char c = p[0];
	size_t n;

	if (c < 0x80)
		return 1;
	if ((c & 0xE0) == 0xC0)
		n = 2;
	else if ((c & 0xF0) == 0xE0)
		n = 3;
	else if ((c & 0xF8) == 0xF0)
		n = 4;
	else
		return 1;
	if (n > avail)
		return 1;
	for (size_t i = 1; i < n; i++)
		if ((p[i] & 0xC0) != 0x80)
			return 1;
	return n;
}

/* Decode UTF-16 or UTF-32 to UTF-8.  Returns the byte length written, or
 * (size_t)-1 if the value is not a Unicode scalar and so has no UTF-8 form. */
static size_t
decode_unit(char *out, unsigned long cp)
{
	if (cp > 0x10FFFF || (cp >= 0xD800 && cp <= 0xDFFF))
		return (size_t)-1;
	if (cp < 0x80) {
		out[0] = (char)cp;
		return 1;
	}
	if (cp < 0x800) {
		out[0] = (char)(0xC0 | (cp >> 6));
		out[1] = (char)(0x80 | (cp & 0x3F));
		return 2;
	}
	if (cp < 0x10000) {
		out[0] = (char)(0xE0 | (cp >> 12));
		out[1] = (char)(0x80 | ((cp >> 6) & 0x3F));
		out[2] = (char)(0x80 | (cp & 0x3F));
		return 3;
	}
	out[0] = (char)(0xF0 | (cp >> 18));
	out[1] = (char)(0x80 | ((cp >> 12) & 0x3F));
	out[2] = (char)(0x80 | ((cp >> 6) & 0x3F));
	out[3] = (char)(0x80 | (cp & 0x3F));
	return 4;
}

/* Mac OS Roman for the bytes above 0x7F, taken from the reference tool one
 * byte at a time.  The half below is ASCII and stands for itself.  Every value
 * is a Unicode scalar, so decode_unit() accepts all of them. */
static const unsigned short mac_roman_high[128] = {
	0x00c4, 0x00c5, 0x00c7, 0x00c9, 0x00d1, 0x00d6, 0x00dc, 0x00e1, /* 80 */
	0x00e0, 0x00e2, 0x00e4, 0x00e3, 0x00e5, 0x00e7, 0x00e9, 0x00e8, /* 88 */
	0x00ea, 0x00eb, 0x00ed, 0x00ec, 0x00ee, 0x00ef, 0x00f1, 0x00f3, /* 90 */
	0x00f2, 0x00f4, 0x00f6, 0x00f5, 0x00fa, 0x00f9, 0x00fb, 0x00fc, /* 98 */
	0x2020, 0x00b0, 0x00a2, 0x00a3, 0x00a7, 0x2022, 0x00b6, 0x00df, /* a0 */
	0x00ae, 0x00a9, 0x2122, 0x00b4, 0x00a8, 0x2260, 0x00c6, 0x00d8, /* a8 */
	0x221e, 0x00b1, 0x2264, 0x2265, 0x00a5, 0x00b5, 0x2202, 0x2211, /* b0 */
	0x220f, 0x03c0, 0x222b, 0x00aa, 0x00ba, 0x03a9, 0x00e6, 0x00f8, /* b8 */
	0x00bf, 0x00a1, 0x00ac, 0x221a, 0x0192, 0x2248, 0x2206, 0x00ab, /* c0 */
	0x00bb, 0x2026, 0x00a0, 0x00c0, 0x00c3, 0x00d5, 0x0152, 0x0153, /* c8 */
	0x2013, 0x2014, 0x201c, 0x201d, 0x2018, 0x2019, 0x00f7, 0x25ca, /* d0 */
	0x00ff, 0x0178, 0x2044, 0x20ac, 0x2039, 0x203a, 0xfb01, 0xfb02, /* d8 */
	0x2021, 0x00b7, 0x201a, 0x201e, 0x2030, 0x00c2, 0x00ca, 0x00c1, /* e0 */
	0x00cb, 0x00c8, 0x00cd, 0x00ce, 0x00cf, 0x00cc, 0x00d3, 0x00d4, /* e8 */
	0xf8ff, 0x00d2, 0x00da, 0x00db, 0x00d9, 0x0131, 0x02c6, 0x02dc, /* f0 */
	0x00af, 0x02d8, 0x02d9, 0x02da, 0x00b8, 0x02dd, 0x02db, 0x02c7, /* f8 */
};

/* Whether every byte of the buffer belongs to a well formed UTF-8 sequence.
 * This is the same notion of well formedness tu_utf8_strict() applies, asked
 * as a question about the whole buffer rather than about the sequence at one
 * place.  tu_utf8_strict() gives a well formed sequence the full length its
 * lead byte calls for and a malformed one less, so the two disagree only where
 * a sequence is not well formed, which is what this is looking for. */
static int
plain_is_utf8(const unsigned char *p, size_t n)
{
	for (size_t i = 0; i < n;) {
		size_t want, got;
		unsigned char c = p[i];

		if (c < 0x80)
			want = 1;
		else if ((c & 0xE0) == 0xC0)
			want = 2;
		else if ((c & 0xF0) == 0xE0)
			want = 3;
		else
			want = 4;
		/* A malformed sequence is reported as U+FFFD, which a well
		 * formed one can also be, so the code point says nothing and
		 * the length says everything. */
		(void)tu_utf8_strict(p + i, n - i, &got);
		if (got < want)
			return 0;
		i += got;
	}
	return 1;
}

/* Whether the reference tool takes a stretch of text as Mac OS Roman at all.
 *
 * Mac OS Roman is the fallback for text that is not UTF-8, and the test is
 * made once over the whole text before any of it is decoded.  What counts as
 * "not UTF-8" is narrower than it looks: a byte that is not part of a well
 * formed sequence condemns the text, but only 0xA9 is excused.  So a buffer
 * holding nothing but ASCII and stray 0xA9 bytes is still read as UTF-8, with
 * each of those bytes coming back as U+FFFD: "\251" is U+FFFD, "\251\251" is
 * two of them, and "A\251A" is U+FFFD between two A's.  Every other byte above
 * 0x7F that stands outside a well formed sequence condemns the buffer, so
 * "\251\200" is ©Ä.
 *
 * The well formed part is neither excused nor condemned.  It only ever keeps
 * the bytes around it from condemning the text, and once they have, it is read
 * from the table like any other: the valid é in "a\303\251\342b" comes back as
 * √©, the two characters its own bytes spell, because the malformed \342 has
 * condemned the text by then.  One sequence is a poor guide in the other
 * direction, the overlong form, which is not a sequence at all: "\300\200" is
 * ¿Ä, both bytes read from the table.
 *
 * The decision is about the whole text and not about the run of bytes one 0xA9
 * happens to sit in, so 0xA9 beside a condemning byte is © like any other, and
 * 0xA9 on a line of a file holding every byte from 0x80 up is © as well. */
static int
is_mac_roman(const unsigned char *p, size_t n)
{
	for (size_t i = 0; i < n;) {
		unsigned char c = p[i];
		size_t want, got;

		if (c < 0x80) {
			i++;
			continue;
		}
		if (c == 0xA9) {
			/* The one byte that is repaired rather than believed. */
			i++;
			continue;
		}
		if ((c & 0xE0) == 0xC0)
			want = 2;
		else if ((c & 0xF0) == 0xE0)
			want = 3;
		else
			want = 4;
		/* A malformed sequence is reported as U+FFFD, which a well
		 * formed one can also be, so the code point says nothing and
		 * the length says everything.  A short length here means this
		 * byte is not part of a well formed sequence, and so condemns
		 * the text. */
		(void)tu_utf8_strict(p + i, n - i, &got);
		if (got < want)
			return 1;
		i += got;
	}
	return 0;
}

/* Read a stretch of text as Mac OS Roman and write it out as UTF-8.  One byte
 * in is one character out, so the character count is the byte count.  A Mac OS
 * Roman character is never more than three bytes of UTF-8, so this can grow
 * the buffer threefold and no further. */
static size_t
decode_mac_roman(const unsigned char *raw, size_t rawlen, char **out)
{
	char *text = xmalloc(rawlen * 3 + 1);
	size_t len = 0;

	for (size_t i = 0; i < rawlen; i++) {
		unsigned long cp;

		if (raw[i] < 0x80)
			cp = raw[i];
		else
			cp = mac_roman_high[raw[i] - 0x80];
		len += decode_unit(text + len, cp);
	}
	text[len] = '\0';
	*out = text;
	return len;
}

/* Read a stretch of text as UTF-8 and write it out as UTF-8, repairing what is
 * not well formed, and report how many characters it holds.  The bytes that
 * get repaired are the ones is_mac_roman() excused, and each is one U+FFFD
 * however long the run it started was.  A well formed sequence is kept as it
 * stands, so no buffer ever grows here: the repair replaces bytes with three
 * only when the byte it replaces stood alone.  A four byte sequence is above
 * the BMP and so stands for two UTF-16 units, which is what -info counts. */
static size_t
decode_utf8_loose(const unsigned char *raw, size_t rawlen, char **out,
    size_t *nchars)
{
	char *text = xmalloc(rawlen * 3 + 1);
	size_t len = 0;

	*nchars = 0;
	for (size_t i = 0; i < rawlen;) {
		unsigned long cp;
		size_t got;

		if (raw[i] < 0x80) {
			cp = raw[i];
			got = 1;
		} else {
			cp = tu_utf8_strict(raw + i, rawlen - i, &got);
			if (got == 1) {
				/* Either a byte that cannot begin a character or
				 * a sequence that goes wrong partway.  Both
				 * cost one U+FFFD, and either way the bytes
				 * that were not part of a well formed
				 * sequence are not carried over. */
				cp = 0xFFFD;
				got = 1;
			} else {
				*nchars += got == 4 ? 2 : 1;
				memcpy(text + len, raw + i, got);
				len += got;
				i += got;
				continue;
			}
		}
		len += decode_unit(text + len, cp);
		(*nchars)++;
		i += got;
	}
	text[len] = '\0';
	*out = text;
	return len;
}


/* The wide code unit of "width" bytes at p, in the byte order the mark named. */
static unsigned long
unit_at(const unsigned char *p, size_t width, int be, size_t i)
{
	unsigned long v = 0;
	size_t k;

	if (be) {
		for (k = 0; k < width; k++)
			v = (v << 8) | p[i * width + k];
	} else {
		for (k = width; k > 0; k--)
			v = (v << 8) | p[i * width + k - 1];
	}
	return v;
}

int
tu_check_file(const char *path)
{
	struct stat st;

	if (stat(path, &st) != 0)
		return errno == EACCES || errno == EPERM ? TU_READ_DENIED :
		    TU_READ_MISSING;
	if (S_ISDIR(st.st_mode))
		return TU_READ_UNOPENABLE;
	return 0;
}

int
tu_read_plain(const char *path, tu_doc_t *out)
{
	FILE *fp;
	unsigned char *raw = NULL;
	size_t rawlen = 0, cap = 0;
	int rc, why;

	out->text = NULL;
	out->len = 0;
	out->nchars = 0;

	/* A directory is not a document, and reading one would look like an
	 * empty file rather than a failure. */
	if ((why = tu_check_file(path)) != 0)
		return why;
	if ((fp = fopen(path, "rb")) == NULL)
		return errno == EACCES ? TU_READ_DENIED : TU_READ_MISSING;
	for (;;) {
		size_t got;

		if (rawlen + 65536 > cap) {
			unsigned char *grown;

			cap = cap ? cap * 2 : 131072;
			grown = realloc(raw, cap);
			if (grown == NULL) {
				fclose(fp);
				free(raw);
				return TU_READ_UNOPENABLE;
			}
			raw = grown;
		}
		got = fread(raw + rawlen, 1, 65536, fp);
		rawlen += got;
		if (got < 65536)
			break;
	}
	/* A short read means the stream failed rather than ended. */
	if (ferror(fp)) {
		why = errno == EACCES ? TU_READ_DENIED : TU_READ_UNOPENABLE;
		fclose(fp);
		free(raw);
		return why;
	}
	fclose(fp);

	rc = tu_decode_plain(raw, rawlen, out);
	free(raw);
	return rc;
}

/* The three read diagnostics, in the reference tool's wording.  Two of them
 * quote the last path component in U+201C and U+201D double quotes. */
void
tu_read_error(const char *path, int why)
{
	const char *base = strrchr(path, '/');

	base = base != NULL ? base + 1 : path;
	switch (why) {
	case TU_READ_DENIED:
		fprintf(stderr,
		    "Error reading %s.  You don\xe2\x80\x99t have permission.\n",
		    path);
		break;
	case TU_READ_UNOPENABLE:
		fprintf(stderr, "Error reading %s.  The file "
		    "\xe2\x80\x9c%s\xe2\x80\x9d couldn\xe2\x80\x99t be opened.\n",
		    path, base);
		break;
	case TU_READ_WRONGFMT:
		fprintf(stderr, "Error reading %s.  The file isn\xe2\x80\x99t "
		    "in the correct format.\n", path);
		break;
	default:
		fprintf(stderr,
		    "Error reading %s.  The file doesn\xe2\x80\x99t exist.\n",
		    path);
		break;
	}
}

/* Decode a plain-text buffer that is already in memory.  The encoding comes
 * from a leading byte order mark, exactly as for a file, so -stdin is
 * interpreted the same way whether it was redirected from a file or not. */
int
tu_decode_plain(const void *buf, size_t rawlen, tu_doc_t *out)
{
	const unsigned char *raw = buf;
	char *text = NULL;
	size_t len = 0, nchars = 0;
	size_t wide = 0;		/* bytes per code unit once decoded */
	size_t skip = 0;		/* UTF-8 mark, dropped only if it fits */
	int be = 0;			/* that code unit is big endian */

	out->text = NULL;
	out->len = 0;
	out->nchars = 0;

	/* Pick the encoding from a leading mark.  A UTF-16 or UTF-32 mark names
	 * the encoding outright, so it is dropped here and its text read that way
	 * whatever else it holds.  A UTF-8 mark only says what the text would be
	 * anyway, so it is left in place for the plain text path below to weigh
	 * with the rest of the buffer. */
	if (rawlen >= 4 && raw[0] == 0xFF && raw[1] == 0xFE &&
	    raw[2] == 0x00 && raw[3] == 0x00) {
		raw += 4; rawlen -= 4; wide = 4;
	} else if (rawlen >= 4 && raw[0] == 0x00 && raw[1] == 0x00 &&
	    raw[2] == 0xFE && raw[3] == 0xFF) {
		raw += 4; rawlen -= 4; wide = 4; be = 1;
	} else if (rawlen >= 3 && raw[0] == 0xEF && raw[1] == 0xBB &&
	    raw[2] == 0xBF) {
		skip = 3; wide = 1;
	} else if (rawlen >= 2 && raw[0] == 0xFF && raw[1] == 0xFE) {
		raw += 2; rawlen -= 2; wide = 2;
	} else if (rawlen >= 2 && raw[0] == 0xFE && raw[1] == 0xFF) {
		raw += 2; rawlen -= 2; wide = 2; be = 1;
	} else {
		wide = 1;
	}

	if (wide == 1 && plain_is_utf8(raw, rawlen)) {
		/* Already UTF-8; count characters without rewriting.  A four byte
		 * sequence is above the BMP and so stands for two UTF-16 units,
		 * which is what the reference tool counts.  Only a buffer that is
		 * UTF-8 all the way through gets its mark dropped, which is why the
		 * mark is counted here rather than in the table above. */
		const unsigned char *p = raw + skip;
		size_t n = rawlen - skip;

		text = xmalloc(n + 1);
		memcpy(text, p, n);
		text[n] = '\0';
		for (size_t i = 0; i < n; i += tu_utf8_seq((unsigned char *)text + i,
		    n - i))
			nchars += tu_utf8_seq((unsigned char *)text + i,
			    n - i) == 4 ? 2 : 1;
		len = n;
	} else if (wide == 1) {
		size_t n = rawlen;
		int macroman;

		/* One byte anywhere in the buffer that is not UTF-8 and is not
		 * 0xA9, and then the reference tool reads all of it as Mac OS
		 * Roman rather than repairing the good part: the valid é in
		 * "a\c3\xa9\xe2b" comes back as √©, the two characters its own
		 * bytes spell, and not as é.  So this is a whole buffer decision,
		 * made before any of it is decoded. */
		macroman = is_mac_roman(raw + skip, rawlen - skip);
		if (skip != 0 && !macroman) {
			/* A UTF-8 mark is not part of the text the 0xA9 rule is
			 * judged on, and when that rule fires the mark is not part
			 * of the result either, because then the text is just what
			 * followed it.  With any other byte above 0x7F in it the
			 * mark is only more text, and is read along with the rest. */
			raw += skip;
			n -= skip;
		}
		if (macroman) {
			len = decode_mac_roman(raw, n, &text);
			nchars = n;
		} else {
			len = decode_utf8_loose(raw, n, &text, &nchars);
		}
	} else {
		/* Wide input: a surrogate pair in UTF-16 is one character, and a
		 * lone surrogate has no UTF-8 form, so it is dropped. */
		size_t units = rawlen / wide;
		size_t outcap = units * 4 + 1;

		text = xmalloc(outcap);
		for (size_t i = 0; i < units; i++) {
			unsigned long cp;

			if (wide == 2) {
				unsigned long a = unit_at(raw, wide, be, i);
				unsigned long b = i + 1 < units ?
				    unit_at(raw, wide, be, i + 1) : 0;

				if (i + 1 < units &&
				    a >= 0xD800 && a <= 0xDBFF &&
				    b >= 0xDC00 && b <= 0xDFFF) {
					cp = 0x10000 + ((a - 0xD800) << 10) +
					    (b - 0xDC00);
					i++;
				} else if (a >= 0xD800 && a <= 0xDFFF) {
					continue;	/* unpaired */
				} else {
					cp = a;
				}
			} else {
				cp = unit_at(raw, wide, be, i);
			}
			{
				size_t n = decode_unit(text + len, cp);

				if (n == (size_t)-1)
					continue;
				len += n;
				/* Above the BMP means a surrogate pair. */
				nchars += cp >= 0x10000 ? 2 : 1;
			}
		}
		text[len] = '\0';
	}
	out->text = text;
	out->len = len;
	out->nchars = nchars;
	return 0;
}

/* -encoding argument to one of this port's encodings, or -1.  Matching is case
 * insensitive, and the reference tool also accepts an NSStringEncoding number,
 * of which only 4, UTF-8, is one this port writes. */
int
tu_encoding_parse(const char *s)
{
	static const struct {
		const char *name;
		tu_encoding_t enc;
	} table[] = {
		{ "utf8",	TU_ENC_UTF8 },
		{ "utf-8",	TU_ENC_UTF8 },
		{ "utf-16",	TU_ENC_UTF16 },
		{ "utf16",	TU_ENC_UTF16 },
		{ "utf-16le",	TU_ENC_UTF16LE },
		{ "utf16le",	TU_ENC_UTF16LE },
		{ "utf-16be",	TU_ENC_UTF16BE },
		{ "utf16be",	TU_ENC_UTF16BE },
		{ "utf-32",	TU_ENC_UTF32 },
		{ "utf32",	TU_ENC_UTF32 },
		{ "utf-32le",	TU_ENC_UTF32LE },
		{ "utf32le",	TU_ENC_UTF32LE },
		{ "utf-32be",	TU_ENC_UTF32BE },
		{ "utf32be",	TU_ENC_UTF32BE },
	};
	size_t i;

	if (s == NULL)
		return -1;
	/* The reference tool takes an NSStringEncoding number here, and reads it
	 * leniently, so a leading run of digits is a number even with trailing
	 * rubbish behind it.  It accepts every number it has a table for, so a
	 * number is never an invalid encoding; only 4, which is UTF-8, is one
	 * this port can write, and the rest are accepted here and declined when
	 * there is output to write. */
	if (s[0] >= '0' && s[0] <= '9') {
		unsigned long v = 0;
		const char *p = s;

		while (*p >= '0' && *p <= '9')
			v = v * 10 + (unsigned long)(*p++ - '0');
		return v == 4 ? TU_ENC_UTF8 : TU_ENC_UNSUPPORTED;
	}
	for (i = 0; i < sizeof(table) / sizeof(table[0]); i++) {
		const char *a = s, *b = table[i].name;

		while (*a != '\0' && *b != '\0') {
			unsigned char ca = (unsigned char)*a, cb = (unsigned char)*b;

			if (ca >= 'A' && ca <= 'Z')
				ca = (unsigned char)(ca - 'A' + 'a');
			if (cb >= 'A' && cb <= 'Z')
				cb = (unsigned char)(cb - 'A' + 'a');
			if (ca != cb)
				break;
			a++;
			b++;
		}
		if (*a == '\0' && *b == '\0')
			return (int)table[i].enc;
	}
	return -1;
}

/* The canonical lower case name the HTML writer's charset attribute carries,
 * which is not always the spelling that was asked for: "utf8" and the
 * NSStringEncoding number 4 both come out as "utf-8". */
const char *
tu_encoding_name(tu_encoding_t enc)
{
	switch (enc) {
	case TU_ENC_UTF16:
		return "utf-16";
	case TU_ENC_UTF16LE:
		return "utf-16le";
	case TU_ENC_UTF16BE:
		return "utf-16be";
	case TU_ENC_UTF32:
		return "utf-32";
	case TU_ENC_UTF32LE:
		return "utf-32le";
	case TU_ENC_UTF32BE:
		return "utf-32be";
	default:
		return "utf-8";
	}
}

/* The code point a UTF-8 sequence starts, and how many bytes it occupies.  A
 * malformed byte stands for itself, so that re-encoding text that did not come
 * from tu_decode_plain() still terminates and reproduces the input. */
static unsigned long
cp_at(const unsigned char *p, size_t avail, size_t *width)
{
	size_t n = tu_utf8_seq(p, avail);
	unsigned long cp;
	size_t k;

	if (n > avail)
		n = avail ? avail : 1;
	*width = n;
	if (n == 1)
		return p[0];
	cp = (unsigned long)(p[0] & (0xFF >> (n + 1)));
	for (k = 1; k < n; k++)
		cp = (cp << 6) | (unsigned long)(p[k] & 0x3F);
	return cp;
}

/* The next code point in the UTF-8 at p, and how many bytes of it stand there.
 * Unlike tu_utf8_seq() this does not pass a malformed byte through as itself:
 * a byte that cannot begin a character is U+FFFD on its own, and a sequence
 * that starts well and then goes wrong is U+FFFD for the part that was well
 * formed, with the byte that spoilt it left to be looked at again.  A sequence
 * the input simply stops in the middle of is U+FFFD for all of it.  Those are
 * the rules that make one bad byte cost one U+FFFD and the text around it
 * survive, which is what the reference tool does with a command line
 * argument.  avail is how many bytes are there; it is never less than one. */
unsigned long
tu_utf8_strict(const unsigned char *p, size_t avail, size_t *width)
{
	unsigned long cp;
	unsigned char lo, hi;
	size_t n, k;

	if (p[0] < 0x80) {
		*width = 1;
		return p[0];
	}
	/* The second byte is not always free of the code point's own limits: E0
	 * rules out the overlong forms, ED the surrogates, F0 and F4 the ends of
	 * the range, so for four of the lead bytes it has a range of its own. */
	if (p[0] >= 0xc2 && p[0] <= 0xdf) {
		n = 2; lo = 0x80; hi = 0xbf;
	} else if (p[0] == 0xe0) {
		n = 3; lo = 0xa0; hi = 0xbf;
	} else if (p[0] == 0xed) {
		n = 3; lo = 0x80; hi = 0x9f;
	} else if (p[0] >= 0xe1 && p[0] <= 0xef) {
		n = 3; lo = 0x80; hi = 0xbf;
	} else if (p[0] == 0xf0) {
		n = 4; lo = 0x90; hi = 0xbf;
	} else if (p[0] == 0xf4) {
		n = 4; lo = 0x80; hi = 0x8f;
	} else if (p[0] >= 0xf1 && p[0] <= 0xf3) {
		n = 4; lo = 0x80; hi = 0xbf;
	} else {
		*width = 1;
		return 0xfffd;
	}
	if (avail < 2 || p[1] < lo || p[1] > hi) {
		*width = 1;
		return 0xfffd;
	}
	for (k = 2; k < n; k++)
		if (avail <= k || (p[k] & 0xc0) != 0x80) {
			*width = k;
			return 0xfffd;
		}
	*width = n;
	cp = (unsigned long)(p[0] & (0xffu >> (n + 1)));
	for (k = 1; k < n; k++)
		cp = (cp << 6) | (unsigned long)(p[k] & 0x3fu);
	return cp;
}

/* Store one code unit of "width" bytes at the running offset, in the byte order
 * asked for.  The mark is little endian, and so are the "UTF-16" and "UTF-32"
 * forms, so only the explicitly big endian ones reverse. */
static void
put_unit(unsigned char *buf, size_t *n, size_t width, unsigned long cp,
    int be)
{
	size_t k;

	for (k = 0; k < width; k++) {
		unsigned long shift = be ? (width - 1 - k) * 8 : k * 8;

		buf[(*n)++] = (unsigned char)(cp >> shift);
	}
}

/* Re-encode UTF-8 into one of the wide encodings.  This is the inverse of
 * tu_decode_plain(): a character above the BMP becomes a surrogate pair in
 * UTF-16 and a single value in UTF-32, and the two "UTF-16" and "UTF-32"
 * forms are little endian behind a mark, while the byte order named forms
 * carry no mark. */
int
tu_encode_bytes(const char *utf8, size_t len, tu_encoding_t enc,
    char **out, size_t *outlen)
{
	size_t inwidth, width, mark, cap, i, n = 0;
	int be;
	unsigned char *buf;

	*out = NULL;
	if (enc == TU_ENC_UTF8) {
		buf = xmalloc(len + 1);
		memcpy(buf, utf8, len);
		*out = (char *)buf;
		*outlen = len;
		return 0;
	}

	switch (enc) {
	case TU_ENC_UTF16:
	case TU_ENC_UTF16LE:
		width = 2;
		mark = (enc == TU_ENC_UTF16) ? 2 : 0;
		break;
	case TU_ENC_UTF16BE:
		width = 2;
		mark = 0;
		break;
	case TU_ENC_UTF32:
	case TU_ENC_UTF32LE:
		width = 4;
		mark = (enc == TU_ENC_UTF32) ? 4 : 0;
		break;
	default:
		width = 4;
		mark = 0;
		break;
	}
	be = enc == TU_ENC_UTF16BE || enc == TU_ENC_UTF32BE;

	/* One output unit per input byte is not enough: a single ASCII byte
	 * becomes two units in UTF-16 and four in UTF-32.  Four is the widest
	 * ratio any of these encodings can reach, since a four byte UTF-8
	 * sequence never needs more than four bytes out. */
	cap = len * 4 + mark + 1;
	buf = xmalloc(cap);

	if (enc == TU_ENC_UTF16) {
		buf[n++] = 0xFF;
		buf[n++] = 0xFE;
	} else if (enc == TU_ENC_UTF32) {
		buf[n++] = 0xFF;
		buf[n++] = 0xFE;
		buf[n++] = 0x00;
		buf[n++] = 0x00;
	}

	for (i = 0; i < len;) {
		unsigned long cp = cp_at((const unsigned char *)utf8 + i,
		    len - i, &inwidth);

		i += inwidth;
		if (cp > 0x10FFFF)	/* not a scalar value, so not encodable */
			continue;
		if (width == 4) {
			put_unit(buf, &n, 4, cp, be);
		} else if (cp >= 0x10000) {
			unsigned long v = cp - 0x10000;

			put_unit(buf, &n, 2, 0xD800 + (v >> 10), be);
			put_unit(buf, &n, 2, 0xDC00 + (v & 0x3FF), be);
		} else {
			put_unit(buf, &n, 2, cp, be);
		}
	}

	*out = (char *)buf;
	*outlen = n;
	return 0;
}

int
tu_write_txt(const tu_doc_t *d, const char *path, tu_encoding_t enc)
{
	FILE *fp;
	char *buf = NULL;
	size_t buflen = 0;
	int rc = 0;

	if (tu_encode_bytes(d->text, d->len, enc, &buf, &buflen) != 0) {
		tu_write_failed(path);
		return -1;
	}
	fp = fopen(path, "wb");
	if (fp == NULL) {
		free(buf);
		tu_write_failed(path);
		return -1;
	}
	if (buflen != 0 && fwrite(buf, 1, buflen, fp) != buflen)
		rc = -1;
	if (fclose(fp) != 0)
		rc = -1;
	free(buf);
	if (rc != 0)
		tu_write_failed(path);
	return rc;
}

/* The number of days from 1970-01-01 to a proleptic Gregorian date, and the
 * date a number of days names.  Spelled out rather than reached for through
 * timegm() and gmtime_r(), which are not in C11 and would want feature test
 * macros this port does not otherwise need. */
static long
days_from_civil(long y, int m, int d)
{
	long era;
	unsigned yoe, doy, doe;

	y -= m <= 2;
	era = (y >= 0 ? y : y - 399) / 400;
	yoe = (unsigned)(y - era * 400);
	doy = (unsigned)((153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1);
	doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
	return era * 146097 + (long)doe - 719468;
}

static void
civil_from_days(long z, long *y, int *m, int *d)
{
	long era;
	unsigned doe, yoe, doy;
	int mp;

	z += 719468;
	era = (z >= 0 ? z : z - 146096) / 146097;
	doe = (unsigned)(z - era * 146097);
	yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
	*y = (long)yoe + era * 400;
	doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
	mp = (int)((5 * doy + 2) / 153);
	*d = (int)(doy - (153 * (unsigned)mp + 2) / 5) + 1;
	*m = mp < 10 ? mp + 3 : mp - 9;
	if (*m <= 2)
		*y += 1;
}

/* The -creationtime and -modificationtime argument.  Only the first twenty
 * characters are looked at and only their shape is checked: the six fields are
 * not examined for range, so a month of 13 or an hour of 25 is a time the
 * reference tool accepts, and anything at all may follow the closing Z.  What
 * comes back is the time put through the epoch and back out again, which is
 * where it is normalised: the six fields are then all in range and they mean
 * the same moment they would have meant had they been written in range.
 *
 *      0 1 2 3  4  5 6  7  8 9  10  11 12  13  14 15  16  17 18  19
 *      Y Y Y Y  -  M M  -  D D  T  H H  :  M M  :  S S  Z
 */
int
tu_parse_timestamp(const char *s, struct tu_time *t)
{
	static const char literal[5] = { '-', '-', 'T', ':', ':' };
	static const int at[5] = { 4, 7, 10, 13, 16 };
	static const int start[6] = { 0, 5, 8, 11, 14, 17 };
	static const int width[6] = { 4, 2, 2, 2, 2, 2 };
	long v[6], days, rest;

	if (strlen(s) < 20 || s[19] != 'Z')
		return 0;
	for (int i = 0; i < 5; i++)
		if (s[at[i]] != literal[i])
			return 0;
	for (int i = 0; i < 19; i++) {
		int fixed = 0;

		for (int k = 0; k < 5; k++)
			if (at[k] == i)
				fixed = 1;
		if (!fixed && (s[i] < '0' || s[i] > '9'))
			return 0;
	}
	for (int f = 0; f < 6; f++) {
		v[f] = 0;
		for (int k = 0; k < width[f]; k++)
			v[f] = v[f] * 10 + (s[start[f] + k] - '0');
	}
	t->unix = days_from_civil(v[0], (int)v[1], (int)v[2]) * 86400L +
	    v[3] * 3600L + v[4] * 60L + v[5];
	days = t->unix / 86400L;
	rest = t->unix % 86400L;
	/* Division rounds towards zero, and a time before the epoch has a
	 * negative remainder that has to be put back into the day. */
	if (rest < 0) {
		rest += 86400L;
		days -= 1;
	}
	civil_from_days(days, &t->year, &t->month, &t->day);
	t->hour = (int)(rest / 3600);
	t->minute = (int)(rest / 60) % 60;
	t->second = (int)(rest % 60);
	return 1;
}
