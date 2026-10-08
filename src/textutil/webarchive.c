/* Web archive writer.
 *
 * A web archive is the format the reference tool writes for -convert
 * webarchive: a binary property list that holds the document as the other
 * root of an HTML page.  The HTML is what the -convert html writer produces
 * for the same document, taken whole as a single data blob, and with it go
 * the one URL the archive names and the MIME type and encoding name that say
 * how to show it.  A web archive names no other resources, so its property
 * list is always the same thirteen objects in the same order, and only the
 * encoding name and the HTML data vary between documents.
 *
 * The binary property list, as Apple serialises it here, is read like this.
 * An object is one of the fixed markers below, sized so that it can be
 * skipped from its own head: a string or a blob of data carries its own
 * length, and a container carries its item count but the items themselves
 * are only references, each an object number taking one byte for the few
 * objects an archive holds.  After the objects comes an offset table, one
 * entry per object in order, each entry the object's absolute position in
 * the file in as few bytes as the greatest of them takes, and then a fixed
 * trailing header of eight fields, each a separate big-endian whole: the two
 * byte sizes in one byte apiece, the object count, the root object's number,
 * and where the offset table begins -- those four in as many bytes as the
 * field is wide.  Positions need the number of bytes that fits the largest,
 * which here is two for an ordinary document and four past 64K.
 *
 * The numbers the objects are read by are their order in the file, and the
 * archive is serialised depth first: the root dictionary, then the value of
 * each key it pairs (a dictionary writes its keys first and its values
 * after).  The layout below follows that order, and the few sizes the offset
 * table needs are noted as each object is laid down. */

#include "textutil.h"

#include <stdlib.h>
#include <string.h>

/* A plain ASCII string object, whose length is folded into its marker when
 * it fits and otherwise runs on ahead of the text in the one-byte form the
 * reference tool writes. */
static void
bplist_ascii(char *out, size_t *pos, const char *s, size_t len)
{
	if (len < 0xF) {
		out[(*pos)++] = (char)(0x50 | len);
	} else {
		out[(*pos)++] = 0x5F;
		out[(*pos)++] = 0x10;
		out[(*pos)++] = (char)len;
	}
	memcpy(out + *pos, s, len);
	*pos += len;
}

/* A blob of data, in the same two forms.  The length head is an integer
 * object, which takes as many bytes as the length needs: one for a length
 * under 256, two for one under 64K and four beyond. */
static void
bplist_data(char *out, size_t *pos, const char *s, size_t len)
{
	if (len < 0xF) {
		out[(*pos)++] = (char)(0x40 | len);
	} else if (len < 0x100) {
		out[(*pos)++] = 0x4F;
		out[(*pos)++] = 0x10;
		out[(*pos)++] = (char)len;
	} else if (len < 0x10000) {
		out[(*pos)++] = 0x4F;
		out[(*pos)++] = 0x11;
		out[(*pos)++] = (char)(len >> 8);
		out[(*pos)++] = (char)len;
	} else {
		out[(*pos)++] = 0x4F;
		out[(*pos)++] = 0x12;
		out[(*pos)++] = (char)(len >> 24);
		out[(*pos)++] = (char)(len >> 16);
		out[(*pos)++] = (char)(len >> 8);
		out[(*pos)++] = (char)len;
	}
	memcpy(out + *pos, s, len);
	*pos += len;
}

static void
w64(char *p, size_t off, unsigned long v)
{
	size_t k;

	for (k = 0; k < 8; k++)
		p[off + k] = (char)(v >> (56 - 8 * k));
}

int
tu_write_webarchive(const tu_doc_t *d, const char *path, const tu_style_t *st,
    const tu_meta_t *meta, tu_encoding_t enc)
{
	const char *name = tu_encoding_name(enc);
	size_t nname = strlen(name);
	char *html = NULL;
	size_t hlen = 0;
	static const char url[] = "file:///index.html";
	static const char mime[] = "text/html";
	/* The object each string goes to by its number, filled in as it is
	 * written. */
	size_t offs[13];
	char *out;
	size_t pos = 8;
	size_t table, ois, i;
	FILE *fp;

	if (tu_html_build(d, st, meta, enc, &html, &hlen) != 0) {
		tu_write_failed(path);
		return -1;
	}
	if (SIZE_MAX - 8 < hlen + 1024) {
		free(html);
		tu_write_failed(path);
		return -1;
	}
	if ((out = malloc(8 + 1024 + hlen)) == NULL) {
		free(html);
		tu_write_failed(path);
		return -1;
	}
	memcpy(out, "bplist00", 8);

	/* The layout the offsets are taken from.  The root dictionary pairs one
	 * string, named by references 1 and 2 -- the string object, then the
	 * dictionary that is its value.  That dictionary's fields are the five
	 * keys 3 through 7 and the five values 8 through 12, laid down as its
	 * keys first. */
	offs[0] = pos;
	out[pos++] = 0xD1;
	out[pos++] = 0x01;
	out[pos++] = 0x02;

	offs[1] = pos;
	bplist_ascii(out, &pos, "WebMainResource",
	    sizeof("WebMainResource") - 1);

	offs[2] = pos;
	out[pos++] = 0xD5;
	for (i = 0; i < 5; i++)
		out[pos++] = (char)(3 + i);
	for (i = 0; i < 5; i++)
		out[pos++] = (char)(8 + i);

	offs[3] = pos;
	bplist_ascii(out, &pos, "WebResourceURL",
	    sizeof("WebResourceURL") - 1);
	offs[4] = pos;
	bplist_ascii(out, &pos, "WebResourceTextEncodingName",
	    sizeof("WebResourceTextEncodingName") - 1);
	offs[5] = pos;
	bplist_ascii(out, &pos, "WebResourceData",
	    sizeof("WebResourceData") - 1);
	offs[6] = pos;
	bplist_ascii(out, &pos, "WebResourceMIMEType",
	    sizeof("WebResourceMIMEType") - 1);
	offs[7] = pos;
	bplist_ascii(out, &pos, "WebResourceFrameName",
	    sizeof("WebResourceFrameName") - 1);

	offs[8] = pos;
	bplist_ascii(out, &pos, url, sizeof(url) - 1);
	offs[9] = pos;
	bplist_ascii(out, &pos, name, nname);
	offs[10] = pos;
	bplist_data(out, &pos, html, hlen);
	free(html);
	offs[11] = pos;
	bplist_ascii(out, &pos, mime, sizeof(mime) - 1);
	offs[12] = pos;
	bplist_ascii(out, &pos, "", 0);

	/* The offset table takes its size from the greatest position above, and
	 * then the trailing header follows it, the two sizes in one byte each
	 * and the three eight-byte counts in one field each. */
	table = pos;
	ois = pos - 1 <= 0xFF ? 1 : (pos - 1 <= 0xFFFF ? 2 : 4);
	if (SIZE_MAX - table < 13 * ois + 32) {
		free(out);
		tu_write_failed(path);
		return -1;
	}
	if ((out = realloc(out, table + 13 * ois + 32)) == NULL) {
		tu_write_failed(path);
		return -1;
	}
	for (i = 0; i < 13; i++) {
		char *p = out + table + i * ois;

		if (ois == 1)
			p[0] = (char)offs[i];
		else if (ois == 2) {
			p[0] = (char)(offs[i] >> 8);
			p[1] = (char)offs[i];
		} else {
			p[0] = (char)(offs[i] >> 24);
			p[1] = (char)(offs[i] >> 16);
			p[2] = (char)(offs[i] >> 8);
			p[3] = (char)offs[i];
		}
	}
	pos = table + 13 * ois;
	memset(out + pos, 0, 6);
	out[pos + 6] = (char)ois;
	out[pos + 7] = 1;
	/* 13 objects, root 0, then the offset table's own position. */
	w64(out, pos + 8, 13);
	w64(out, pos + 16, 0);
	w64(out, pos + 24, table);
	pos += 32;

	if ((fp = fopen(path, "wb")) == NULL) {
		free(out);
		tu_write_failed(path);
		return -1;
	}
	if (fwrite(out, 1, pos, fp) != pos) {
		fclose(fp);
		free(out);
		tu_write_failed(path);
		return -1;
	}
	if (fclose(fp) != 0) {
		free(out);
		tu_write_failed(path);
		return -1;
	}
	free(out);
	return 0;
}