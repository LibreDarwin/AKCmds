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
		wml_text(f, p + start, i - start);
		fputs("</w:t>", f);
		fputs(elem, f);
		i += width - 1;
		start = i + 1;
	}
	fputs("<w:t>", f);
	wml_text(f, p + start, n - start);
	fputs("</w:t>", f);
}

/* The width of the paragraph mark at i, which is a carriage return, a line
 * feed, or the two of them in that order and worth one mark rather than two.  A
 * line feed followed by a carriage return is two marks and not one, and writes
 * an empty paragraph between the two the same way two newlines do. */
static size_t
wml_mark_width(const char *p, size_t n, size_t i)
{
	if (p[i] == '\r')
		return i + 1 < n && p[i + 1] == '\n' ? 2 : 1;
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
	/* A document with nothing in it is a paragraph and nothing else: no
	 * pPr, no run, and no w:t, which is the one place the body is not
	 * what the loop below would write for the same text. */
	if (len == 0) {
		fputs("<w:p></w:p>", f);
	} else {
		for (;;) {
			size_t mark = i < len ?
			    wml_mark_width(p, len, i) : 0;
			int sep = i < len && wml_is_para_sep(p, len, i);

			if (i == len) {
				/* A mark at the very end does not begin a
				 * paragraph, so the loop ends here rather than
				 * writing an empty one after it. */
				if (start == i)
					break;
			} else if (mark == 0 && !sep) {
				i++;
				continue;
			}
			fputs("<w:p><w:pPr></w:pPr><w:r>", f);
			wml_rpr(f, &face, points);
			wml_run(f, p + start, i - start);
			fputs("</w:r></w:p>", f);
			if (i == len)
				break;
			start = i + (mark != 0 ? mark : 3);
			i = start;
		}
	}
	fputs("<w:sectPr></w:sectPr>", f);
	fputs("</wx:sect></w:body>", f);
	fputs(wml_postamble, f);

	if (ferror(f) || fclose(f) != 0) {
		tu_write_failed(path);
		return -1;
	}
	return 0;
}
