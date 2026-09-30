/*
 * textutil - Word 2003 XML (wordml) output.
 *
 * One flat XML document, no container and no timestamps, which makes it the
 * one office format whose bytes can be compared directly.  The envelope is
 * fixed: the same namespace block every time, an empty w:docPr, and a
 * w:fonts table naming Times New Roman whatever the document's own font is.
 * Only the metadata group and the paragraphs carry anything.
 *
 * Copyright (C) 2026, LibreDarwin
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "textutil.h"

/* Two processing instructions, one to a line, and then the opening tag.  The
 * first two lines end in a newline and the opening tag does not: the body
 * begins on the same line as the tag, and so does the closing one, and the
 * file ends without a newline of its own. */
static const char wml_preamble[] =
    "<?xml version=\"1.0\" encoding=\"UTF-8\" standalone=\"yes\"?>\n"
    "<?mso-application progid=\"Word.Document\"?>\n"
    "<w:wordDocument "
    "xmlns:w=\"http://schemas.microsoft.com/office/word/2003/wordml\" "
    "xmlns:v=\"urn:schemas-microsoft-com:vml\" "
    "xmlns:w10=\"urn:schemas-microsoft-com:office:word\" "
    "xmlns:sl=\"http://schemas.microsoft.com/schemaLibrary/2003/core\" "
    "xmlns:aml=\"http://schemas.microsoft.com/aml/2001/core\" "
    "xmlns:wx=\"http://schemas.microsoft.com/office/word/2003/auxHint\" "
    "xmlns:o=\"urn:schemas-microsoft-com:office:office\" "
    "xmlns:dt=\"uuid:C2F41010-65B3-11d1-A29F-00AA00C14882\" "
    "w:macrosPresent=\"no\" w:embeddedObjPresent=\"no\" w:ocxPresent=\"no\" "
    "xml:space=\"preserve\">";

static const char wml_postamble[] =
    "</w:wordDocument>";

/* The order here is not the order the options are given in, and it is not
 * alphabetical either: title, subject, author, keywords, description, last
 * author, the two times, company.  A value that was never given is left out
 * entirely, which is the same rule the other writers use. */
static const struct {
	size_t offset;
	const char *name;
} wml_meta[] = {
	{ offsetof(tu_meta_t, title),			"Title" },
	{ offsetof(tu_meta_t, subject),		"Subject" },
	{ offsetof(tu_meta_t, author),		"Author" },
	{ offsetof(tu_meta_t, keywords),		"Keywords" },
	{ offsetof(tu_meta_t, comment),		"Description" },
	{ offsetof(tu_meta_t, editor),		"LastAuthor" },
	{ offsetof(tu_meta_t, creationtime),		"Created" },
	{ offsetof(tu_meta_t, modificationtime),	"LastSaved" },
	{ offsetof(tu_meta_t, company),		"Company" }
};

static void
wml_text(FILE *f, const char *p, size_t n)
{
	/* A less than sign, an ampersand and a greater than sign are written as
	 * entities.  A quote and an apostrophe are not, which is not what a
	 * generic XML escaper would do, and neither is anything else: the
	 * document is UTF-8 and every other byte is written through.
	 * xml:space is set on the document element, so leading and trailing
	 * spaces inside w:t survive without the reference tool writing
	 * xml:space on the element itself. */
	for (size_t i = 0; i < n; i++) {
		if (p[i] == '<')
			fputs("&lt;", f);
		else if (p[i] == '&')
			fputs("&amp;", f);
		else if (p[i] == '>')
			fputs("&gt;", f);
		else
			fputc((unsigned char)p[i], f);
	}
}

/* The text of a run, which is the body with the C0 controls dropped: all but
 * the three that are text in their own right, tab, line feed and carriage
 * return.  U+007F is kept.  This is done on the way out rather than before the
 * text was split into paragraphs, because a control is not a paragraph mark and
 * cannot bring a paragraph about, but it is not text either and must not be
 * written. */
static void
wml_body_text(FILE *f, const char *p, size_t n)
{
	for (size_t i = 0; i < n; i++) {
		unsigned char c = (unsigned char)p[i];

		if (c < 0x20 && c != '\t' && c != '\n' && c != '\r')
			continue;
		wml_text(f, &p[i], 1);
	}
}

static void
wml_meta_group(FILE *f, const tu_meta_t *m)
{
	if (m == NULL)
		return;
	for (size_t i = 0; i < sizeof(wml_meta) / sizeof(wml_meta[0]); i++) {
		const char *v = *(const char *const *)((const char *)m +
		    wml_meta[i].offset);

		if (v == NULL)
			continue;
		fprintf(f, "<o:%s>", wml_meta[i].name);
		wml_text(f, v, strlen(v));
		fprintf(f, "</o:%s>", wml_meta[i].name);
	}
}

static void
wml_rpr(FILE *f, const tu_font_t *face, int points)
{
	fprintf(f, "<w:rPr><w:rFonts w:ascii=\"%s\" w:h-ansi=\"%s\" w:cs=\"%s\"/>"
	    "<wx:font wx:val=\"%s\"/><w:sz w:val=\"%d\"/><w:sz-cs w:val=\"%d\"/>",
	    face->wml, face->wml, face->wml, face->wml, points * 2, points * 2);
	/* Bold before italic, which is the opposite of the order RTF writes
	 * the two run attributes in. */
	if (face->bold)
		fputs("<w:b/>", f);
	if (face->italic)
		fputs("<w:i/>", f);
	fputs("</w:rPr>", f);
}

/* The contents of one paragraph, which is a sequence of text runs and the two
 * inline elements, with a w:t around every piece of text whether or not that
 * piece is empty.  A paragraph of plain text is therefore one w:t, a tab splits
 * the text either side of it into two, and a paragraph that is nothing but a tab
 * is an empty w:t, the tab, and another empty w:t. */
static void
wml_run(FILE *f, const char *p, size_t n)
{
	size_t start = 0;

	for (size_t i = 0; i < n; i++) {
		const char *elem = NULL;
		size_t width = 1;

		if (p[i] == '\t') {
			elem = "<w:tab/>";
		} else if (p[i] == '\f') {
			elem = "<w:br w:type=\"page\"/>";
		} else if (p[i] == (char)0xE2 && n - i >= 3 &&
		    (unsigned char)p[i + 1] == 0x80 &&
		    (unsigned char)p[i + 2] == 0xA8) {
			/* A line separator is a break inside the paragraph
			 * rather than a new one.  The paragraph separator in the
			 * same block is a new paragraph, so the loop below
			 * never hands one of those over; matching it here as
			 * well is a guard against writing one as text. */
			elem = "<w:br/>";
			width = 3;
		}
		if (elem == NULL)
			continue;
		fputs("<w:t>", f);
		wml_body_text(f, p + start, i - start);
		fputs("</w:t>", f);
		fputs(elem, f);
		i += width - 1;
		start = i + 1;
	}
	fputs("<w:t>", f);
	wml_body_text(f, p + start, n - start);
	fputs("</w:t>", f);
}

/* The levels the text of the paragraph being written is under.  A run is a
 * stretch of the text under one stack of them, so the text either side of a
 * control that leaves the stack where it was is one run and not two, and the
 * stack is compared by what is in it rather than by how deep it is.  The
 * scratch space each paragraph needs is kept here and allocated once. */
struct wml_embed {
	unsigned long *stack;		/* the levels the text is under */
	size_t dep;			/* how deep stack is */
	unsigned long *prev;		/* the stack the last byte was under */
	size_t prev_dep;
	char *buf;			/* the text with the controls taken out */
	size_t *rid;			/* which run each byte of buf is in */
	size_t *rdep;			/* how deep the run of each byte is */
};

/* Whether two stacks of levels are the same, which is what leaves the text
 * either side of a control one run. */
static int
wml_same(const unsigned long *a, size_t na, const unsigned long *b, size_t nb)
{
	return na == nb && (na == 0 || memcmp(a, b, na * sizeof(*a)) == 0);
}

/* A run with no text of its own, which is what an empty paragraph and the tail
 * of a paragraph that ends with a level still open are written as. */
static void
wml_empty_run(FILE *f, const tu_font_t *face, int points)
{
	fputs("<w:r>", f);
	wml_rpr(f, face, points);
	fputs("<w:t></w:t></w:r>", f);
}

/* Write the runs of one paragraph, whose text begins at p, and say whether any
 * of them was written under a level.  A level is a direction given to the text
 * it covers rather than a character the reader is shown, so it is taken out of
 * the text and the text is cut wherever the levels open over it change; a level
 * that is opened and closed with no text between leaves the text either side of
 * it one run.  The wordml writer has no way to record the direction itself and
 * splits its runs all the same.  Returns the number of bytes of text written,
 * which is zero for a paragraph that is nothing but levels. */
static size_t
wml_runs(FILE *f, const char *p, size_t n, const tu_font_t *face, int points,
    struct wml_embed *e, int *under)
{
	size_t m = 0, k, r;

	e->dep = 0;
	e->prev_dep = 0;
	for (size_t i = 0; i < n; ) {
		unsigned long cp = tu_bidi_embed((const unsigned char *)p + i,
		    n - i);

		if (cp != 0) {
			if (cp == 0x202C) {
				if (e->dep > 0)
					e->dep--;
			} else {
				e->stack[e->dep++] = cp;
			}
			i += 3;
			continue;
		}
		if (m == 0)
			r = 0;
		else if (wml_same(e->stack, e->dep, e->prev, e->prev_dep))
			r = e->rid[m - 1];
		else
			r = e->rid[m - 1] + 1;
		e->rid[m] = r;
		e->rdep[m] = e->dep;
		e->buf[m] = p[i];
		memcpy(e->prev, e->stack, e->dep * sizeof(*e->stack));
		e->prev_dep = e->dep;
		m++;
		i++;
	}

	for (k = 0; k < m; ) {
		size_t j = k;

		while (j < m && e->rid[j] == e->rid[k])
			j++;
		fputs("<w:r>", f);
		wml_rpr(f, face, points);
		wml_run(f, e->buf + k, j - k);
		fputs("</w:r>", f);
		k = j;
	}
	/* Whether the text ends with a level still open over it, which is what
	 * the paragraph closes with an empty run of its own. */
	*under = m > 0 && e->rdep[m - 1] > 0;
	return m;
}

/* The width of the paragraph mark at i, which is a carriage return, a line
 * feed, or the two of them in that order and worth one mark rather than two.  A
 * line feed followed by a carriage return is two marks and not one, and writes
 * an empty paragraph between the two the same way two newlines do.  A level or
 * a mark between the two is not shown to a reader and does not come between
 * them here either: the reference tool takes those out before it looks for the
 * end of a paragraph, so a carriage return and a line feed with only those
 * between them are still one mark.
 *
 * A mark is the exception, and only at the head.  A carriage return begins a
 * line, and the head of a line may name a direction, so a mark immediately
 * after it is that mark and is gone -- and only then does the search for the
 * line feed begin, going over the levels and no further.  A second mark, or a
 * mark that a level came before, is text between the two halves of what would
 * have been a pair, and so keeps them apart. */
static size_t
wml_mark_width(const char *p, size_t n, size_t i)
{
	if (p[i] == '\r') {
		size_t j = i + 1;

		j += tu_bidi_mark((const unsigned char *)p + j, n - j);
		while (j < n && tu_bidi_embed((const unsigned char *)p + j,
		    n - j) != 0)
			j += 3;
		return j < n && p[j] == '\n' ? j - i + 1 : 1;
	}
	if (p[i] == '\n')
		return 1;
	return 0;
}

/* Whether the three bytes at i are U+2029, the paragraph separator, which ends
 * a paragraph the way a carriage return does.  Read as a whole so that a
 * truncated sequence at the end of the buffer is not a match. */
static int
wml_is_para_sep(const char *p, size_t n, size_t i)
{
	return n - i >= 3 && (unsigned char)p[i] == 0xE2 &&
	    (unsigned char)p[i + 1] == 0x80 && (unsigned char)p[i + 2] == 0xA9;
}

int
tu_write_wordml(const tu_doc_t *d, const char *path,
    const tu_style_t *st, const tu_meta_t *meta)
{
	tu_font_t face;
	FILE *f;
	const char *p;
	size_t i = 0, start = 0, len;
	int points;
	struct wml_embed e;
	int empty, failed;

	tu_font_face(st != NULL ? st->font : NULL, &face);
	points = st != NULL && st->fontsize > 0 ? st->fontsize : 12;
	if ((f = fopen(path, "wb")) == NULL) {
		tu_write_failed(path);
		return -1;
	}

	fputs(wml_preamble, f);
	fputs("<o:DocumentProperties>", f);
	wml_meta_group(f, meta);
	fputs("</o:DocumentProperties>", f);
	/* The document's own font does not reach this table, which names
	 * Times New Roman in all four slots every time. */
	fputs("<w:fonts><w:defaultFonts w:ascii=\"Times New Roman\" "
	    "w:fareast=\"Times New Roman\" w:h-ansi=\"Times New Roman\" "
	    "w:cs=\"Times New Roman\"/></w:fonts>", f);
	fputs("<w:docPr></w:docPr>", f);
	fputs("<w:body><wx:sect>", f);

	p = d->text != NULL ? d->text : "";
	len = d->len;
	/* A document of nothing writes a paragraph and nothing else: no pPr, no
	 * run, and no w:t, which is the one place the body is not what the loop
	 * below writes for the same text.  A document whose text is nothing but
	 * directions is the same nothing. */
	empty = !tu_bidi_para((const unsigned char *)p, len);
	memset(&e, 0, sizeof(e));
	failed = len / 3 + 2 > SIZE_MAX / sizeof(*e.stack);
	if (!empty && !failed) {
		e.stack = malloc((len / 3 + 2) * sizeof(*e.stack));
		e.prev = malloc((len / 3 + 2) * sizeof(*e.prev));
		e.buf = malloc(len);
		e.rid = malloc(len * sizeof(*e.rid));
		e.rdep = malloc(len * sizeof(*e.rdep));
		failed = e.stack == NULL || e.prev == NULL || e.buf == NULL ||
		    e.rid == NULL || e.rdep == NULL;
	}
	if (failed) {
		free(e.stack);
		free(e.prev);
		free(e.buf);
		free(e.rid);
		free(e.rdep);
		fclose(f);
		tu_write_failed(path);
		return -1;
	}

	if (empty) {
		fputs("<w:p></w:p>", f);
	} else {
		for (;;) {
			size_t mark = i < len ?
			    wml_mark_width(p, len, i) : 0;
			int sep = i < len && wml_is_para_sep(p, len, i);
			int terminated = i < len;
			unsigned long bm;
			size_t head, written;
			int under = 0;

			if (i == len) {
				/* A mark at the very end does not begin a
				 * paragraph, so the loop ends here rather than
				 * writing an empty one after it; neither does
				 * a tail that is nothing but directions. */
				if (start == i ||
				    !tu_bidi_para((const unsigned char *)p +
				    start, i - start))
					break;
			} else if (mark == 0 && !sep) {
				i++;
				continue;
			}
			/* A mark that opens the paragraph says which way it reads
			 * and is not text, so it goes into the paragraph's
			 * properties instead of into its run.  Only the
			 * right-to-left one is recorded: the left-to-right mark
			 * names a direction the paragraph already has. */
			bm = tu_bidi_open((const unsigned char *)p + start,
			    i - start);
			head = start + (bm != 0 ? 3 : 0);

			fputs("<w:p><w:pPr>", f);
			if (bm == 0x200F)
				fputs("<w:bidi/>", f);
			fputs("</w:pPr>", f);
			written = wml_runs(f, p + head, i - head, &face, points,
			    &e, &under);
			/* An empty paragraph is one empty run, and a
			 * paragraph that ends with a level still open over its
			 * text closes it with an empty run after the text. */
			if (written == 0)
				wml_empty_run(f, &face, points);
			if (terminated && under)
				wml_empty_run(f, &face, points);
			fputs("</w:p>", f);

			if (i == len)
				break;
			start = i + (mark != 0 ? mark : 3);
			i = start;
		}
	}
	free(e.stack);
	free(e.prev);
	free(e.buf);
	free(e.rid);
	free(e.rdep);
	fputs("<w:sectPr></w:sectPr>", f);
	fputs("</wx:sect></w:body>", f);
	fputs(wml_postamble, f);

	if (ferror(f) || fclose(f) != 0) {
		tu_write_failed(path);
		return -1;
	}
	return 0;
}
