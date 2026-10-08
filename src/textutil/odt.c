#include "textutil.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "odt_static.h"
#include "zip.h"

/* The odt writer.  Every member is a fixed byte string from odt_static.h but
 * the two that hold the document: the content XML and the metadata (the
 * -title and -author groups, which the reference puts in the same order).
 * The content is built whole in a byte buffer, the paragraph model is the
 * wordml model -- the same paragraph splitting over carriage returns, line
 * feeds and U+2029, the same opening mark dropped from the head of a
 * paragraph, the same tail of nothing but directions not written -- and then
 * each paragraph is text again: no run properties, no direction, just the
 * lines and the inline elements the reference tool writes.  The font-face
 * declaration and the P1 paragraph style are written when the document has
 * any paragraph at all; the empty document leaves both out.  The mimetype
 * member is stored whole, as the format demands. */

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

/* Text escaped for the two dynamic members.  Only the three characters that
 * mark up the document are written long, as the reference does in the text it
 * writes. */
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

/* n spaces as the text:s element, which names its count only when it is not
 * the one it means. */
static int
enc_sp(struct dbuf *d, size_t n)
{
	char buf[64];

	if (n <= 1)
		return dup(d, "<text:s/>");
	snprintf(buf, sizeof buf, "<text:s text:c=\"%zu\"/>", n);
	return dup(d, buf);
}

/* One paragraph's text, which is the paragraph with what is not text to the
 * reference taken out and the rest written as the inline elements: an
 * embedding control anywhere in the paragraph is a direction given to the
 * text it covers rather than a character the reader is shown, so it is gone;
 * a control other than a tab is gone the same way and keeps nothing of the
 * spaces either side of it, which is why the space run is counted across
 * what is dropped; a tab is text:tab and a line separator text:line-break,
 * and each of those opens the next run of spaces whole; every other byte is
 * text, written as it is except for the three that mark up the document.  A
 * paragraph of nothing but what is dropped is an empty text:p. */
static int
enc_body(struct dbuf *d, const char *p, size_t n)
{
	size_t i = 0;
	int at_start = 1;		/* whether the next spaces are a run */

	while (i < n) {
		unsigned char c = (unsigned char)p[i];

		if (tu_bidi_embed((const unsigned char *)p + i, n - i) != 0) {
			i += 3;
			continue;
		}
		if (c < 0x20 && c != '\t') {
			i++;
			continue;
		}
		if (c == '\t') {
			if (dup(d, "<text:tab/>") != 0)
				return -1;
			i++;
			at_start = 1;
			continue;
		}
		if (c == 0xE2 && n - i >= 3 &&
		    (unsigned char)p[i + 1] == 0x80 &&
		    (unsigned char)p[i + 2] == 0xA8) {
			if (dup(d, "<text:line-break/>") != 0)
				return -1;
			i += 3;
			at_start = 1;
			continue;
		}
		if (c == ' ') {
			size_t cnt = 0;

			while (i < n) {
				unsigned char s = (unsigned char)p[i];

				if (s == ' ') {
					cnt++;
					i++;
				} else if (s < 0x20 && s != '\t') {
					i++;
				} else if (tu_bidi_embed(
				    (const unsigned char *)p + i,
				    n - i) != 0) {
					i += 3;
				} else
					break;
			}
			if (at_start) {
				if (enc_sp(d, cnt) != 0)
					return -1;
			} else {
				if (dsz(d, " ", 1) != 0)
					return -1;
				if (cnt > 1 && enc_sp(d, cnt - 1) != 0)
					return -1;
			}
			at_start = 0;
			continue;
		}
		{
			size_t j = i;

			while (j < n) {
				unsigned char t = (unsigned char)p[j];

				if (t == '\t' || t == ' ' ||
				    (t < 0x20) ||
				    (t == 0xE2 && n - j >= 3 &&
				    (unsigned char)p[j + 1] == 0x80 &&
				    (unsigned char)p[j + 2] == 0xA8) ||
				    tu_bidi_embed((const unsigned char *)p + j,
				    n - j) != 0)
					break;
				j++;
			}
			if (esc(d, p + i, j - i) != 0)
				return -1;
			i = j;
			at_start = 0;
		}
	}
	return 0;
}

/* The width of the paragraph mark at i, which is a carriage return, a line
 * feed, or the two of them in that order and worth one mark rather than two.
 * A level or a mark between the two is not shown to a reader and does not
 * come between them here either.  A mark is the exception, and only at the
 * head. */
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

/* Whether the three bytes at i are U+2029, the paragraph separator, which
 * ends a paragraph the way a carriage return does.  Read as a whole so that a
 * truncated sequence at the end of the buffer is not a match. */
static int
wml_is_para_sep(const char *p, size_t n, size_t i)
{
	return n - i >= 3 && (unsigned char)p[i] == 0xE2 &&
	    (unsigned char)p[i + 1] == 0x80 && (unsigned char)p[i + 2] == 0xA9;
}

/* The content member: the font-face declaration, the automatic styles, and
 * then, inside office:text, one paragraph per line of the wordml model.  The
 * P1 style is written when the document has a paragraph to use it; a
 * document of nothing, or of nothing but directions, holds neither that nor
 * a paragraph of its own, and its office:text is empty. */
static int
build_content(struct dbuf *d, const tu_doc_t *doc)
{
	const char *p = doc->text != NULL ? doc->text : "";
	size_t len = doc->len;
	size_t i = 0, start = 0;
	int empty;

	empty = !tu_bidi_para((const unsigned char *)p, len);
	if (dup(d, O_HEAD) != 0)
		return -1;
	if (!empty && dup(d, O_FONTFACE) != 0)
		return -1;
	if (dup(d, O_ENDFONT) != 0 || dup(d, O_AUTO) != 0)
		return -1;
	if (!empty && dup(d, O_P1) != 0)
		return -1;
	if (dup(d, O_TC) != 0 || dup(d, O_ENDSTYLE) != 0 || dup(d, O_MID) != 0)
		return -1;
	if (!empty) {
		for (;;) {
			size_t mark = i < len ?
			    wml_mark_width(p, len, i) : 0;
			int sep = i < len && wml_is_para_sep(p, len, i);
			unsigned long bm;
			size_t head;

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
			 * reads and is not text, so it is dropped: the odt
			 * format records no direction of its own. */
			bm = tu_bidi_open((const unsigned char *)p + start,
			    i - start);
			head = start + (bm != 0 ? 3 : 0);

			if (dup(d, "<text:p text:style-name=\"P1\">") != 0)
				return -1;
			if (enc_body(d, p + head, i - head) != 0)
				return -1;
			if (dup(d, "</text:p>") != 0)
				return -1;

			if (i == len)
				break;
			start = i + (mark != 0 ? mark : 3);
			i = start;
		}
	}
	return dup(d, O_TAIL);
}

/* The metadata member: -title then -author, in the reference's order, each
 * written only when it was given. */
static int
build_meta(struct dbuf *d, const tu_meta_t *m)
{
	if (dup(d, OMETA_HEAD) != 0)
		return -1;
	if (m && m->title) {
		if (dup(d, "<dc:title>") != 0 || esc(d, m->title,
		    strlen(m->title)) != 0 || dup(d, "</dc:title>") != 0)
			return -1;
	}
	if (m && m->author) {
		if (dup(d, "<meta:initial-creator>") != 0 || esc(d, m->author,
		    strlen(m->author)) != 0 || dup(d,
		    "</meta:initial-creator>") != 0)
			return -1;
	}
	return dup(d, OMETA_TAIL);
}

int
tu_write_odt(const tu_doc_t *d, const char *path, const tu_style_t *st,
    const tu_meta_t *meta)
{
	struct dbuf content = { NULL, 0, 0 };
	struct dbuf md = { NULL, 0, 0 };
	struct tu_zip_mem mem[5];
	int rc = -1;

	(void)st;			/* the odt writer takes no run options */

	if (build_content(&content, d) != 0)
		goto out;
	if (build_meta(&md, meta) != 0)
		goto out;

	mem[0].name = "mimetype";
	mem[0].data = MIMETYPE;
	mem[0].len = sizeof MIMETYPE - 1;
	mem[0].stored = 1;
	mem[1].name = "content.xml";
	mem[1].data = content.b;
	mem[1].len = content.len;
	mem[1].stored = 0;
	mem[2].name = "styles.xml";
	mem[2].data = STYLES;
	mem[2].len = sizeof STYLES - 1;
	mem[2].stored = 0;
	mem[3].name = "meta.xml";
	mem[3].data = md.b;
	mem[3].len = md.len;
	mem[3].stored = 0;
	mem[4].name = "META-INF/manifest.xml";
	mem[4].data = MANIFEST;
	mem[4].len = sizeof MANIFEST - 1;
	mem[4].stored = 0;

	if (tu_zip_write(path, mem, 5) != 0)
		goto out;
	rc = 0;

out:
	free(md.b);
	free(content.b);
	if (rc != 0)
		tu_write_failed(path);
	return rc;
}