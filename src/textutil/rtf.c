/*
 * textutil - RTF output.
 *
 * The reference tool wraps plain text in a fixed Cocoa RTF envelope: a
 * Helvetica-Light 12pt run, a white colour table, and default half-inch tab
 * stops.  The envelope is constant for this port, so it lives in one string
 * and only the run text is escaped.
 *
 * The escaping is what needs care:
 *
 *   \  {  }   backslash-escaped
 *   tab      written as a raw tab byte
 *   LF       a backslash followed by a raw LF, which is how RTF spells a
 *   CR       paragraph mark; CR is normalised to the same thing
 *   U+0000   up through U+00FF become \'hh, the cp1252 byte, since the
 *   -U+00FF  envelope declares \ansicpg1252
 *   above    \uNNNN, one per UTF-16 code unit, so a supplementary character
 *   U+00FF   is emitted as its surrogate pair; each escape is followed by a
 *            single space, and \uc0 is written once, ahead of the first \u
 *
 * Copyright (C) 2026, LibreDarwin
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "textutil.h"

static const char envelope_pre[] =
    "{\\rtf1\\ansi\\ansicpg1252\\cocoartf2870\n"
    "\\cocoatextscaling0\\cocoaplatform0{\\fonttbl";

static const char envelope_post[] =
    "\n"
    "{\\colortbl;\\red255\\green255\\blue255;}\n"
    "{\\*\\expandedcolortbl;;}\n";

static const char tx_stops[] =
    "\\tx560\\tx1120\\tx1680\\tx2240\\tx2800\\tx3360\\tx3920\\tx4480"
    "\\tx5040\\tx5600\\tx6160\\tx6720";

/* What goes between the tab stops and \partightenfactor0, by the mark the
 * paragraph opened with.  A paragraph that opened with a right-to-left mark
 * reads right to left and says so; one that opened with a left-to-right mark
 * said which way to read too, and the reference tool records having been told
 * by writing no direction word at all rather than the one for left to right.
 * Everything else is left to right by default, which is spelled out. */
static const char dir_natural[] = "\\pardirnatural";
static const char dir_right[] = "\\rtlpar\\qr";
static const char dir_named[] = "";

struct sink {
	char *buf;
	size_t len, cap;
	int failed;
};

static void
sink_put(struct sink *s, const char *p, size_t n)
{
	if (s->failed)
		return;
	if (s->len + n + 1 > s->cap) {
		size_t cap = s->cap ? s->cap : 512;
		char *b;

		while (cap < s->len + n + 1)
			cap *= 2;
		b = realloc(s->buf, cap);
		if (b == NULL) {
			s->failed = 1;
			return;
		}
		s->buf = b;
		s->cap = cap;
	}
	memcpy(s->buf + s->len, p, n);
	s->len += n;
	/* The room for this was asked for above, and a reader that walks the
	 * buffer looking for the end of it has to find one. */
	s->buf[s->len] = '\0';
}

static void
sink_str(struct sink *s, const char *p)
{
	sink_put(s, p, strlen(p));
}

/* Append a decimal code point, the way RTF wants it: always signed-looking in
 * documentation but in practice the plain positive value. */
static void
sink_uescape(struct sink *s, unsigned long u)
{
	char tmp[24];

	snprintf(tmp, sizeof(tmp), "\\u%lu ", u);
	sink_str(s, tmp);
}

/* The embedding levels the text being written is under, and the ones the reader
 * has been told are open.  The two come apart across a page break, which closes
 * the reader's copy without the text leaving the levels, so both are kept, and
 * the reader's copy is kept whole rather than as a depth: a level that is
 * opened and closed with no text between leaves the reader where it was, so the
 * text either side of it is written under one level rather than two. */
struct rtf_embed {
	unsigned long *stack;		/* the levels the text is under */
	size_t dep;			/* how deep stack is */
	unsigned long *open;		/* the levels the reader has been told are open */
	size_t odep;			/* how deep open is */
	int uc0;			/* whether a text escape has said \uc0 */
};

/* An embedding level, as RTF writes one.  Unlike a text escape this always
 * carries its own \uc0, so that a \u escape cannot be left to run long by
 * whatever \ucN a text escape before it put in force, and it hands that count
 * back afterwards for the next text escape to state for itself. */
static void
embed_control(struct sink *s, struct rtf_embed *e, unsigned long cp)
{
	sink_str(s, "\\uc0");
	sink_uescape(s, cp);
	e->uc0 = 0;
}

/* Bring the levels the reader has been told are open into line with the levels
 * the text is under.  The levels the two agree on are left alone, so a control
 * that opens and closes a level with no text between leaves the text either
 * side of it under one level and writes nothing at all; the levels that differ
 * are closed from the top down and opened from the bottom up. */
static void
embed_reconcile(struct sink *s, struct rtf_embed *e)
{
	size_t k = 0;

	while (k < e->dep && k < e->odep && e->stack[k] == e->open[k])
		k++;
	while (e->odep > k) {
		embed_control(s, e, 0x202C);
		e->odep--;
	}
	while (e->odep < e->dep) {
		embed_control(s, e, e->stack[e->odep]);
		e->open[e->odep] = e->stack[e->odep];
		e->odep++;
	}
}

/* A control that ends a level takes the level off the text, if there is one
 * open.  Nothing is written for it here: whether the reader is told to close a
 * level is settled when the next text is written, so that a level opened and
 * closed with no text between is never told at all. */
static void
embed_unwind(struct rtf_embed *e)
{
	if (e->dep > 0)
		e->dep--;
}

/* Close every level the reader has been told of.  A paragraph mark and the end
 * of the document close the levels and leave nothing open behind them. */
static void
embed_close(struct sink *s, struct rtf_embed *e)
{
	while (e->odep > 0) {
		embed_control(s, e, 0x202C);
		e->odep--;
	}
}

/* A page break writes the levels the reader has been told of out and closes
 * them, since RTF takes the reader's embedding state with it.  The levels
 * themselves are kept, though, and the text after the break is written as if
 * they were still open: they are what a later control closes, and what the
 * paragraph closes at its end. */
static void
embed_page(struct sink *s, struct rtf_embed *e)
{
	size_t i;

	embed_reconcile(s, e);
	for (i = e->odep; i > 0; i--)
		embed_control(s, e, 0x202C);
	sink_str(s, "\\page ");
}

/* A paragraph's properties are written out when they are first needed and
 * again whenever they change, so a run of paragraphs that agree writes the
 * block once.  The first one is followed by a blank line, because it opens the
 * document's text; a later one is not, because it is written inside text that
 * has already begun. */
static void
put_pard(struct sink *s, const char *dir, int first)
{
	sink_str(s, "\\pard");
	sink_str(s, tx_stops);
	sink_str(s, dir);
	sink_str(s, "\\partightenfactor0\n");
	if (first)
		sink_put(s, "\n", 1);
}

/* The run's own properties, which every paragraph shares and so are written
 * once, into the first paragraph, rather than repeated per paragraph. */
static void
put_run_props(struct sink *s, const tu_font_t *face, int points)
{
	char tmp[24];

	/* A bold or italic face sets a run attribute, and where both are set the
	 * oblique one comes first. */
	if (face->italic)
		sink_str(s, "\\f0\\i");
	else
		sink_str(s, "\\f0");
	if (face->bold)
		sink_str(s, "\\b");
	/* RTF sizes are in half points. */
	snprintf(tmp, sizeof(tmp), "\\fs%d", points * 2);
	sink_str(s, tmp);
}

/* The metadata fields, in the order the reference tool writes them.  The order
 * is fixed and is not the order they were given on the command line, and the
 * control words are not the option names: -comment writes \doccomm, -editor
 * writes \operator, and -company is a destination rather than a source.
 * -keywords comes first here, and the two times last, which is where the
 * reference tool puts them as well. */
static const struct {
	size_t offset;
	const char *field;
} info_fields[] = {
	{ offsetof(tu_meta_t, title),		"\\title" },
	{ offsetof(tu_meta_t, subject),	"\\subject" },
	{ offsetof(tu_meta_t, comment),	"\\doccomm" },
	{ offsetof(tu_meta_t, author),		"\\author" },
	{ offsetof(tu_meta_t, editor),		"\\operator" },
	{ offsetof(tu_meta_t, company),	"\\*\\company" }
};

/* The code page 1252 characters above U+00FF, in the bytes they occupy.  This is
 * the one description of which characters the code page has, and it is written
 * the way the code page is indexed -- by byte -- because that is the direction
 * the mapping is consulted in reverse.  Everything the code page has in Latin 1
 * is already the same in both, so nothing below U+0100 needs a table: U+0080 to
 * U+009F are the range where the two code pages part company, and none of it is
 * in 1252, which is why those are written as \u escapes like everything else
 * outside this table. */
static const unsigned short cp1252_above[32] = {
	0x20ac, 0x0000, 0x201a, 0x0192, 0x201e, 0x2026, 0x2020, 0x2021,
	0x02c6, 0x2030, 0x0160, 0x2039, 0x0152, 0x0000, 0x017d, 0x0000,
	0x0000, 0x2018, 0x2019, 0x201c, 0x201d, 0x2022, 0x2013, 0x2014,
	0x02dc, 0x2122, 0x0161, 0x203a, 0x0153, 0x0000, 0x017e, 0x0178
};

/* The code page byte for a character, or 0 when it has none. */
static unsigned char to_cp1252(unsigned long cp);

/* The length in bytes of the UTF-8 character that starts with c, which is
 * what the loops below advance by.  A byte that cannot start one counts as
 * one, since it is written as U+FFFD and takes up no room. */
static size_t
utf8_width(unsigned char c)
{
	if ((c & 0xE0) == 0xC0)
		return 2;
	if ((c & 0xF0) == 0xE0)
		return 3;
	if ((c & 0xF8) == 0xF0)
		return 4;
	return 1;
}

/* Whether the character at p has to be written as a \u escape, which is what
 * happens when the code page the header names has no byte for it.  RTF is
 * 7-bit, so a character that does have one is written as \'hh and needs no
 * escape, and the reference tool lets those run without saying \uc0 again. */
static int
needs_uc(const unsigned char *p, size_t n)
{
	unsigned long cp;
	size_t seq;

	if (n == 0)
		return 0;
	seq = utf8_width(p[0]);
	if (seq == 1)
		return p[0] >= 0x80 && to_cp1252(p[0]) == 0;
	if (p[0] < 0x80)
		return 0;
	if (seq == 2)
		cp = (unsigned char)p[0] & 0x1Fu;
	else if (seq == 3)
		cp = (unsigned char)p[0] & 0x0Fu;
	else
		cp = (unsigned char)p[0] & 0x07u;
	for (size_t k = 1; k < seq && k < n; k++)
		cp = (cp << 6) | ((unsigned char)p[k] & 0x3Fu);
	return to_cp1252(cp) == 0;
}

/* Whether a run of text that a NUL cut short begins again at this character.
 * A character with no code page byte needs a \u escape, and that is how the run
 * resumes.  A control that ends an embedding level is never written as a \u
 * escape of its own, only as the closing of levels already open, so it is not
 * one of those. */
static int
resumes(const unsigned char *p, size_t n)
{
	return needs_uc(p, n) && tu_bidi_embed(p, n) != 0x202C;
}

/* The code page byte for a character, or 0 when it has none. */
static unsigned char
to_cp1252(unsigned long cp)
{
	if (cp < 0x80 || (cp >= 0xa0 && cp < 0x100))
		return (unsigned char)cp;
	for (unsigned i = 0; i < sizeof(cp1252_above) / sizeof(cp1252_above[0]);
	    i++)
		if (cp1252_above[i] != 0 && cp == cp1252_above[i])
			return (unsigned char)(0x80 + i);
	return 0;
}

/* Write one metadata value.  RTF is 7-bit, so a character that fits in the code
 * page the header names is written as \'hh, and one that does not is written as
 * its UTF-16 code units.  \uc0 says how many bytes of the text following an
 * escape the reader is to skip, and it belongs in front of the first \u of a
 * value and nowhere else: the reference tool writes it once and then lets the
 * escapes run, and does not write it a second time after a \'hh or after a
 * character that needed no escape of its own.  A paragraph mark does end that,
 * though, so the first \u after one carries \uc0 again; the body text below
 * keeps the same rule.  A character above the basic plane is therefore a pair
 * of escapes rather than one.  Everything below 0x80 is passed through
 * untouched, control characters included.
 *
 * A byte that is not part of a well formed UTF-8 character is U+FFFD, one for
 * each ill-formed piece of input, which is how a command line argument that
 * was not text reaches the file. */
static void
info_value(struct sink *s, const char *v)
{
	const unsigned char *p = (const unsigned char *)v;
	size_t left = strlen(v);
	int uc0 = 0;
	char tmp[16];

	while (left > 0) {
		unsigned long cp;
		size_t seq;
		unsigned char c = *p;

		if (c < 0x80) {
			if (c == '{' || c == '}' || c == '\\') {
				char esc[2] = { '\\', (char)c };

				sink_put(s, esc, 2);
			} else
				sink_put(s, (const char *)p, 1);
			p++;
			left--;
			continue;
		}
		cp = tu_utf8_strict(p, left, &seq);
		p += seq;
		left -= seq;
		if (cp <= 0xffff) {
			unsigned char b = to_cp1252(cp);

			if (b != 0) {
				snprintf(tmp, sizeof(tmp), "\\'%02x", b);
				sink_str(s, tmp);
				continue;
			}
			snprintf(tmp, sizeof(tmp), "%s\\u%lu ",
			    uc0 ? "" : "\\uc0", cp);
			uc0 = 1;
			sink_str(s, tmp);
			continue;
		}
		/* Above the basic plane, each half of the pair is written as its
		 * own escape. */
		cp -= 0x10000;
		snprintf(tmp, sizeof(tmp), "%s\\u%lu ", uc0 ? "" : "\\uc0",
		    0xd800 + (cp >> 10));
		uc0 = 1;
		sink_str(s, tmp);
		snprintf(tmp, sizeof(tmp), "\\u%lu ", 0xdc00 + (cp & 0x3ff));
		sink_str(s, tmp);
	}
}

/* One metadata group, opening the info group if nothing has opened it yet. */
static void
info_group(struct sink *s, const char *field, const char *v, int *wrote)
{
	if (!*wrote) {
		sink_str(s, "{\\info\n");
		*wrote = 1;
	} else
		sink_put(s, "\n", 1);
	sink_str(s, "{");
	sink_str(s, field);
	sink_put(s, " ", 1);
	info_value(s, v);
	sink_put(s, "}", 1);
}

/* Write the info group, if anything is to go in it.  It is written for an empty
 * document too, and with no metadata at all it is not written: there is no such
 * thing as an empty group here. */
static void
write_info(struct sink *s, const tu_meta_t *meta)
{
	int wrote = 0;

	if (meta == NULL)
		return;
	for (size_t i = 0; i < sizeof(info_fields) / sizeof(info_fields[0]); i++) {
		const char *v = *(const char *const *)((const char *)meta +
		    info_fields[i].offset);

		if (v != NULL)
			info_group(s, info_fields[i].field, v, &wrote);
	}
	/* The two times are written as a broken-down date rather than as a value,
	 * and the date is not the one that was asked for: it is the epoch
	 * seconds put back through the same conversion, so an out of range
	 * field carries into its neighbours and a day of 45 is written as the
	 * 15th of the next month.  \timesinceref is then seconds since 1935 --
	 * the Unix time less 978307200, which is the 11323 days from 1904 to
	 * 1935 -- and is left out from 2147483647 upwards, which is where the
	 * 32-bit RTF field has run out and a date past 2069 loses it.  Values
	 * far below that are not left out: the field is written as a signed
	 * quantity and only its upper end is tested. */
	for (int t = 0; t < 2; t++) {
		const char *ts = t == 0 ? meta->creationtime :
		    meta->modificationtime;
		struct tu_time tm;
		long ref;
		char tmp[160];

		if (ts == NULL || !tu_parse_timestamp(ts, &tm))
			continue;
		ref = tm.unix - 978307200L;
		if (!wrote) {
			sink_str(s, "{\\info\n");
			wrote = 1;
		} else
			sink_put(s, "\n", 1);
		snprintf(tmp, sizeof(tmp),
		    "{%s\\yr%ld\\mo%d\\dy%d\\hr%d\\min%d\\sec%d",
		    t == 0 ? "\\creatim" : "\\revtim", tm.year, tm.month,
		    tm.day, tm.hour, tm.minute, tm.second);
		sink_str(s, tmp);
		if (ref < 2147483647L) {
			snprintf(tmp, sizeof(tmp), "\\timesinceref%ld}", ref);
			sink_str(s, tmp);
		} else
			sink_put(s, "}", 1);
	}
	if (meta->keywords != NULL)
		info_group(s, "\\keywords", meta->keywords, &wrote);
	if (wrote)
		sink_put(s, "}", 1);
}

int
tu_write_rtf(const tu_doc_t *d, const char *path, const tu_style_t *st,
    const tu_meta_t *meta)
{
	struct sink s = { NULL, 0, 0, 0 };
	FILE *fp;
	int empty = !tu_bidi_para((const unsigned char *)d->text, d->len);
	const unsigned char *p = (const unsigned char *)d->text;
	size_t i = 0;
	/* The reference tool writes the text as a C string, so a NUL ends it and
	 * nothing after the NUL is written at all -- not even the high bytes that
	 * would have followed it.  Every other control character is passed
	 * through as itself. */
	size_t len = 0;
	tu_font_t face;
	int points = 12;
	/* A mark that opens a paragraph says which way that paragraph reads, and
	 * is not itself text, so it is dropped from the body.  The direction it
	 * names is written into the paragraph's properties instead. */
	int at_para = 1;
	int have_pard = 0;
	const char *last_dir = NULL;
	/* The embedding controls under the text being written, and the ones the
	 * reader has been told are open, which come apart across a page break. */
	struct rtf_embed e;
	int failed;

	tu_font_face(st != NULL ? st->font : NULL, &face);
	if (st != NULL && st->fontsize > 0)
		points = st->fontsize;

	memset(&e, 0, sizeof(e));

	sink_str(&s, envelope_pre);
	/* An empty document names no font and opens no paragraph: it is just the
	 * envelope.  Either way the font table is closed here. */
	if (empty) {
		sink_str(&s, "}");
	} else {
		sink_put(&s, "\\f0", 3);
		sink_str(&s, face.family);
		sink_str(&s, "\\fcharset0 ");
		sink_str(&s, face.postscript);
		sink_str(&s, ";}");
	}
	sink_str(&s, envelope_post);
	/* The info group goes between the envelope and the paragraph, and is
	 * written whenever any metadata was given at all, an empty value
	 * included.  With none there is no group, not an empty one. */
	write_info(&s, meta);

	len = d->len;

	/* One level per control, and there cannot be more controls than a third of
	 * the text, so this covers any depth the reference tool nests to. */
	failed = len / 3 + 2 > SIZE_MAX / sizeof(*e.stack);
	if (!failed) {
		e.stack = malloc((len / 3 + 2) * sizeof(*e.stack));
		e.open = malloc((len / 3 + 2) * sizeof(*e.open));
		failed = e.stack == NULL || e.open == NULL;
	}
	if (failed) {
		free(e.stack);
		free(e.open);
		free(s.buf);
		tu_write_failed(path);
		return -1;
	}

	/* An empty document writes no text at all, and so opens no paragraph
	 * either, even when it has characters in it that are only directions. */
	while (i < len && !empty) {
		unsigned char c;
		unsigned long cp, ec;
		size_t seq = 1;

		/* The head of a paragraph is where a mark naming its direction
		 * would sit, and where its properties are written.  The properties
		 * are only written when they differ from the paragraph before, which
		 * is what lets a document of many paragraphs that agree carry one
		 * block rather than one per paragraph. */
		if (at_para) {
			unsigned long m = tu_bidi_open(p + i, len - i);
			const char *dir;

			at_para = 0;
			dir = m == 0x200F ? dir_right
			    : m == 0x200E ? dir_named : dir_natural;
			/* A paragraph with nothing in it that is written is not
			 * written, so it has no properties to state and leaves the
			 * ones before it standing. */
			if (tu_bidi_para(p + i, len - i) &&
			    (!have_pard || strcmp(dir, last_dir) != 0)) {
				put_pard(&s, dir, !have_pard);
				if (have_pard) {
					/* The run's properties were written into
					 * the first paragraph; a later one restates
					 * only the colour, which the new \pard has
					 * put back to its default. */
					sink_str(&s, "\\cf0 ");
				} else {
					put_run_props(&s, &face, points);
					sink_str(&s, " \\cf0 ");
				}
				have_pard = 1;
				last_dir = dir;
			}
			/* The mark itself is not shown to the reader. */
			if (m != 0)
				i += 3;
		}

		if (i >= len)
			break;

		/* An embedding or override control is a direction given to the text
		 * that follows rather than a character the reader is shown, so it is
		 * taken out of the text and kept as a level the run under it has.  One
		 * that opens a level says nothing on its own, since a level the text
		 * under it never uses is never written.  One that ends a level with
		 * none open does nothing; otherwise it closes the level there and
		 * then, rather than waiting for the next character to do it. */
		if ((ec = tu_bidi_embed(p + i, len - i)) != 0) {
			if (ec == 0x202C)
				embed_unwind(&e);
			else
				e.stack[e.dep++] = ec;
			i += 3;
			continue;
		}

		c = p[i];

		if (c < 0x80) {
			cp = c;
		} else if ((c & 0xE0) == 0xC0) {
			seq = 2;
			cp = c & 0x1Fu;
		} else if ((c & 0xF0) == 0xE0) {
			seq = 3;
			cp = c & 0x0Fu;
		} else if ((c & 0xF8) == 0xF0) {
			seq = 4;
			cp = c & 0x07u;
		} else {
			/* Not a lead byte; pass it through as cp1252. */
			cp = c;
			seq = 1;
		}
		for (size_t k = 1; k < seq && i + k < len; k++)
			cp = (cp << 6) | (p[i + k] & 0x3Fu);
		i += seq;

		/* A paragraph mark ends the paragraph without any text of its own
		 * being written, so the levels under the text are closed and none
		 * is opened for it.  A CR that begins the paragraph is swallowed
		 * with the text that came before it, and a CR on its own is a
		 * paragraph mark, which is the LF case.  A CR that pairs with the
		 * LF after it is that same mark written as a pair, and the levels
		 * are closed before its CR rather than between the two halves, so
		 * that no text is written under them.
		 *
		 * U+2029 is a paragraph mark too, and not a \u escape: every
		 * shape it was compared in against a plain LF, with the levels
		 * and the \uc0 that a text escape would leave behind included,
		 * gave the same bytes either way.  So it is the mark in every
		 * respect, and is written as one. */
		if (cp == '\n' || cp == '\r' || cp == 0x2029) {
			int pair = 0;

			/* The controls between the two halves of the pair are not
			 * written at all, since the mark closes the levels and leaves
			 * no text under them for them to be recorded on. */
			if (cp == '\r') {
				size_t j = i;

				while (tu_bidi_embed(p + j, len - j) != 0)
					j += 3;
				if (j < len && p[j] == '\n') {
					pair = 1;
					i = j + 1;
				}
			}
			e.uc0 = 0;
			at_para = 1;
			embed_close(&s, &e);
			e.dep = 0;
			if (pair)
				sink_put(&s, "\r", 1);
			sink_put(&s, "\\\n", 2);
			continue;
		}

		/* Whatever this character turns out to be, it is written under the
		 * levels that are open, so they are stated before it is looked at. */
		embed_reconcile(&s, &e);

		switch (cp) {
		case '\\':
			sink_str(&s, "\\\\");
			continue;
		case '{':
			sink_str(&s, "\\{");
			continue;
		case '}':
			sink_str(&s, "\\}");
			continue;
		case '\t':
			sink_put(&s, "\t", 1);
			continue;
		case '\f':
			/* A form feed is a page break, which is the one control
			 * RTF spells out rather than writing as itself.  It also
			 * ends what the text before it set up, the way a paragraph
			 * mark does, so the next \u escape says \uc0
			 * again, and the levels the text was under are written
			 * out and closed around it. */
			e.uc0 = 0;
			embed_page(&s, &e);
			continue;

		case 0:
			/* A NUL ends the text of the run it falls in.  What
			 * follows is dropped up to the next page break,
			 * paragraph mark, or character that has no code page
			 * byte and so needs a \u escape, and any of those
			 * begins a run that the NUL does not reach into.  A CR
			 * that begins the paragraph is swallowed with the text
			 * that came before it, and so is a CR that pairs with
			 * the LF after it; a CR on its own is a paragraph mark
			 * and ends the paragraph, so the one that is left over
			 * is written as a mark and the text after it survives.
			 *
			 * The scan decodes as it goes rather than testing byte
			 * by byte, because whether a run resumes depends on the
			 * character and not on its first byte. */
			while (i < len && p[i] != '\f' && p[i] != '\r' &&
			    p[i] != '\n') {
				unsigned long ec = tu_bidi_embed(p + i, len - i);

				/* A control that ends a level is not a place the
				 * run begins again, but it still says something
				 * about the text that follows, so it is acted on
				 * and the drop goes on past it. */
				if (ec == 0x202C) {
					embed_unwind(&e);
					i += 3;
					continue;
				}
				if (ec != 0 || resumes(p + i, len - i))
					break;
				i += utf8_width((unsigned char)p[i]);
			}
			if (i < len && p[i] == '\r' && i + 1 < len &&
			    p[i + 1] == '\n')
				i++;
			continue;
		case '\r':
			/* A CR that pairs with the LF after it is text, and was
			 * passed through untouched above. */
			sink_put(&s, "\r", 1);
			continue;
		}
		if (cp < 0x80) {
			sink_put(&s, (const char *)&c, 1);
			continue;
		}
		{
			unsigned char b = to_cp1252(cp);

			if (b != 0) {
				char esc[8];

				snprintf(esc, sizeof(esc), "\\'%02x", b);
				sink_str(&s, esc);
				continue;
			}
		}
		if (!e.uc0) {
			sink_str(&s, "\\uc0");
			e.uc0 = 1;
		}
		if (cp >= 0x10000) {
			unsigned long v = cp - 0x10000;

			sink_uescape(&s, 0xD800 + (v >> 10));
			sink_uescape(&s, 0xDC00 + (v & 0x3FF));
		} else {
			sink_uescape(&s, cp);
		}
	}

	/* The document's last paragraph ends like any other, so the levels still
	 * open are closed before the RTF group is. */
	embed_close(&s, &e);
	free(e.stack);
	free(e.open);

	sink_put(&s, "}", 1);

	if (s.failed) {
		free(s.buf);
		tu_write_failed(path);
		return -1;
	}
	if ((fp = fopen(path, "wb")) == NULL) {
		free(s.buf);
		tu_write_failed(path);
		return -1;
	}
	if (fwrite(s.buf, 1, s.len, fp) != s.len) {
		fclose(fp);
		free(s.buf);
		tu_write_failed(path);
		return -1;
	}
	if (fclose(fp) != 0) {
		free(s.buf);
		tu_write_failed(path);
		return -1;
	}
	free(s.buf);
	return 0;
}

/* ------------------------------------------------------------------------
 * The reader.
 *
 * -info is the one command that has to read a rich file back, and all it wants
 * from one is the text and the metadata, so that is what this produces: the
 * run attributes are dropped, which costs nothing because -info does not
 * report them and the conversions this port can do do not need them.
 *
 * The text of a group is part of the document unless the group is a header
 * table or carries \*, in which case the whole group is passed over.  Inside
 * \info the text is metadata rather than document text, and which field it
 * belongs to is decided by the control word that opened the group.  \info
 * itself is the one destination that is read rather than skipped, and
 * \*\company inside it is read for the same reason: it is the only company
 * there is.
 *
 * Copyright (C) 2026, LibreDarwin
 * SPDX-License-Identifier: BSD-3-Clause
 */

/* The character a code page 1252 byte stands for, which is the direction the
 * table above is not written in.  A byte the code page has no character for is
 * that byte's own value, so the Latin 1 range needs no table. */
static unsigned long
from_cp1252(unsigned char b)
{
	if (b < 0xa0)
		return b;
	return cp1252_above[b - 0x80];
}

/* Which metadata field an \info control word names.  -1 is none. */
enum {
	TU_MF_NONE = -1,
	TU_MF_TITLE, TU_MF_AUTHOR, TU_MF_EDITOR, TU_MF_COMPANY,
	TU_MF_SUBJECT, TU_MF_KEYWORDS, TU_MF_COMMENT,
	TU_MF_CREATIM, TU_MF_REVTIM
};

/* A group nested deeper than this is treated as document text, which is what
 * the reference tool does with a document nested far deeper than any writer
 * makes. */
#define RTF_MAXDEPTH 128

struct rtf {
	const unsigned char *p, *end;
	struct sink text;		/* the document text */
	size_t nchars;			/* characters in it, as -info counts */
	struct sink val;		/* the metadata value being collected */
	tu_meta_t *m;
	int uc;				/* characters a \uN is followed by */
	int in_info;			/* inside the \info group */
	int target;			/* TU_MF_*, or TU_MF_NONE */
	int datefield;			/* which \yr..\sec is being read */
	struct tu_time date;
	int ndate;			/* date fields read, 0 means none */
	int skip;			/* characters still to be passed over */
	int bad;			/* a backslash that names nothing legal */
	int depth;
	int finfo[RTF_MAXDEPTH];
	int ftarget[RTF_MAXDEPTH];
	int fskip[RTF_MAXDEPTH];
};

/* Append one character as UTF-8.  A character the reader has no room for as
 * itself, which is a lone surrogate out of a \u escape, is U+FFFD. */
static void
rtf_put_cp(struct rtf *r, unsigned long cp)
{
	char b[4];
	size_t n;

	if (cp >= 0xd800 && cp < 0xdc00)
		cp = 0xfffd;
	if (cp < 0x80) {
		b[0] = (char)cp;
		n = 1;
	} else if (cp < 0x800) {
		b[0] = (char)(0xc0 | (cp >> 6));
		b[1] = (char)(0x80 | (cp & 0x3f));
		n = 2;
	} else if (cp < 0x10000) {
		b[0] = (char)(0xe0 | (cp >> 12));
		b[1] = (char)(0x80 | ((cp >> 6) & 0x3f));
		b[2] = (char)(0x80 | (cp & 0x3f));
		n = 3;
	} else {
		b[0] = (char)(0xf0 | (cp >> 18));
		b[1] = (char)(0x80 | ((cp >> 12) & 0x3f));
		b[2] = (char)(0x80 | ((cp >> 6) & 0x3f));
		b[3] = (char)(0x80 | (cp & 0x3f));
		n = 4;
	}
	if (r->target != TU_MF_NONE)
		sink_put(&r->val, b, n);
	else {
		sink_put(&r->text, b, n);
		r->nchars++;
	}
}

/* The header tables, which are read past rather than read.  \info is not among
 * them because its contents are wanted. */
static int
ignorable_name(const char *name, size_t n)
{
	static const char *const names[] = {
		"annotation", "colortbl", "colorschememapping", "datastore",
		"fchars", "filetbl", "fonttbl", "footer", "footerf", "footerl",
		"footerr", "footnote", "generator", "header", "headerf",
		"headerl", "headerr", "latentstyles", "lchars", "listpicture",
		"listoverridetable", "listtable", "mmathPr", "nonshppict",
		"panose", "pict", "revtbl", "rsidtbl", "shppict", "stylesheet",
		"themedata", "upr", "xmlnstbl"
	};

	for (size_t i = 0; i < sizeof(names) / sizeof(names[0]); i++)
		if (strlen(names[i]) == n && memcmp(names[i], name, n) == 0)
			return 1;
	return 0;
}

/* The length of the control word at p, which is its letters. */
static size_t
ctl_len(const unsigned char *p, const unsigned char *end)
{
	size_t n = 0;

	while (p + n < end && ((p[n] >= 'a' && p[n] <= 'z') ||
	    (p[n] >= 'A' && p[n] <= 'Z')))
		n++;
	return n;
}

static int
ctl_is(const unsigned char *p, const unsigned char *end, const char *name)
{
	size_t n = strlen(name);

	return ctl_len(p, end) == n && memcmp(p, name, n) == 0;
}

/* Step over a group, braces and all, which is how a destination that is passed
 * over is stepped over. */
static void
rtf_skip_group(const unsigned char *p, const unsigned char *end,
    const unsigned char **next)
{
	int depth = 0;

	while (p < end) {
		if (*p == '{')
			depth++;
		else if (*p == '}') {
			if (--depth == 0) {
				*next = p + 1;
				return;
			}
		} else if (*p == '\\') {
			p++;
			if (p < end && *p != '\\' && *p != '{' && *p != '}' &&
			    *p != '\'' && *p != '*' && !ctl_len(p, end)) {
				/* A control symbol, which is the one backslash
				 * and one character. */
				if (p < end)
					p++;
				continue;
			}
			/* A control word or a hexadecimal escape is three or more
			 * characters, and the letters run to the end of it. */
			if (p < end && *p == '\'') {
				if (end - p >= 3)
					p += 3;
				else
					p = end;
			} else
				p += ctl_len(p, end);
		}
		p++;
	}
	*next = end;
}

/* The metadata field a control word names inside \info, or TU_MF_NONE. */
static int
info_field(const unsigned char *p, const unsigned char *end)
{
	if (ctl_is(p, end, "title"))		return TU_MF_TITLE;
	if (ctl_is(p, end, "author"))		return TU_MF_AUTHOR;
	if (ctl_is(p, end, "operator"))	return TU_MF_EDITOR;
	if (ctl_is(p, end, "company"))		return TU_MF_COMPANY;
	if (ctl_is(p, end, "subject"))		return TU_MF_SUBJECT;
	if (ctl_is(p, end, "keywords"))	return TU_MF_KEYWORDS;
	if (ctl_is(p, end, "doccomm"))		return TU_MF_COMMENT;
	if (ctl_is(p, end, "creatim"))		return TU_MF_CREATIM;
	if (ctl_is(p, end, "revtim"))		return TU_MF_REVTIM;
	return TU_MF_NONE;
}

/* Store a metadata value.  A group that appears twice keeps the last one,
 * which is what the reference tool does. */
static void
meta_put(struct rtf *r, int field, char *v)
{
	const char **slot = NULL;

	switch (field) {
	case TU_MF_TITLE:	slot = &r->m->title; break;
	case TU_MF_AUTHOR:	slot = &r->m->author; break;
	case TU_MF_EDITOR:	slot = &r->m->editor; break;
	case TU_MF_COMPANY:	slot = &r->m->company; break;
	case TU_MF_SUBJECT:	slot = &r->m->subject; break;
	case TU_MF_KEYWORDS:	slot = &r->m->keywords; break;
	case TU_MF_COMMENT:	slot = &r->m->comment; break;
	}
	if (slot == NULL || r->m == NULL) {
		free(v);
		return;
	}
	free((char *)*slot);
	*slot = v;
}

/* Normalise a collected date and hand it over as the broken-down string the
 * option parser uses, so that -info formats the one kind of time it has.  A
 * group with no usable field in it is the reference tool's own zero date, which
 * is 16 minutes and 8 seconds past midnight on the first of January of year
 * one, and is what it reports for a creatim with nothing in it. */
static char *
date_string(struct rtf *r)
{
	char buf[40];

	if (r->ndate == 0) {
		snprintf(buf, sizeof(buf), "0001-01-01T00:16:08Z");
	} else {
		char in[32];

		snprintf(in, sizeof(in), "%04ld-%02d-%02dT%02d:%02d:%02dZ",
		    r->date.year, r->date.month, r->date.day, r->date.hour,
		    r->date.minute, r->date.second);
		if (!tu_parse_timestamp(in, &r->date))
			return NULL;
		snprintf(buf, sizeof(buf), "%04ld-%02d-%02dT%02d:%02d:%02dZ",
		    r->date.year, r->date.month, r->date.day, r->date.hour,
		    r->date.minute, r->date.second);
	}
	return strdup(buf);
}

/* Put away what a metadata group collected. */
static void
rtf_close_info(struct rtf *r, int field)
{
	char *v;

	if (field == TU_MF_NONE || r->m == NULL)
		return;
	if (field == TU_MF_CREATIM || field == TU_MF_REVTIM) {
		const char **slot = field == TU_MF_CREATIM ?
		    &r->m->creationtime : &r->m->modificationtime;

		free((char *)*slot);
		v = date_string(r);
		*slot = v;
		return;
	}
	v = r->val.buf != NULL ? r->val.buf : strdup("");
	r->val.buf = NULL;
	r->val.len = r->val.cap = 0;
	if (v != NULL)
		meta_put(r, field, v);
}

/* Read the value of a \yr, \mo, \dy, \hr, \min or \sec.  A value that is not a
 * number is left out, and a negative one is left out too, which is what makes
 * a group holding only a negative year the zero date rather than a date before
 * the calendar began. */
static void
rtf_date_field(struct rtf *r, const unsigned char *p, const unsigned char *end,
    int which)
{
	long v = 0;
	int neg = 0;
	int got = 0;

	if (p < end && (*p == '-' || *p == '+')) {
		neg = *p == '-';
		p++;
	}
	while (p < end && *p >= '0' && *p <= '9') {
		v = v * 10 + (*p - '0');
		if (v > 100000000L)
			v = 100000000L;
		got = 1;
		p++;
	}
	if (!got || neg)
		return;
	switch (which) {
	case 0: r->date.year = v; break;
	case 1: r->date.month = (int)v; break;
	case 2: r->date.day = (int)v; break;
	case 3: r->date.hour = (int)v; break;
	case 4: r->date.minute = (int)v; break;
	default: r->date.second = (int)v; break;
	}
	r->ndate++;
}

/* The character a control symbol stands for, or 0 when it stands for none. */
static unsigned long
symbol_cp(int c)
{
	switch (c) {
	case '~':	return 0x00a0;
	case '_':	return 0x2011;
	case '\\':	return '\\';
	case '{':	return '{';
	case '}':	return '}';
	case '-':	return 0;
	default:	return 0;
	}
}

/* Read the document. */
static void
rtf_run(struct rtf *r)
{
	while (r->p < r->end) {
		unsigned char c = *r->p;

		if (c == '\r' || c == '\n') {
			/* A line ending in the source is not text. */
			r->p++;
			continue;
		}
		if (c == '{') {
			const unsigned char *q = r->p + 1;
			int ignorable = 0, info = 0;

			while (q < r->end && (*q == ' ' || *q == '\t' ||
			    *q == '\r' || *q == '\n'))
				q++;
			if (q + 1 < r->end && q[0] == '\\' && q[1] == '*') {
				/* Marked ignorable, except inside \info where
				 * \*\company is the only company there is. */
				if (!r->in_info)
					ignorable = 1;
			} else if (q < r->end && q[0] == '\\') {
				ignorable = ignorable_name((const char *)q + 1,
				    ctl_len(q + 1, r->end));
				info = !ignorable &&
				    ctl_is(q + 1, r->end, "info");
			}
			if (ignorable) {
				rtf_skip_group(r->p, r->end, &r->p);
				continue;
			}
			if (r->depth < RTF_MAXDEPTH) {
				r->finfo[r->depth] = r->in_info;
				r->ftarget[r->depth] = r->target;
				r->fskip[r->depth] = r->skip;
				r->depth++;
			}
			if (info) {
				r->in_info = 1;
				r->target = TU_MF_NONE;
				r->ndate = 0;
			}
			r->p++;
			continue;
		}
		if (c == '}') {
			if (r->depth > 0) {
				int field = r->target;
				int was_info = r->in_info;

				r->depth--;
				r->in_info = r->finfo[r->depth];
				r->target = r->ftarget[r->depth];
				r->skip = r->fskip[r->depth];
				if (was_info)
					rtf_close_info(r, field);
			}
			/* The brace that closes the outermost group closes the
			 * document with it, so what follows is not part of it
			 * and is not read. */
			if (r->depth == 0)
				break;
			r->p++;
			continue;
		}
		if (c != '\\') {
			/* A run of plain text.  Each byte is a code page 1252
			 * character, because that is the page the header
			 * declares and the one the reference tool reads with. */
			const unsigned char *q = r->p;
			size_t run = 0;

			while (q < r->end && *q != '\\' && *q != '{' &&
			    *q != '}' && *q != '\r' && *q != '\n')
				q++;
			for (; run < (size_t)(q - r->p); run++) {
				if (r->skip > 0) {
					r->skip--;
					continue;
				}
				rtf_put_cp(r, from_cp1252(r->p[run]));
			}
			r->p = q;
			continue;
		}
		/* A backslash: a control word, a control symbol, or a
		 * hexadecimal escape. */
		r->p++;
		if (r->p >= r->end)
			break;
		/* A backslash straight in front of a line ending is a
		 * paragraph mark, and the line ending goes with it. */
		if (*r->p == '\r' || *r->p == '\n') {
			if (*r->p == '\r' && r->p + 1 < r->end &&
			    r->p[1] == '\n')
				r->p += 2;
			else
				r->p++;
			if (r->skip <= 0)
				rtf_put_cp(r, '\n');
			continue;
		}
		if (*r->p == '\'') {
			int hi = -1, lo = -1;

			if (r->end - r->p >= 3) {
				const unsigned char *h = r->p + 1;

				if (h[0] >= '0' && h[0] <= '9')
					hi = h[0] - '0';
				else if (h[0] >= 'a' && h[0] <= 'f')
					hi = h[0] - 'a' + 10;
				else if (h[0] >= 'A' && h[0] <= 'F')
					hi = h[0] - 'A' + 10;
				if (h[1] >= '0' && h[1] <= '9')
					lo = h[1] - '0';
				else if (h[1] >= 'a' && h[1] <= 'f')
					lo = h[1] - 'a' + 10;
				else if (h[1] >= 'A' && h[1] <= 'F')
					lo = h[1] - 'A' + 10;
			}
			r->p += hi >= 0 && lo >= 0 ? 3 : 1;
			if (hi >= 0 && lo >= 0) {
				if (r->skip > 0)
					r->skip--;
				else
					rtf_put_cp(r, from_cp1252((unsigned char)
					    (hi * 16 + lo)));
			}
			continue;
		}
		{
			size_t n = ctl_len(r->p, r->end);
			const unsigned char *name = r->p;
			const unsigned char *q = r->p + n;
			long num = 0;
			int got = 0;

			while (q < r->end && *q >= '0' && *q <= '9') {
				num = num * 10 + (*q - '0');
				if (num > 100000000L)
					num = 100000000L;
				got = 1;
				q++;
			}
			/* A space after a control word is its delimiter and
			 * belongs to the control word, not to the text. */
			if (q < r->end && *q == ' ')
				q++;
			r->p = q;
			if (n == 0) {
				/* A control symbol: the backslash and this one
				 * character, and nothing more.  A \* that was
				 * kept because it names a real field has to be
				 * stepped over here rather than left to be read
				 * as text. */
				unsigned long cp = symbol_cp(*name);

				r->p = name + 1;
				if (cp != 0 && r->skip <= 0)
					rtf_put_cp(r, cp);
				continue;
			}
			/* Inside \info the field control words pick where the
			 * text goes, and the date fields are read into a time
			 * rather than collected as text. */
			if (r->in_info) {
				int field = info_field(name, name + n);
				int df = -1;

				if (r->target != TU_MF_NONE) {
					if (ctl_is(name, name + n, "yr"))
						df = 0;
					else if (ctl_is(name, name + n, "mo"))
						df = 1;
					else if (ctl_is(name, name + n, "dy"))
						df = 2;
					else if (ctl_is(name, name + n, "hr"))
						df = 3;
					else if (ctl_is(name, name + n, "min"))
						df = 4;
					else if (ctl_is(name, name + n, "sec"))
						df = 5;
				}
				if (df >= 0) {
					rtf_date_field(r, name + n, q, df);
					continue;
				}
				if (field != TU_MF_NONE &&
				    field != r->target) {
					/* A field that follows another one
					 * inside the same group closes the one
					 * before it. */
					rtf_close_info(r, r->target);
					r->target = field;
					r->ndate = 0;
					r->val.len = 0;
					if (r->val.buf != NULL)
						r->val.buf[0] = '\0';
					continue;
				}
			}
			if (ctl_is(name, name + n, "uc")) {
				r->uc = got ? (int)num : 1;
				continue;
			}
			if (ctl_is(name, name + n, "u")) {
				long u = got ? num : 0;
				const unsigned char *d = name + n;

				if (*name == 'u' && n == 1) {
					while (d < q && *d == '-')
						d++;
					if (d > name + n)
						u = -u;
				}
				if (r->skip > 0) {
					r->skip--;
					continue;
				}
				rtf_put_cp(r, (unsigned long)(u < 0 ?
				    u + 0x10000 : u));
				r->skip = r->uc;
				continue;
			}
			if (r->in_info)
				continue;
			if (ctl_is(name, name + n, "par")) {
				if (r->skip <= 0)
					rtf_put_cp(r, '\n');
			} else if (ctl_is(name, name + n, "line")) {
				if (r->skip <= 0)
					rtf_put_cp(r, 0x2028);
			} else if (ctl_is(name, name + n, "tab")) {
				if (r->skip <= 0)
					rtf_put_cp(r, '\t');
			} else if (ctl_is(name, name + n, "emdash")) {
				if (r->skip <= 0)
					rtf_put_cp(r, 0x2014);
			} else if (ctl_is(name, name + n, "endash")) {
				if (r->skip <= 0)
					rtf_put_cp(r, 0x2013);
			} else if (ctl_is(name, name + n, "lquote")) {
				if (r->skip <= 0)
					rtf_put_cp(r, 0x2018);
			} else if (ctl_is(name, name + n, "rquote")) {
				if (r->skip <= 0)
					rtf_put_cp(r, 0x2019);
			} else if (ctl_is(name, name + n, "ldblquote")) {
				if (r->skip <= 0)
					rtf_put_cp(r, 0x201c);
			} else if (ctl_is(name, name + n, "rdblquote")) {
				if (r->skip <= 0)
					rtf_put_cp(r, 0x201d);
			} else if (ctl_is(name, name + n, "bullet")) {
				if (r->skip <= 0)
					rtf_put_cp(r, 0x2022);
			} else if (ctl_is(name, name + n, "enspace")) {
				if (r->skip <= 0)
					rtf_put_cp(r, 0x2002);
			} else if (ctl_is(name, name + n, "emspace")) {
				if (r->skip <= 0)
					rtf_put_cp(r, 0x2003);
			} else if (ctl_is(name, name + n, "qmspace")) {
				if (r->skip <= 0)
					rtf_put_cp(r, 0x2005);
			}
		}
	}
}

void
tu_meta_free(tu_meta_t *m)
{
	/* Each member is named rather than walked over, so that a member which
	 * is not a string cannot be mistaken for one. */
#define FREE_MF(f) do { free((char *)(m)->f); (m)->f = NULL; } while (0)
	FREE_MF(keywords);
	FREE_MF(title);
	FREE_MF(author);
	FREE_MF(subject);
	FREE_MF(comment);
	FREE_MF(editor);
	FREE_MF(company);
	FREE_MF(creationtime);
	FREE_MF(modificationtime);
#undef FREE_MF
}

/* Read an RTF file into its text and its metadata.  Both may be left alone: a
 * NULL out is simply not filled in.  Returns 0 on success. */
int
tu_read_rtf(const void *buf, size_t len, tu_doc_t *out, tu_meta_t *meta)
{
	struct rtf r;
	const unsigned char *start = buf;

	/* A newline or a carriage return in front of the mark is skipped, as
	 * many as there are.  A space or a tab in front of it is not, and is
	 * this reader not opening the file at all.  That is the difference
	 * between a file the reference tool reads and one it refuses. */
	while (len > 0 && (*start == '\n' || *start == '\r')) {
		start++;
		len--;
	}

	/* RTF starts with that, and a file that does not is not a text file
	 * this reader can open at all.  A file that does start with it and
	 * then falls apart is the other kind of failure. */
	if (len < 5 || memcmp(start, "{\\rtf", 5) != 0)
		return TU_READ_UNOPENABLE;
	memset(&r, 0, sizeof(r));
	r.p = start;
	r.end = start + len;
	r.m = meta;
	r.uc = 1;
	r.target = TU_MF_NONE;
	r.datefield = -1;
	rtf_run(&r);
	/* A group left open is a file that falls apart.  A group closed more
	 * times than it was opened is not: the reference tool stops caring
	 * once the outermost group is done.  A read that fails leaves out
	 * empty, the way tu_meta_free leaves a meta it never filled in empty,
	 * because a caller that falls back to another reader will go on to use
	 * it. */
	if (r.bad || r.depth != 0) {
		free(r.text.buf);
		free(r.val.buf);
		if (out != NULL) {
			out->text = NULL;
			out->len = 0;
			out->nchars = 0;
		}
		return TU_READ_WRONGFMT;
	}
	if (r.text.buf == NULL)
		sink_put(&r.text, "", 0);
	if (r.text.failed) {
		free(r.text.buf);
		free(r.val.buf);
		if (out != NULL) {
			out->text = NULL;
			out->len = 0;
			out->nchars = 0;
		}
		return TU_READ_UNOPENABLE;
	}
	if (out != NULL) {
		out->text = r.text.buf;
		out->len = r.text.len;
		out->nchars = r.nchars;
	} else
		free(r.text.buf);
	free(r.val.buf);
	return 0;
}

/* Read an RTFD bundle, which is a folder holding a TXT.rtf.  The text file is
 * the one the writer makes, and a bundle without one has no text to read. */
int
tu_read_rtfd(const char *path, tu_doc_t *out, tu_meta_t *meta)
{
	char inner[PATH_MAX];
	char *buf;
	size_t len;
	FILE *fp;
	int rc;

	if (snprintf(inner, sizeof(inner), "%s/TXT.rtf", path) >=
	    (int)sizeof(inner))
		return TU_READ_UNOPENABLE;
	if ((fp = fopen(inner, "rb")) == NULL)
		return TU_READ_UNOPENABLE;
	if (fseek(fp, 0, SEEK_END) != 0) {
		fclose(fp);
		return TU_READ_UNOPENABLE;
	}
	len = (size_t)ftell(fp);
	rewind(fp);
	if (len == 0 || (buf = malloc(len)) == NULL) {
		fclose(fp);
		return TU_READ_UNOPENABLE;
	}
	if (fread(buf, 1, len, fp) != len) {
		free(buf);
		fclose(fp);
		return TU_READ_UNOPENABLE;
	}
	fclose(fp);
	rc = tu_read_rtf(buf, len, out, meta);
	free(buf);
	return rc;
}
