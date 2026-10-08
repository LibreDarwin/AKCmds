#include "textutil.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "docx_static.h"
#include "zip.h"

/* The docx writer.  The document member is the wordml model, byte for byte:
 * the same paragraph splitting, the same marks at the head, the same
 * run-splitting over the embedding levels and the same empty document -- a
 * bare <w:p></w:p> -- and a trailing direction-only tail that is not written.
 * The markup is the difference, and every string of it was matched to the
 * reference: a paragraph opens with its pPr and an optional <w:bidi/> for a
 * right-to-left mark, a run carries its rFonts (the family the face belongs
 * to, in the docx camel-case name), the size in half-points and the bold and
 * italic attributes, and text is always a <w:t xml:space="preserve">, empty,
 * between two pieces, or not.  A tab is <w:tab/>, a form feed a page break,
 * a line separator <w:br/>.  The other seven members are fixed byte strings
 * from docx_static.h except the core properties, which carry the -title and
 * -author groups. */

/* A growable byte buffer, the two dynamic members being built whole. */
struct dbuf {
	char *b;
	size_t len, cap;
};

static int
dsz(struct dbuf *d, const void *s, size_t n)
{
	if (d->len + n > d->cap) {
		size_t cap = d->cap ? d->cap * 2 : 512;
		char *b;

		while (cap < d->len + n)
			cap *= 2;
		b = realloc(d->b, cap);
		if (b == NULL)
			return -1;
		d->b = b;
		d->cap = cap;
	}
	memcpy(d->b + d->len, s, n);
	d->len += n;
	return 0;
}

static int
dup(struct dbuf *d, const char *s)
{
	return dsz(d, s, strlen(s));
}

/* Text escaped for a w:t.  Only the three characters that mark up the document
 * are written long, as the reference does in the text it writes. */
static int
esc(struct dbuf *d, const char *s, size_t n)
{
	size_t i = 0;

	while (i < n) {
		size_t j = i;

		while (j < n && s[j] != '&' && s[j] != '<' && s[j] != '>')
			j++;
		if (dsz(d, s + i, j - i) != 0)
			return -1;
		if (j < n) {
			if (s[j] == '&' && dsz(d, "&amp;", 5) != 0)
				return -1;
			if (s[j] == '<' && dsz(d, "&lt;", 4) != 0)
				return -1;
			if (s[j] == '>' && dsz(d, "&gt;", 4) != 0)
				return -1;
		}
		i = j + 1;
	}
	return 0;
}

/* The text of a run, which is the paragraph text with the C0 controls dropped:
 * all but the three that are text in their own right, tab, line feed and
 * carriage return.  U+007F is kept. */
static int
run_text(struct dbuf *d, const char *p, size_t n)
{
	for (size_t i = 0; i < n; i++) {
		unsigned char c = (unsigned char)p[i];

		if (c < 0x20 && c != '\t' && c != '\n' && c != '\r')
			continue;
		if (esc(d, &p[i], 1) != 0)
			return -1;
	}
	return 0;
}

/* The run properties, which name the family the face belongs to and the size
 * in half-points.  The docx writer spells the second slot w:hAnsi, where the
 * wordml writer spells it w:h-ansi, and has no wx:font element.  Bold before
 * italic, the same order as wordml. */
static int
docx_rpr(struct dbuf *d, const tu_font_t *face, int points)
{
	char sz[64];

	if (dup(d, "<w:rPr><w:rFonts w:ascii=\"") != 0 ||
	    dup(d, face->wml) != 0 ||
	    dup(d, "\" w:hAnsi=\"") != 0 || dup(d, face->wml) != 0 ||
	    dup(d, "\" w:cs=\"") != 0 || dup(d, face->wml) != 0 ||
	    dup(d, "\"/>") != 0)
		return -1;
	snprintf(sz, sizeof sz, "<w:sz w:val=\"%d\"/><w:sz-cs w:val=\"%d\"/>",
	    points * 2, points * 2);
	if (dup(d, sz) != 0)
		return -1;
	if (face->bold && dup(d, "<w:b/>") != 0)
		return -1;
	if (face->italic && dup(d, "<w:i/>") != 0)
		return -1;
	return dup(d, "</w:rPr>");
}

/* The contents of one piece of a run: the text with the inline elements, with
 * a w:t around every piece of text whether or not that piece is empty.  A tab
 * is <w:tab/>, a form feed a page break, a line separator a break inside the
 * paragraph.  A piece of nothing but what is dropped is an empty w:t. */
static int
docx_piece(struct dbuf *d, const char *p, size_t n)
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
			elem = "<w:br/>";
			width = 3;
		}
		if (elem == NULL)
			continue;
		if (dup(d, W_T_OPEN) != 0 || run_text(d, p + start,
		    i - start) != 0 || dup(d, W_T_CLOSE) != 0)
			return -1;
		if (dup(d, elem) != 0)
			return -1;
		i += width - 1;
		start = i + 1;
	}
	return dup(d, W_T_OPEN) != 0 || run_text(d, p + start,
	    n - start) != 0 || dup(d, W_T_CLOSE) != 0;
}

/* The levels the text of the paragraph being written is under.  A run is a
 * stretch of the text under one stack of them, so the text either side of a
 * control that leaves the stack where it was is one run and not two, and the
 * stack is compared by what is in it rather than by how deep it is.  The
 * scratch space each paragraph needs is kept here and allocated once. */
struct docx_embed {
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
docx_same(const unsigned long *a, size_t na, const unsigned long *b, size_t nb)
{
	return na == nb && (na == 0 || memcmp(a, b, na * sizeof(*a)) == 0);
}

/* A run with no text of its own, which is what an empty paragraph and the tail
 * of a paragraph that ends with a level still open are written as. */
static int
docx_empty_run(struct dbuf *d, const tu_font_t *face, int points)
{
	return dup(d, "<w:r>") != 0 || docx_rpr(d, face, points) != 0 ||
	    dup(d, W_T_OPEN) != 0 || dup(d, W_T_CLOSE) != 0 ||
	    dup(d, "</w:r>") != 0;
}

/* Write the runs of one paragraph, whose text begins at p, and say whether any
 * of them was written under a level.  A level is a direction given to the text
 * it covers rather than a character the reader is shown, so it is taken out of
 * the text and the text is cut wherever the levels open over it change; a level
 * that is opened and closed with no text between leaves the text either side of
 * it one run.  Returns the number of bytes of text written, which is zero for a
 * paragraph that is nothing but levels. */
static size_t
docx_runs(struct dbuf *d, const char *p, size_t n, const tu_font_t *face,
    int points, struct docx_embed *e, int *under)
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
		else if (docx_same(e->stack, e->dep, e->prev, e->prev_dep))
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
		if (dup(d, "<w:r>") != 0 || docx_rpr(d, face, points) != 0 ||
		    docx_piece(d, e->buf + k, j - k) != 0 ||
		    dup(d, "</w:r>") != 0)
			return (size_t)-1;
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

/* The document member: the wordml body model, in docx markup, between the two
 * fixed members that open and close it.  A document of nothing, or of nothing
 * but directions, is a single bare paragraph, exactly as wordml writes it. */
static int
build_document(struct dbuf *d, const tu_doc_t *doc, const tu_style_t *st)
{
	tu_font_t face;
	struct docx_embed e;
	const char *p = doc->text != NULL ? doc->text : "";
	size_t len = doc->len;
	size_t i = 0, start = 0;
	int points, empty, failed;

	tu_font_face(st != NULL ? st->font : NULL, &face);
	points = st != NULL && st->fontsize > 0 ? st->fontsize : 12;

	if (dup(d, W_PREFIX) != 0)
		return -1;
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
		return -1;
	}

	if (empty) {
		if (dup(d, "<w:p></w:p>") != 0)
			return -1;
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
			/* A mark that opens the paragraph says which way it
			 * reads and is not text, so it goes into the
			 * paragraph's properties instead of into its run.
			 * Only the right-to-left one is recorded: the
			 * left-to-right mark names a direction the paragraph
			 * already has. */
			bm = tu_bidi_open((const unsigned char *)p + start,
			    i - start);
			head = start + (bm != 0 ? 3 : 0);

			if (dup(d, "<w:p><w:pPr>") != 0)
				return -1;
			if (bm == 0x200F && dup(d, "<w:bidi/>") != 0)
				return -1;
			if (dup(d, "</w:pPr>") != 0)
				return -1;
			written = docx_runs(d, p + head, i - head, &face,
			    points, &e, &under);
			if (written == (size_t)-1)
				return -1;
			/* An empty paragraph is one empty run, and a
			 * paragraph that ends with a level still open over its
			 * text closes it with an empty run after the text. */
			if (written == 0 && docx_empty_run(d, &face,
			    points) != 0)
				return -1;
			if (terminated && under && docx_empty_run(d, &face,
			    points) != 0)
				return -1;
			if (dup(d, "</w:p>") != 0)
				return -1;

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
	return dup(d, W_SUFFIX);
}

/* The core properties member: -title then -author, in the reference's order,
 * each written only when it was given. */
static int
build_core(struct dbuf *d, const tu_meta_t *m)
{
	if (dup(d, CORE_PREFIX) != 0)
		return -1;
	if (m && m->title) {
		if (dup(d, "<dc:title>") != 0 || esc(d, m->title,
		    strlen(m->title)) != 0 || dup(d, "</dc:title>") != 0)
			return -1;
	}
	if (m && m->author) {
		if (dup(d, "<dc:creator>") != 0 || esc(d, m->author,
		    strlen(m->author)) != 0 || dup(d, "</dc:creator>") != 0)
			return -1;
	}
	return dup(d, CORE_SUFFIX);
}

int
tu_write_docx(const tu_doc_t *d, const char *path, const tu_style_t *st,
    const tu_meta_t *meta)
{
	struct dbuf document = { NULL, 0, 0 };
	struct dbuf core = { NULL, 0, 0 };
	struct tu_zip_mem mem[8];
	int rc = -1;

	if (build_document(&document, d, st) != 0)
		goto out;
	if (build_core(&core, meta) != 0)
		goto out;

	mem[0].name = "[Content_Types].xml";
	mem[0].data = CT;
	mem[0].len = sizeof CT - 1;
	mem[1].name = "_rels/.rels";
	mem[1].data = RELS;
	mem[1].len = sizeof RELS - 1;
	mem[2].name = "word/_rels/document.xml.rels";
	mem[2].data = DRELS;
	mem[2].len = sizeof DRELS - 1;
	mem[3].name = "word/document.xml";
	mem[3].data = document.b;
	mem[3].len = document.len;
	mem[4].name = "word/theme/theme1.xml";
	mem[4].data = THEME1;
	mem[4].len = sizeof THEME1 - 1;
	mem[5].name = "docProps/core.xml";
	mem[5].data = core.b;
	mem[5].len = core.len;
	mem[6].name = "docProps/app.xml";
	mem[6].data = APP;
	mem[6].len = sizeof APP - 1;
	mem[7].name = "docProps/meta.xml";
	mem[7].data = META;
	mem[7].len = sizeof META - 1;
	for (size_t i = 0; i < 8; i++)
		mem[i].stored = 0;

	if (tu_zip_write(path, mem, 8) != 0)
		goto out;
	rc = 0;

out:
	free(core.b);
	free(document.b);
	if (rc != 0)
		tu_write_failed(path);
	return rc;
}