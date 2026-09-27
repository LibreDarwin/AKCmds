/*
 * textutil - -info.
 *
 * Five fields per file: the name as given on the command line, the detected
 * type, the size, the length in characters, and a preview of the contents.
 *
 * The size of a bundle is its directory's own st_size, not the total of its
 * members.  That is the filesystem's number, not a property of the contents,
 * so two bundles holding identical text can report different sizes.
 *
 * The preview is the first line, cut to 30 UTF-16 code units.  The ellipsis is
 * not tied to that cut alone: it also appears when anything at all follows the
 * first line's newline.  A file of exactly "a\n" is therefore previewed as
 * "a", while "a\n\n" is previewed as "a...".  The cut is counted in code
 * units, not bytes, so multibyte text is not cut short; a character that would
 * not fit whole is left out rather than halved.
 *
 * A file that cannot be read is reported on stderr, and does not by itself
 * make textutil fail.
 *
 * Copyright (C) 2026, LibreDarwin
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include <dirent.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>

#include "textutil.h"

/* The preview cut, in UTF-16 code units. */
#define PREVIEW_UNITS 30

/* Decide the type of a file for -info.  A name the tool can read decides it,
 * and beats what the bytes look like; otherwise the first few bytes do.  The
 * formats with no reader here are not named, so a file called .doc is read as
 * what it holds. */
static tu_fmt_t
detect(const char *path, const struct stat *st, const char *head, size_t headlen)
{
	const char *dot = strrchr(path, '.');

	/* A directory is an RTFD bundle because of what it is called.  What it
	 * holds decides whether it can be read, not what it is. */
	if (S_ISDIR(st->st_mode))
		return dot != NULL && strcasecmp(dot, ".rtfd") == 0 ?
		    FMT_RTFD : FMT_TXT;
	if (dot != NULL) {
		switch (tu_fmt_parse(dot + 1)) {
		case FMT_TXT:
		case FMT_RTF:
		case FMT_HTML:
		case FMT_WEBARCHIVE:
			return (tu_fmt_t)tu_fmt_parse(dot + 1);
		default:
			break;
		}
	}
	if (headlen >= 5 && memcmp(head, "{\\rtf", 5) == 0)
		return FMT_RTF;
	if (headlen >= 5 && (memcmp(head, "<html", 5) == 0 ||
	    memcmp(head, "<HTML", 5) == 0))
		return FMT_HTML;
	if (headlen >= 9 && memcmp(head, "<!DOCTYPE", 9) == 0)
		return FMT_HTML;
	return FMT_TXT;
}

/* The Size and Length fields.  has_size is 0 for stdin, which has no file to
 * measure.  These two come before the metadata and the Contents preview, which
 * is the order the reference tool prints them in. */
static void
info_print_size(const tu_doc_t *doc, int has_size, long long size)
{
	/* Both count nouns are singular at one. */
	if (has_size)
		printf("  Size:  %lld byte%s\n", size, size == 1 ? "" : "s");
	printf("  Length:  %zu character%s\n", doc->nchars,
	    doc->nchars == 1 ? "" : "s");
}

/* The Contents field, shared by -info on a file and on stdin. */
static void
info_print_body(const tu_doc_t *doc)
{
	const char *p, *nl, *end;
	size_t linelen, i, shown = 0;
	int truncated = 0, more = 0, split = 0, sep = 0;

	/* An empty document has nothing to preview, and the field is dropped
	 * rather than shown blank. */
	if (doc->nchars == 0)
		return;
	fputs("  Contents:  ", stdout);

	p = doc->text != NULL ? doc->text : "";
	end = p + doc->len;
	nl = p;
	/* A line ends at a newline or at a separator.  A separator is not a
	 * newline to a terminal, so the preview stops at one rather than
	 * printing it, which is the same as for a newline. */
	while (nl < end && *nl != '\0' && *nl != '\n' && *nl != '\r' &&
	    !(nl + 2 < end && (unsigned char)nl[0] == 0xe2 &&
	    (unsigned char)nl[1] == 0x80 && ((unsigned char)nl[2] == 0xa8 ||
	    (unsigned char)nl[2] == 0xa9)))
		nl++;
	if (nl < end && *nl == '\0')
		linelen = (size_t)(nl - p);
	else if (nl < end && (*nl == '\n' || *nl == '\r'))
		linelen = (size_t)(nl - p);
	else {
		linelen = (size_t)(nl - p);
		sep = 1;
	}
	/* The cut is 30 UTF-16 code units, the same unit the Length field above
	 * counts, so a supplementary character spends two of the 30 and a BMP
	 * character spends one.  A character is only shown if it fits whole. */
	for (i = 0; p + i < nl;) {
		size_t avail = linelen - i;
		size_t seq = tu_utf8_seq((const unsigned char *)p + i, avail);
		size_t units = seq == 4 ? 2 : 1;

		if (shown + units > PREVIEW_UNITS) {
			truncated = 1;
			/* Landing between the halves of a surrogate pair leaves
			 * no character at all to show, and the reference tool
			 * renders that whole preview as the literal text
			 * (null) rather than the part that did fit. */
			split = units == 2 && shown == PREVIEW_UNITS - 1;
			break;
		}
		shown += units;
		i += seq;
	}
	/* Anything past the terminator means there is more to show.  After a
	 * separator, though, a further separator is nothing at all: a
	 * paragraph that ends the text leaves the preview as it stood. */
	if (nl < end && *nl != '\0') {
		const char *r = nl + (sep ? 3 : 1);

		if (sep) {
			more = 0;
			while (r < end) {
				if (*r == '\n' || *r == '\r' ||
				    (r + 2 < end && (unsigned char)r[0] == 0xe2 &&
				    (unsigned char)r[1] == 0x80 &&
				    ((unsigned char)r[2] == 0xa8 ||
				    (unsigned char)r[2] == 0xa9))) {
					r += (*r == '\n' || *r == '\r') ?
					    1 : 3;
					continue;
				}
				more = 1;
				break;
			}
		} else
			more = (size_t)(r - p) < doc->len;
	}
	if (truncated || more) {
		if (split) {
			fputs("(null)", stdout);
		} else {
			/* Whole characters, so multibyte text previews as
			 * itself rather than as a trail of lead bytes. */
			shown = 0;
			for (i = 0; p + i < nl;) {
				size_t avail = linelen - i;
				size_t seq = tu_utf8_seq(
				    (const unsigned char *)p + i, avail);
				size_t units = seq == 4 ? 2 : 1;

				if (shown + units > PREVIEW_UNITS)
					break;
				fwrite(p + i, 1, seq, stdout);
				shown += units;
				i += seq;
			}
		}
		fputs("...", stdout);
	} else {
		/* Nothing was left out, so the line is shown as it stands,
		 * keeping the newline that ended it.  That is why a file of
		 * exactly "a\n" previews as "a" on a line of its own. */
		size_t n = linelen;

		/* A newline that ended the line is part of it; a separator is
		 * not, and is left out. */
		if (nl < end && *nl != '\0' && !sep)
			n++;
		fwrite(p, 1, n, stdout);
	}
	putchar('\n');
}

/* The metadata lines, in the order the reference tool prints them.  A value
 * that is there but empty is printed with nothing after the colon, and a value
 * that is not there is not printed at all, which is the same distinction the
 * RTF writer makes between an empty group and no group.  HTML has no line for
 * the title: the reference tool does not read the title element back, and
 * neither does this. */
static void info_print_time(const char *label, const char *v);

static void
info_print_meta(const tu_meta_t *m, int with_title)
{
	if (m == NULL)
		return;
	if (with_title && m->title != NULL)
		printf("  Title:  %s\n", m->title);
	if (m->author != NULL)
		printf("  Author:  %s\n", m->author);
	if (m->editor != NULL)
		printf("  Last Editor:  %s\n", m->editor);
	if (m->company != NULL)
		printf("  Company:  %s\n", m->company);
	if (m->subject != NULL)
		printf("  Subject:  %s\n", m->subject);
	if (m->keywords != NULL)
		printf("  Keywords:  %s\n", m->keywords);
	if (m->comment != NULL)
		printf("  Comment:  %s\n", m->comment);
	info_print_time("Created", m->creationtime);
	info_print_time("Last Modified", m->modificationtime);
}

/* The two times are held in the broken-down form the option parser uses, and
 * -info wants them with a space and a zone instead.  The zone is always +0000:
 * a time read out of a file carries no offset, and the reference tool reads it
 * as though it were one. */
static void
info_print_time(const char *label, const char *v)
{
	char when[32];

	if (v == NULL)
		return;
	if (strlen(v) >= 20)
		snprintf(when, sizeof(when), "%.19s", v);
	else
		return;
	when[10] = ' ';
	when[19] = '\0';
	printf("  %s:  %s +0000\n", label, when);
}

int
tu_info_file(const char *path, int forced)
{
	struct stat st;
	FILE *fp;
	char head[1024];
	size_t headlen = 0;

	memset(head, 0, sizeof(head));
	tu_doc_t doc;
	tu_fmt_t fmt;
	tu_meta_t meta;

	if (stat(path, &st) != 0) {
		tu_read_error(path, errno == EACCES || errno == EPERM ?
		    TU_READ_DENIED : TU_READ_MISSING);
		return 0;
	}
	/* A forced -format decides the type instead of the contents, and is
	 * reached before the head is read, since there is nothing here to sniff.
	 * Only plain text is read, so a forced reader this port does not have
	 * is refused here, in the reference tool's own words. */
	if (forced >= 0) {
		int group = tu_fmtread_for((tu_fmt_t)forced);

		if (group != TU_FMTREAD_TXT) {
			if (group == TU_FMTREAD_ABSENT)
				fprintf(stderr, "textutil: reading %s input "
				    "is not implemented\n",
				    tu_fmt_name((tu_fmt_t)forced));
			else
				tu_read_error(path, group == TU_FMTREAD_RICH ?
				    TU_READ_UNOPENABLE : TU_READ_WRONGFMT);
			return 0;
		}
		fmt = FMT_TXT;
	} else if (S_ISREG(st.st_mode) && (fp = fopen(path, "rb")) != NULL) {
		headlen = fread(head, 1, sizeof(head), fp);
		fclose(fp);
		fmt = detect(path, &st, head, headlen);
	} else {
		fmt = detect(path, &st, head, 0);
	}

	/* Plain text is read as plain text.  RTF and RTFD are read for their
	 * text and their metadata, which is all -info reports; HTML is read
	 * the same way.  A format this port has no reader for is read as
	 * nothing, so the length is zero and no metadata is printed. */
	memset(&meta, 0, sizeof(meta));
	doc.text = NULL;
	doc.len = 0;
	doc.nchars = 0;
	if (fmt == FMT_WEBARCHIVE) {
		/* A web archive is a binary property list, not text.  This
		 * port has no property list reader, so it says so rather than
		 * counting the bytes of a container as if they were a
		 * document.  Recorded in src/textutil/NOTES.md. */
		fprintf(stderr, "textutil: reading %s input is not "
		    "implemented\n", tu_fmt_name(FMT_WEBARCHIVE));
		return 0;
	}
	if (fmt == FMT_TXT) {
		/* A directory that is not a bundle is not a text file
		 * either, and the reference tool will not open it.  A web
		 * archive is named but not unpacked, so its bytes are
		 * counted as they lie. */
		int why = S_ISDIR(st.st_mode) ? TU_READ_UNOPENABLE :
		    tu_read_plain(path, &doc);

		if (why != 0) {
			tu_read_error(path, why);
			return 0;
		}
	} else if (fmt == FMT_RTF || fmt == FMT_HTML) {
		char *buf;
		size_t len;
		int why;
		FILE *in = fopen(path, "rb");

		if (in == NULL) {
			tu_read_error(path, TU_READ_UNOPENABLE);
			return 0;
		}
		if (fseek(in, 0, SEEK_END) != 0) {
			fclose(in);
			tu_read_error(path, TU_READ_UNOPENABLE);
			return 0;
		}
		len = (size_t)ftell(in);
		rewind(in);
		buf = len != 0 ? malloc(len) : calloc(1, 1);
		if (buf == NULL || (len != 0 && fread(buf, 1, len, in) != len)) {
			free(buf);
			fclose(in);
			tu_read_error(path, TU_READ_UNOPENABLE);
			return 0;
		}
		fclose(in);
		why = fmt == FMT_RTF ? tu_read_rtf(buf, len, &doc, &meta) :
		    tu_read_html(buf, len, &doc, &meta);
		free(buf);
		if (why != 0) {
			tu_read_error(path, why);
			return 0;
		}
	} else if (fmt == FMT_RTFD) {
		int why = tu_read_rtfd(path, &doc, &meta);

		if (why != 0) {
			tu_read_error(path, why);
			return 0;
		}
	}

	printf("File:  %s\n", path);
	printf("  Type:  %s\n", tu_fmt_type_string(fmt));
	info_print_size(&doc, 1, (long long)st.st_size);
	info_print_meta(&meta, fmt != FMT_HTML);
	info_print_body(&doc);
	tu_doc_free(&doc);
	tu_meta_free(&meta);
	return 0;
}

/* -info -stdin names the input "stdin" and omits Size, because there is no
 * file behind it to measure. */
void
tu_info_doc(const char *label, const tu_doc_t *doc)
{
	printf("File:  %s\n", label);
	printf("  Type:  %s\n", tu_fmt_type_string(FMT_TXT));
	info_print_size(doc, 0, 0);
	info_print_body(doc);
}
