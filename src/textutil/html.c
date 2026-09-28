/*
 * textutil - HTML output.
 *
 * The reference tool emits a fixed HTML 4.01 Strict envelope with an empty
 * <title> and a Cocoa HTML Writer generator tag, then one paragraph per line
 * of input.
 *
 * Paragraphs come in two flavours: a line with text on it, and a line with
 * none.  Each flavour gets a class, and the classes are numbered in the order
 * the paragraphs first introduced them, so a document of nothing but blank
 * lines calls that first class p1 rather than p2.  A line with nothing on it
 * is emitted as <br>, or as no-break spaces when it holds only blanks, and
 * its class is given a min-height so that it still occupies a line.  Only the
 * classes actually used appear in the stylesheet, which is therefore
 * assembled after the body is known.
 *
 * Within a line, leading whitespace is turned into no-break spaces inside an
 * Apple-converted-space span, a tab becomes an Apple-tab-span, and &, < and >
 * are escaped.  Everything else, including text outside ASCII, is written
 * through as UTF-8 unless -encoding asks for one of the wide encodings, in
 * which case the whole document is re-encoded and the charset attribute names
 * the encoding that was used.
 *
 * Copyright (C) 2026, LibreDarwin
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "textutil.h"

static const char head_before_style[] =
    "<!DOCTYPE html PUBLIC \"-//W3C//DTD HTML 4.01//EN\""
    " \"http://www.w3.org/TR/html4/strict.dtd\">\n"
    "<html>\n"
    "<head>\n";
/* The charset attribute names the encoding the document is written in, so it
 * follows -encoding rather than always saying utf-8. */
static const char meta_prefix[] =
    "  <meta http-equiv=\"Content-Type\" content=\"text/html; charset=";
static const char meta_suffix[] = "\">\n";
static const char meta_after_charset[] =
    "  <meta http-equiv=\"Content-Style-Type\" content=\"text/css\">\n";

/* -title fills the empty <title> the reference tool writes by default. */
static const char title_open[] = "  <title>";
static const char title_close[] = "</title>\n";
static const char title_empty[] = "  <title></title>\n";

static const char head_after_title[] =
    "  <meta name=\"Generator\" content=\"Cocoa HTML Writer\">\n"
    "  <meta name=\"CocoaVersion\" content=\"2685.6\">\n"
    "  <style type=\"text/css\">\n";

static const char head_after_style[] = "  </style>\n</head>\n<body>\n";
static const char tail[] = "</body>\n</html>\n";

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
}

static void
sink_str(struct sink *s, const char *p)
{
	sink_put(s, p, strlen(p));
}

static void
emit_escaped(struct sink *s, const char *p, size_t n)
{
	for (size_t i = 0; i < n; i++) {
		switch (p[i]) {
		case '&': sink_str(s, "&amp;"); break;
		case '<': sink_str(s, "&lt;"); break;
		case '>': sink_str(s, "&gt;"); break;
		default:  sink_put(s, &p[i], 1); break;
		}
	}
}

/* A copy of v with every ill-formed piece of UTF-8 put to U+FFFD.  A command
 * line argument is meant to be text, so the reference tool repairs it this way
 * before it reaches the document; a file read as plain text is a different
 * matter and is taken as MacRoman instead, which is why this is not also done
 * to the body. */
static char *
repaired(const char *v)
{
	size_t n = strlen(v), i = 0, w = 0;
	char *out = malloc(n * 3 + 1);

	if (out == NULL)
		return NULL;
	while (i < n) {
		unsigned long cp;
		size_t seq;

		cp = tu_utf8_strict((const unsigned char *)v + i, n - i, &seq);
		i += seq;
		if (cp < 0x80)
			out[w++] = (char)cp;
		else if (cp < 0x800) {
			out[w++] = (char)(0xc0 | (cp >> 6));
			out[w++] = (char)(0x80 | (cp & 0x3f));
		} else {
			out[w++] = (char)(0xe0 | (cp >> 12));
			out[w++] = (char)(0x80 | ((cp >> 6) & 0x3f));
			out[w++] = (char)(0x80 | (cp & 0x3f));
		}
	}
	out[w] = '\0';
	return out;
}

/* As emit_escaped(), but for the inside of an attribute, where a quote would
 * otherwise end the value. */
static void
emit_escaped_attr(struct sink *s, const char *p, size_t n)
{
	for (size_t i = 0; i < n; i++) {
		switch (p[i]) {
		case '&': sink_str(s, "&amp;"); break;
		case '<': sink_str(s, "&lt;"); break;
		case '>': sink_str(s, "&gt;"); break;
		case '"': sink_str(s, "&quot;"); break;
		default:  sink_put(s, &p[i], 1); break;
		}
	}
}

/* One metadata meta tag. */
static void
emit_meta(struct sink *s, const char *name, const char *value)
{
	char tmp[48];

	snprintf(tmp, sizeof(tmp), "  <meta name=\"%s\" content=\"", name);
	sink_str(s, tmp);
	emit_escaped_attr(s, value, strlen(value));
	sink_str(s, "\">\n");
}

/* The metadata the head carries, in the order the reference tool writes it.  The
 * names are not the option names: -editor is LastAuthor and -comment is
 * Description.  -keywords is last of the plain values, and the two times come
 * after it. */
static const struct {
	size_t offset;
	const char *name;
} head_metas[] = {
	{ offsetof(tu_meta_t, author),	"Author" },
	{ offsetof(tu_meta_t, editor),	"LastAuthor" },
	{ offsetof(tu_meta_t, company),	"Company" },
	{ offsetof(tu_meta_t, subject),	"Subject" },
	{ offsetof(tu_meta_t, comment),	"Description" },
	{ offsetof(tu_meta_t, keywords),	"Keywords" },
};

/* One run of n spaces becomes a span of no-break spaces and plain spaces.
 * The first space is always a no-break space, and of the rest every other one
 * is kept as typed, working back from the space that touches the text; so two
 * spaces give NBSP+space and three give NBSP+NBSP+space.  A run that reaches
 * the end of the line is treated as one space longer with that last space
 * dropped, which is why a line of two spaces is two no-break spaces. */
static void
emit_space_run(struct sink *s, size_t n, int at_end)
{
	size_t m = at_end ? n + 1 : n;

	sink_str(s, "<span class=\"Apple-converted-space\">");
	for (size_t i = 0; i < m; i++) {
		if (at_end && i + 1 == m)
			break;
		if (i != 0 && (i % 2) == ((m - 1) % 2))
			sink_str(s, " ");
		else
			sink_put(s, "\xc2\xa0", 2);
	}
	sink_str(s, "</span>");
}

/* One paragraph's worth of line content.  Sets *used_tab when a tab was
 * written, because that adds a rule to the stylesheet. */
static void
emit_line_body(struct sink *s, const char *p, size_t n, int *used_tab)
{
	size_t i = 0;

	/* The leading run of blanks becomes no-break spaces. */
	while (i < n && p[i] == ' ') {
		size_t j = i;

		while (j < n && p[j] == ' ')
			j++;
		emit_space_run(s, j - i, j == n);
		i = j;
	}
	for (; i < n; i++) {
		if (p[i] == '\t') {
			sink_str(s, "<span class=\"Apple-tab-span\">\t</span>");
			*used_tab = 1;
			continue;
		}
		emit_escaped(s, &p[i], 1);
	}
}

/* Where the line starting at pos ends, and how many bytes it occupies.  A
 * CR, an LF and a CRLF pair each end a line and each count once. */
static size_t
line_at(const char *text, size_t len, size_t pos, size_t *adv)
{
	size_t i = pos;

	while (i < len && text[i] != '\n' && text[i] != '\r')
		i++;
	*adv = i - pos + 1;
	if (i < len && text[i] == '\r' && i + 1 < len && text[i + 1] == '\n')
		*adv = i - pos + 2;	/* the pair is one terminator */
	return i - pos;
}

/* How many lines the text holds.  A trailing terminator closes the last line
 * rather than opening an empty one, so "a\n" is one line and "a\n\n" is
 * two: the second is empty. */
static size_t
count_lines(const char *text, size_t len)
{
	size_t n = 0, pos = 0, adv;

	if (len == 0)
		return 0;
	while (pos < len) {
		line_at(text, len, pos, &adv);
		pos += adv;
		n++;
	}
	return n;
}

int
tu_write_html(const tu_doc_t *d, const char *path, const tu_style_t *st,
    const tu_meta_t *meta, tu_encoding_t enc)
{
	(void)st;			/* the HTML writer takes no run options */
	struct sink s = { NULL, 0, 0, 0 };
	struct sink body = { NULL, 0, 0, 0 };
	const char *text = d->text;
	size_t len = d->len, pos = 0, want = count_lines(text, len);
	size_t n = 0;
	/* Class numbers are handed out in order of first use, so a document of
	 * nothing but blank lines calls the first of them p1.  Format 0 is a
	 * line with text on it, format 1 a line with none. */
	int cls[2] = { 0, 0 }, ncls = 0;
	int used_tab = 0;
	FILE *fp;
	char *outbuf = NULL;
	size_t outlen = 0;

	/* The body is built first, because the stylesheet lists the classes in
	 * the order the paragraphs introduced them, and the stylesheet has to
	 * close before <body> can open. */
	while (n < want) {
		size_t adv, linelen = line_at(text, len, pos, &adv);
		int blank = 1, fmt, c;

		for (size_t k = 0; k < linelen; k++)
			if (text[pos + k] != ' ' && text[pos + k] != '\t') {
				blank = 0;
				break;
			}
		fmt = blank ? 1 : 0;
		if (cls[fmt] == 0)
			cls[fmt] = ++ncls;
		c = cls[fmt];

		sink_str(&body, "<p class=\"p");
		sink_put(&body, (const char *)&(char){ (char)('0' + c) }, 1);
		if (linelen == 0) {
			sink_str(&body, "\"><br></p>\n");
		} else {
			sink_str(&body, "\">");
			emit_line_body(&body, text + pos, linelen, &used_tab);
			sink_str(&body, "</p>\n");
		}
		pos += adv;
		n++;
	}

	sink_str(&s, head_before_style);
	sink_str(&s, meta_prefix);
	sink_str(&s, tu_encoding_name(enc));
	sink_str(&s, meta_suffix);
	sink_str(&s, meta_after_charset);
	if (meta != NULL && meta->title != NULL) {
		char *title = repaired(meta->title);

		if (title != NULL) {
			sink_str(&s, title_open);
			emit_escaped(&s, title, strlen(title));
			sink_str(&s, title_close);
			free(title);
		} else
			sink_str(&s, title_empty);
	} else
		sink_str(&s, title_empty);
	for (size_t i = 0;
	    i < sizeof(head_metas) / sizeof(head_metas[0]); i++) {
		const char *v = *(const char *const *)((const char *)meta +
		    head_metas[i].offset);

		if (v != NULL) {
			char *fixed = repaired(v);

			if (fixed != NULL) {
				emit_meta(&s, head_metas[i].name, fixed);
				free(fixed);
			}
		}
	}
	/* A time is written in the same normalised form the RTF writer uses, so
	 * a month of 13 is the February that follows.  The tag named
	 * ModificationTime is a copy of the creation time, or of nothing at
	 * all when there was no -creationtime and so is all zeroes.  That is
	 * what the reference tool writes, and it is not what -modificationtime
	 * was given. */
	if (meta != NULL) {
		struct tu_time ct;
		char buf[32];
		const char *shown = NULL;

		if (meta->creationtime != NULL &&
		    tu_parse_timestamp(meta->creationtime, &ct)) {
			snprintf(buf, sizeof(buf), "%04ld-%02d-%02dT%02d:%02d:%02dZ",
			    ct.year, ct.month, ct.day, ct.hour, ct.minute,
			    ct.second);
			shown = buf;
		}
		if (meta->creationtime != NULL)
			emit_meta(&s, "CreationTime",
			    shown != NULL ? shown : "0000-00-00T00:00:00Z");
		if (meta->modificationtime != NULL)
			emit_meta(&s, "ModificationTime",
			    shown != NULL ? shown : "0000-00-00T00:00:00Z");
	}
	sink_str(&s, head_after_title);
	for (int c = 1; c <= ncls; c++) {
		int fmt = cls[0] == c ? 0 : 1;

		sink_str(&s, "    p.p");
		sink_put(&s, (const char *)&(char){ (char)('0' + c) }, 1);
		sink_str(&s, " {margin: 0.0px 0.0px 0.0px 0.0px;"
		    " font: 12.0px 'Helvetica Light'");
		/* A line with nothing on it still has to take up room. */
		if (fmt == 1)
			sink_str(&s, "; min-height: 14.0px");
		sink_str(&s, "}\n");
	}
	/* A tab is only preserved if the stylesheet says so. */
	if (used_tab)
		sink_str(&s, "    span.Apple-tab-span {white-space:pre}\n");
	sink_str(&s, head_after_style);
	sink_put(&s, body.buf != NULL ? body.buf : "", body.len);
	sink_str(&s, tail);
	free(body.buf);

	if (s.failed) {
		free(s.buf);
		tu_write_failed(path);
		return -1;
	}

	/* The document is held whole, so -encoding is applied to it as one
	 * piece: a wide encoding gets a mark and wide units, and the head and
	 * tail markup travels with it. */
	if (tu_encode_bytes(s.buf, s.len, enc, &outbuf, &outlen) != 0) {
		free(s.buf);
		tu_write_failed(path);
		return -1;
	}
	free(s.buf);

	if ((fp = fopen(path, "wb")) == NULL) {
		free(outbuf);
		tu_write_failed(path);
		return -1;
	}
	if (fwrite(outbuf, 1, outlen, fp) != outlen) {
		fclose(fp);
		free(outbuf);
		tu_write_failed(path);
		return -1;
	}
	if (fclose(fp) != 0) {
		free(outbuf);
		tu_write_failed(path);
		return -1;
	}
	free(outbuf);
	return 0;
}

/* ------------------------------------------------------------------------
 * The reader.
 *
 * -info is the one command that has to read HTML back, and all it wants from
 * it is the text and the metadata.  The tags that carry formatting are passed
 * over, and so are the elements whose contents are text for somebody else to
 * read, which is why a script's body and a style sheet's do not become
 * document text.
 *
 * The layout is the one the reference tool produces.  An element that starts a
 * paragraph breaks the line before it, but only when something has been
 * written since the last break, so an empty document does not open with a
 * blank line; an element that ends a paragraph always breaks.  A <br> breaks
 * at once.  A run of spaces becomes one space, dropped at the start of a line
 * and at the ends of the document.  Inside <pre> the spaces are kept as they
 * lie, and an image is not turned into an attachment.  The body bytes are code
 * page 1252, the same page RTF's hexadecimal escapes are read with, and not
 * UTF-8, so a byte that is the start of a UTF-8 character is two characters
 * here rather than one.
 *
 * A list is laid out with a tab, a mark and a tab in front of every item, and
 * a nested one with a mark of its own; what is written here is one tab, a
 * bullet and a tab, so the length of a document with a nested list in it is
 * short by that much.  See src/textutil/NOTES.md.
 *
 * Copyright (C) 2026, LibreDarwin
 * SPDX-License-Identifier: BSD-3-Clause
 */

struct html {
	const char *p, *end;
	struct sink text;
	size_t nchars;
	tu_meta_t *m;
	/* The name and the value of the meta tag being read.  They may come
	 * in either order, so both are kept until the tag ends. */
	char *mname;
	size_t mnamelen, mcap;
	char *mval;
	size_t mvallen, mvcap;
	int cname;			/* the last attribute was a name */
	int skip;			/* inside head, script or style */
	int pre;				/* inside pre or textarea */
	int space;			/* a run of spaces is being held */
	int brk;			/* a <br> has ended the line already */
	int started;			/* text written since the last break */
	int item;			/* the number the next item is */
	int items;			/* the items the list open now has */
	int listdepth;
	int ordered;			/* the list open now counts its items */
};

/* The page's own characters above 0x7f, which the body is read with. */
static unsigned long
html_cp1252(unsigned char b)
{
	static const unsigned long above[] = {
		0x20ac, 0x0081, 0x201a, 0x0192, 0x201e, 0x2026, 0x2020,
		0x2021, 0x02c6, 0x2030, 0x0160, 0x2039, 0x0152, 0x008d,
		0x017d, 0x008f, 0x0090, 0x2018, 0x2019, 0x201c, 0x201d,
		0x2022, 0x2013, 0x2014, 0x02dc, 0x2122, 0x0161, 0x203a,
		0x0153, 0x009d, 0x017e, 0x0178
	};

	if (b < 0xa0)
		return b;
	return above[b - 0xa0];
}

/* A paragraph ends with a line separator rather than a newline, which is how
 * a break inside a paragraph is written. */
#define HTML_BREAK 0x2028

/* One character of text, counted the way -info counts. */
static void
html_cp(struct html *h, unsigned long cp)
{
	char b[4];
	size_t n;

	if (cp == 0 || cp > 0x10ffff)
		return;
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
	if (cp != HTML_BREAK)
		h->brk = 0;
	sink_put(&h->text, b, n);
	/* -info counts a character outside the basic plane as two, which is
	 * how many it takes to write one in UTF-16. */
	h->nchars += cp > 0xffff ? 2 : 1;
	h->started = 1;
}

/* The end of a line.  Nothing written since the last break means there is
 * nothing to break away from. */
static void
html_break(struct html *h)
{
	if (h->started)
		html_cp(h, HTML_BREAK);
	h->space = 0;
	h->started = 0;
}

/* The end of a paragraph, which is a break whether or not there was a line to
 * break. */
static void
html_endpara(struct html *h)
{
	/* A <br> has ended the line already, and a paragraph that ends after
	 * one is the paragraph that <br> ended. */
	if (!h->brk)
		html_cp(h, HTML_BREAK);
	h->brk = 0;
	h->space = 0;
	h->started = 0;
}

/* The spaces of a run, which are one space, held until something shows whether
 * there is one worth keeping. */
static void
html_space(struct html *h)
{
	if (h->pre || !h->started)
		return;
	h->space = 1;
}

static void
html_flush_space(struct html *h)
{
	if (h->space) {
		html_cp(h, ' ');
		h->space = 0;
	}
}

/* Is this one of the spaces a browser folds together? */
static int
html_iswhite(unsigned char c)
{
	return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\f' ||
	    c == '\v';
}

/* What the name of a reference stands for, the name being without its & and ;
 * or 0 for a name the page leaves undefined, which is then left as it lies. */
static unsigned long
html_entity(const char *p, size_t len)
{
	static const struct {
		const char *name;
		unsigned long cp;
	} named[] = {
		{ "quot", 0x0022 }, { "amp", 0x0026 }, { "apos", 0x0027 },
		{ "lt", 0x003c }, { "gt", 0x003e }, { "nbsp", 0x00a0 },
		{ "iexcl", 0x00a1 }, { "cent", 0x00a2 }, { "pound", 0x00a3 },
		{ "curren", 0x00a4 }, { "yen", 0x00a5 }, { "brvbar", 0x00a6 },
		{ "sect", 0x00a7 }, { "uml", 0x00a8 }, { "copy", 0x00a9 },
		{ "ordf", 0x00aa }, { "laquo", 0x00ab }, { "not", 0x00ac },
		{ "reg", 0x00ae }, { "macr", 0x00af }, { "deg", 0x00b0 },
		{ "plusmn", 0x00b1 }, { "sup2", 0x00b2 }, { "sup3", 0x00b3 },
		{ "acute", 0x00b4 }, { "micro", 0x00b5 }, { "para", 0x00b6 },
		{ "middot", 0x00b7 }, { "cedil", 0x00b8 }, { "sup1", 0x00b9 },
		{ "ordm", 0x00ba }, { "raquo", 0x00bb }, { "frac14", 0x00bc },
		{ "frac12", 0x00bd }, { "frac34", 0x00be }, { "iquest", 0x00bf },
		{ "Agrave", 0x00c0 }, { "Aacute", 0x00c1 }, { "Acirc", 0x00c2 },
		{ "Atilde", 0x00c3 }, { "Auml", 0x00c4 }, { "Aring", 0x00c5 },
		{ "AElig", 0x00c6 }, { "Ccedil", 0x00c7 }, { "Egrave", 0x00c8 },
		{ "Eacute", 0x00c9 }, { "Ecirc", 0x00ca }, { "Euml", 0x00cb },
		{ "Igrave", 0x00cc }, { "Iacute", 0x00cd }, { "Icirc", 0x00ce },
		{ "Iuml", 0x00cf }, { "ETH", 0x00d0 }, { "Ntilde", 0x00d1 },
		{ "Ograve", 0x00d2 }, { "Oacute", 0x00d3 }, { "Ocirc", 0x00d4 },
		{ "Otilde", 0x00d5 }, { "Ouml", 0x00d6 }, { "times", 0x00d7 },
		{ "Oslash", 0x00d8 }, { "Ugrave", 0x00d9 }, { "Uacute", 0x00da },
		{ "Ucirc", 0x00db }, { "Uuml", 0x00dc }, { "Yacute", 0x00dd },
		{ "THORN", 0x00de }, { "szlig", 0x00df }, { "agrave", 0x00e0 },
		{ "aacute", 0x00e1 }, { "acirc", 0x00e2 }, { "atilde", 0x00e3 },
		{ "auml", 0x00e4 }, { "aring", 0x00e5 }, { "aelig", 0x00e6 },
		{ "ccedil", 0x00e7 }, { "egrave", 0x00e8 }, { "eacute", 0x00e9 },
		{ "ecirc", 0x00ea }, { "euml", 0x00eb }, { "igrave", 0x00ec },
		{ "iacute", 0x00ed }, { "icirc", 0x00ee }, { "iuml", 0x00ef },
		{ "eth", 0x00f0 }, { "ntilde", 0x00f1 }, { "ograve", 0x00f2 },
		{ "oacute", 0x00f3 }, { "ocirc", 0x00f4 }, { "otilde", 0x00f5 },
		{ "ouml", 0x00f6 }, { "divide", 0x00f7 }, { "oslash", 0x00f8 },
		{ "ugrave", 0x00f9 }, { "uacute", 0x00fa }, { "ucirc", 0x00fb },
		{ "uuml", 0x00fc }, { "yacute", 0x00fd }, { "thorn", 0x00fe },
		{ "yuml", 0x00ff }, { "OElig", 0x0152 }, { "oelig", 0x0153 },
		{ "Scaron", 0x0160 }, { "scaron", 0x0161 }, { "Yuml", 0x0178 },
		{ "fnof", 0x0192 }, { "circ", 0x02c6 }, { "tilde", 0x02dc },
		{ "ensp", 0x2002 }, { "emsp", 0x2003 }, { "thinsp", 0x2009 },
		{ "zwnj", 0x200c }, { "zwj", 0x200d }, { "lrm", 0x200e },
		{ "rlm", 0x200f }, { "ndash", 0x2013 }, { "mdash", 0x2014 },
		{ "lsquo", 0x2018 }, { "rsquo", 0x2019 }, { "sbquo", 0x201a },
		{ "ldquo", 0x201c }, { "rdquo", 0x201d }, { "bdquo", 0x201e },
		{ "dagger", 0x2020 }, { "Dagger", 0x2021 }, { "bull", 0x2022 },
		{ "hellip", 0x2026 }, { "permil", 0x2030 }, { "prime", 0x2032 },
		{ "Prime", 0x2033 }, { "lsaquo", 0x2039 }, { "rsaquo", 0x203a },
		{ "oline", 0x203e }, { "frasl", 0x2044 }, { "euro", 0x20ac },
		{ "trade", 0x2122 }, { "larr", 0x2190 }, { "uarr", 0x2191 },
		{ "rarr", 0x2192 }, { "darr", 0x2193 }, { "harr", 0x2194 },
		{ "crarr", 0x21b5 }, { "lArr", 0x21d0 }, { "uArr", 0x21d1 },
		{ "rArr", 0x21d2 }, { "dArr", 0x21d3 }, { "hArr", 0x21d4 },
		{ "forall", 0x2200 }, { "part", 0x2202 }, { "exist", 0x2203 },
		{ "empty", 0x2205 }, { "nabla", 0x2207 }, { "isin", 0x2208 },
		{ "notin", 0x2209 }, { "ni", 0x220b }, { "prod", 0x220f },
		{ "sum", 0x2211 }, { "minus", 0x2212 }, { "lowast", 0x2217 },
		{ "radic", 0x221a }, { "prop", 0x221d }, { "infin", 0x221e },
		{ "ang", 0x2220 }, { "and", 0x2227 }, { "or", 0x2228 },
		{ "cap", 0x2229 }, { "cup", 0x222a }, { "int", 0x222b },
		{ "there4", 0x2234 }, { "sim", 0x223c }, { "cong", 0x2245 },
		{ "asymp", 0x2248 }, { "ne", 0x2260 }, { "equiv", 0x2261 },
		{ "le", 0x2264 }, { "ge", 0x2265 }, { "sub", 0x2282 },
		{ "sup", 0x2283 }, { "nsub", 0x2284 }, { "sube", 0x2286 },
		{ "supe", 0x2287 }, { "oplus", 0x2295 }, { "otimes", 0x2297 },
		{ "perp", 0x22a5 }, { "sdot", 0x22c5 }, { "lceil", 0x2308 },
		{ "rceil", 0x2309 }, { "lfloor", 0x230a }, { "rfloor", 0x230b },
		{ "lang", 0x2329 }, { "rang", 0x232a }, { "loz", 0x25ca },
		{ "spades", 0x2660 }, { "clubs", 0x2663 }, { "hearts", 0x2665 },
		{ "diams", 0x2666 }
	};
	size_t i;

	if (len > 1 && p[0] == '#') {
		unsigned long v = 0;
		size_t k = 1;
		int any = 0, hex = len > 2 && (p[1] == 'x' || p[1] == 'X');

		if (hex)
			k = 2;
		for (; k < len; k++) {
			int d;

			if (p[k] >= '0' && p[k] <= '9')
				d = p[k] - '0';
			else if (hex && p[k] >= 'a' && p[k] <= 'f')
				d = p[k] - 'a' + 10;
			else if (hex && p[k] >= 'A' && p[k] <= 'F')
				d = p[k] - 'A' + 10;
			else
				return 0;
			if (v < 0x200000)
				v = v * (hex ? 16 : 10) + (unsigned long)d;
			any = 1;
		}
		return any ? v : 0;
	}
	for (i = 0; i < sizeof named / sizeof *named; i++)
		if (strlen(named[i].name) == len &&
		    memcmp(named[i].name, p, len) == 0)
			return named[i].cp;
	return 0;
}

/* A run of bytes with nothing in it that needs looking at. */
static void
html_raw(struct html *h, const char *p, size_t len)
{
	size_t i;

	for (i = 0; i < len; i++) {
		unsigned long cp = html_cp1252((unsigned char)p[i]);

		html_flush_space(h);
		html_cp(h, cp);
	}
}

/* Text, with the references turned into the characters they stand for and the
 * runs of spaces folded into one. */
static void
html_text(struct html *h, const char *p, size_t len)
{
	size_t i, run = 0;

	for (i = 0; i < len; i++) {
		unsigned char c = (unsigned char)p[i];

		if (c == '&') {
			const char *semi = memchr(p + i, ';', len - i);
			size_t n;

			if (semi != NULL && (n = (size_t)(semi - (p + i))) >= 2 &&
			    n <= 11) {
				unsigned long cp = html_entity(p + i + 1, n - 1);

				if (cp != 0) {
					if (run != 0) {
						html_raw(h, p + i - run, run);
						run = 0;
					}
					html_flush_space(h);
					html_cp(h, cp);
					i += n;
					continue;
				}
			}
		} else if (html_iswhite(c) && !h->pre) {
			if (run != 0) {
				html_raw(h, p + i - run, run);
				run = 0;
			}
			html_space(h);
			continue;
		}
		run++;
	}
	if (run != 0)
		html_raw(h, p + len - run, run);
}

/* The elements that start and end a paragraph.  The ones that only hold other
 * paragraphs, such as a table or a list, are not among them: what they hold
 * breaks the line by itself. */
static int
html_isblock(const char *n, size_t len)
{
	static const char *const names[] = {
		"p", "div", "h1", "h2", "h3", "h4", "h5", "h6", "blockquote",
		"pre", "li", "dt", "dd", "td", "th", "form", "fieldset",
		"address", "center", "dir", "menu", "hr", "article", "aside",
		"footer", "header", "main", "nav", "section", "figure",
		"figcaption", "noscript", "caption"
	};
	size_t i;

	for (i = 0; i < sizeof names / sizeof *names; i++)
		if (strlen(names[i]) == len && memcmp(names[i], n, len) == 0)
			return 1;
	return 0;
}

/* The elements whose contents are not document text.  A <meta> is not one of
 * them: it has no body, and the ones inside a head are where the metadata of
 * the page is written, which is exactly what is wanted here. */
static int
html_isskip(const char *n, size_t len)
{
	static const char *const names[] = {
		"script", "style", "title"
	};
	size_t i;

	for (i = 0; i < sizeof names / sizeof *names; i++)
		if (strlen(names[i]) == len && memcmp(names[i], n, len) == 0)
			return 1;
	return 0;
}

/* Does this begin a tag? */
static int
html_isname(unsigned char c)
{
	return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '/' ||
	    c == '!' || c == '?';
}

/* A tag name, compared without regard to case. */
static int
html_is(const char *n, size_t len, const char *want)
{
	size_t i;

	if (strlen(want) != len)
		return 0;
	for (i = 0; i < len; i++) {
		int a = n[i], b = want[i];

		if (a >= 'A' && a <= 'Z')
			a += 'a' - 'A';
		if (b >= 'A' && b <= 'Z')
			b += 'a' - 'A';
		if (a != b)
			return 0;
	}
	return 1;
}

/* Copy a name or a value into one of the two scratch buffers, growing it. */
static int
html_save(char **buf, size_t *len, size_t *cap, const char *p, size_t n)
{
	if (*len < n + 1) {
		size_t want = n + 64;
		char *b = realloc(*buf, want);

		if (b == NULL)
			return 0;
		*buf = b;
		*cap = want;
	}
	memcpy(*buf, p, n);
	(*buf)[n] = '\0';
	*len = n + 1;
	return 1;
}

/* A <meta> tag, whose name and content are metadata.  Either attribute may
 * come first, so both are collected and placed when the tag ends.  A name with
 * no content is metadata whose value is empty, which -info prints as a line
 * with nothing after the colon. */
static void
html_meta(struct html *h)
{
	const char *name = h->mname, *val = h->mval;
	size_t nlen = h->mnamelen - 1, vlen = h->mvallen - 1;
	const char **slot = NULL;
	int isdate = 0;

	h->mnamelen = 0;
	h->mvallen = 0;
	if (h->m == NULL || name == NULL || val == NULL)
		return;
	/* The name is what says which field this is, and the reference tool
	 * takes the two attributes in either order.  A name it does not
	 * report, such as Generator, is read and then left out. */
	if (html_is(name, nlen, "author"))
		slot = &h->m->author;
	else if (html_is(name, nlen, "lastauthor"))
		slot = &h->m->editor;
	else if (html_is(name, nlen, "company"))
		slot = &h->m->company;
	else if (html_is(name, nlen, "subject"))
		slot = &h->m->subject;
	else if (html_is(name, nlen, "keywords"))
		slot = &h->m->keywords;
	else if (html_is(name, nlen, "description"))
		slot = &h->m->comment;
	else if (html_is(name, nlen, "creationtime"))
		slot = &h->m->creationtime, isdate = 1;
	else if (html_is(name, nlen, "modificationtime"))
		slot = &h->m->modificationtime, isdate = 1;
	if (slot == NULL)
		return;
	/* A time is kept as the broken-down form the option parser uses,
	 * which is the form a Cocoa HTML page writes, and -info turns it
	 * into the form it prints.  A value too short to hold one is not a
	 * time, and is left out rather than shown as it lies. */
	if (isdate) {
		struct tu_time t;
		char buf[32];

		if (vlen < 20)
			return;
		{
			char in[24];

			memcpy(in, val, 20);
			in[20] = '\0';
			if (!tu_parse_timestamp(in, &t))
				return;
		}
		snprintf(buf, sizeof(buf), "%04ld-%02d-%02dT%02d:%02d:%02dZ",
		    t.year, t.month, t.day, t.hour, t.minute, t.second);
		free((char *)*slot);
		*slot = strdup(buf);
		return;
	}
	free((char *)*slot);
	*slot = strdup(val);
}

/* The attributes of a tag, and what the tag itself does. */
static void
html_tag(struct html *h, const char *p, const char *end, int closing)
{
	const char *name = p, *attrs;
	size_t namelen, i;
	int ismeta = 0, selfclose = 0;

	while (name < end && !html_iswhite((unsigned char)*name) &&
	    *name != '/' && *name != '>')
		name++;
	namelen = (size_t)(name - p);
	if (namelen == 0)
		return;
	attrs = name;
	if (html_is(p, namelen, "meta"))
		ismeta = 1;
	if (h->mname != NULL || h->mval != NULL) {
		h->mnamelen = 0;
		h->mvallen = 0;
	}
	/* The attributes, which is where the metadata of a meta tag is. */
	if (ismeta && h->m != NULL) {
		const char *q = attrs;

		while (q < end) {
			const char *an, *av;
			size_t anlen, avlen;

			while (q < end && html_iswhite((unsigned char)*q))
				q++;
			if (q >= end || *q == '/')
				break;
			an = q;
			while (q < end && !html_iswhite((unsigned char)*q) &&
			    *q != '=' && *q != '/')
				q++;
			anlen = (size_t)(q - an);
			av = NULL;
			avlen = 0;
			if (q < end && *q == '=') {
				char quote;

				q++;
				if (q < end && (*q == '"' || *q == '\'')) {
					quote = *q++;
					av = q;
					while (q < end && *q != quote)
						q++;
					avlen = (size_t)(q - av);
					if (q < end)
						q++;
				} else {
					av = q;
					while (q < end && !html_iswhite(
					    (unsigned char)*q) && *q != '/')
						q++;
					avlen = (size_t)(q - av);
				}
			}
			/* A meta tag names a field and gives its value, and
			 * the two attributes may come in either order, so
			 * they are picked out by what they are called
			 * rather than by where they lie. */
			if (anlen != 0 && (html_is(an, anlen, "name") ||
			    html_is(an, anlen, "http-equiv")) &&
			    !html_save(&h->mname, &h->mnamelen, &h->mcap,
			    av != NULL ? av : "", av != NULL ? avlen : 0))
				return;
			if (anlen != 0 && html_is(an, anlen, "content") &&
			    !html_save(&h->mval, &h->mvallen, &h->mvcap,
			    av != NULL ? av : "", av != NULL ? avlen : 0))
				return;
		}
	}
	/* An empty element is one that ends in a slash, which is not the
	 * same as a slash anywhere in the tag: a value may hold one. */
	for (i = (size_t)(end - p); i > 0; i--)
		if (!html_iswhite((unsigned char)p[i - 1])) {
			selfclose = p[i - 1] == '/';
			break;
		}
	if (ismeta && !closing)
		html_meta(h);
	/* Inside a part that is passed over only the end of it is of any
	 * interest, and the metadata of a head is read wherever it lies. */
	if (h->skip != 0 && !ismeta) {
		if (html_isskip(p, namelen) && closing && h->skip > 0)
			h->skip--;
		else if (html_isskip(p, namelen) && !closing)
			h->skip++;
		return;
	}
	/* The elements that have no end of their own, and so are taken as
	 * they open: a <br> breaks the line, an <hr> breaks it twice, and
	 * an <img> is an attachment in the text. */
	if (!closing && (html_is(p, namelen, "br") ||
	    html_is(p, namelen, "hr") || html_is(p, namelen, "img") ||
	    html_is(p, namelen, "input") || selfclose)) {
		if (html_is(p, namelen, "br")) {
			html_flush_space(h);
			html_cp(h, HTML_BREAK);
			h->brk = 1;
			h->started = 0;
		} else if (html_is(p, namelen, "hr")) {
			html_break(h);
			html_cp(h, HTML_BREAK);
		} else if (html_is(p, namelen, "img") ||
		    html_is(p, namelen, "input"))
			html_cp(h, 0xfffc);
		return;
	}
	if (closing) {
		if (html_isskip(p, namelen)) {
			if (h->skip > 0)
				h->skip--;
		} else if (html_is(p, namelen, "pre") ||
		    html_is(p, namelen, "textarea")) {
			h->pre = 0;
			if (html_is(p, namelen, "pre"))
				html_endpara(h);
		}
		else if (html_is(p, namelen, "q"))
			html_cp(h, 0x201d);
		else if (html_is(p, namelen, "li") || html_is(p, namelen, "dd") ||
		    html_is(p, namelen, "dt"))
			html_endpara(h);
		else if (html_is(p, namelen, "ul") || html_is(p, namelen, "ol")) {
			/* A list with nothing in it is laid out as a
			 * mark and nothing else, which is one more than
			 * leaving it out would be. */
			if (h->items == 0) {
				html_break(h);
				html_cp(h, '\t');
				if (h->ordered) {
					char num[16];

					snprintf(num, sizeof(num), "%d",
					    h->item);
					html_raw(h, num, strlen(num));
				} else
					html_cp(h, 0x2022);
				html_cp(h, '\t');
				h->started = 0;
				html_endpara(h);
			} else
				html_break(h);
			if (h->listdepth > 0)
				h->listdepth--;
		} else if (html_isblock(p, namelen))
			html_endpara(h);
		return;
	}
	if (html_isskip(p, namelen)) {
		h->skip++;
		return;
	}
	if (html_is(p, namelen, "pre") || html_is(p, namelen, "textarea"))
		h->pre = 1;
	if (html_is(p, namelen, "q")) {
		html_cp(h, 0x201c);
		return;
	}
	if (html_is(p, namelen, "ul") || html_is(p, namelen, "ol")) {
		html_break(h);
		if (h->listdepth++ == 0) {
			h->item = 1;
			h->items = 0;
			h->ordered = html_is(p, namelen, "ol");
		}
		return;
	}
	if (html_is(p, namelen, "li") || html_is(p, namelen, "dd") ||
	    html_is(p, namelen, "dt")) {
		html_break(h);
		h->items++;
		if (h->listdepth > 0) {
			/* A tab, a mark and a tab, then the item.  A
			 * mark is a bullet, or the item's number in a
			 * list that counts.  What the mark is written as
			 * is not something written, so a break asked for
			 * next does not fall between it and the item. */
			html_cp(h, '\t');
			if (h->ordered) {
				char num[16];

				snprintf(num, sizeof(num), "%d", h->item++);
				html_raw(h, num, strlen(num));
			} else
				html_cp(h, 0x2022);
			html_cp(h, '\t');
			h->started = 0;
		}
		return;
	}
	if (html_isblock(p, namelen))
		html_break(h);
}

static void
html_run(struct html *h, const char *p, const char *end)
{
	h->p = p;
	h->end = end;
	h->item = 1;
	while (p < end) {
		if (*p == '<' && p + 1 < end && html_isname((unsigned char)p[1])) {
			const char *q = p + 1;
			int closing = 0;

			if (q < end && *q == '!') {
				/* A comment, a declaration or a processing
				 * instruction: none of it is text. */
				if (end - q >= 3 && q[1] == '-' && q[2] == '-') {
					const char *stop = q + 3;

					while (stop + 2 < end &&
					    !(stop[0] == '-' && stop[1] == '-' &&
					    stop[2] == '>'))
						stop++;
					p = stop + 2 < end ? stop + 3 : end;
				} else {
					while (q < end && *q != '>')
						q++;
					p = q < end ? q + 1 : end;
				}
				continue;
			}
			if (q < end && *q == '?') {
				while (q < end && *q != '>')
					q++;
				p = q < end ? q + 1 : end;
				continue;
			}
			if (q < end && *q == '/') {
				closing = 1;
				q++;
			}
			{
				/* A > inside a quoted value does not end the
				 * tag, and the name itself ends at the first
				 * space. */
				const char *name = q;
				int empty = 0;
				char quote = 0;

				while (q < end) {
					if (quote != 0) {
						if (*q == quote)
							quote = 0;
					} else if (*q == '"' || *q == '\'')
						quote = *q;
					else if (*q == '>')
						break;
					q++;
				}
				if (q >= end)
					empty = 1;
				else if (q > name && q[-1] == '/')
					empty = 1;
				if (!empty)
					html_tag(h, name, q, closing);
				p = q < end ? q + 1 : end;
			}
			continue;
		}
		{
			const char *q = p;
			size_t len;

			while (q < end && !(*q == '<' && q + 1 < end &&
			    html_isname((unsigned char)q[1])))
				q++;
			len = (size_t)(q - p);
			if (h->skip == 0) {
				if (h->pre)
					html_raw(h, p, len);
				else
					html_text(h, p, len);
			}
			p = q;
		}
	}
}

/* Read an HTML file into its text and the metadata -info reports.  A NULL out
 * is not filled in.  Returns 0 on success. */
int
tu_read_html(const void *buf, size_t len, tu_doc_t *out, tu_meta_t *meta)
{
	struct html h;

	memset(&h, 0, sizeof(h));
	h.m = meta;
	html_run(&h, buf, (const char *)buf + len);
	if (h.text.buf == NULL)
		sink_put(&h.text, "", 0);
	if (h.text.failed) {
		free(h.text.buf);
		free(h.mname);
		free(h.mval);
		/* A read that fails leaves out empty, for the same reason and
		 * with the same rule as tu_read_rtf. */
		if (out != NULL) {
			out->text = NULL;
			out->len = 0;
			out->nchars = 0;
		}
		return TU_READ_UNOPENABLE;
	}
	if (out != NULL) {
		out->text = h.text.buf;
		out->len = h.text.len;
		out->nchars = h.nchars;
	} else
		free(h.text.buf);
	free(h.mname);
	free(h.mval);
	return 0;
}
