/*
 * textutil - convert between plain text and rich text file formats.
 *
 * Copyright (C) 2026, LibreDarwin
 * SPDX-License-Identifier: BSD-3-Clause
 */

#ifndef TEXTUTIL_H
#define TEXTUTIL_H

#include <limits.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <sys/stat.h>

/* The -help text, one entry per output line, NULL terminated.  The reference
 * tool writes these bytes to stdout and exits 0, both for -help and when no
 * command is given at all. */
extern const char *const tu_usage_lines[];

/* Every format name -convert and -cat accept.  Names are recognised whether
 * or not this port can write them, so that the option parser matches the
 * reference tool exactly; see src/textutil/NOTES.md for which are written. */
typedef enum {
	FMT_TXT = 0,
	FMT_RTF,
	FMT_RTFD,
	FMT_HTML,
	FMT_DOC,
	FMT_DOCX,
	FMT_ODT,
	FMT_WORDML,
	FMT_WEBARCHIVE,
	FMT_COUNT
} tu_fmt_t;

/* The output encodings -encoding can name.  Only the Unicode encodings are
 * offered, which are the ones that can represent every character this port
 * reads; the reference tool also accepts single byte encodings from a table
 * this port does not carry, and rejects them as inapplicable instead.  A
 * "UTF-16" or "UTF-32" mark means little endian with a leading mark, and the
 * byte order marked forms carry no mark. */
typedef enum {
	TU_ENC_UTF8 = 0,
	TU_ENC_UTF16,
	TU_ENC_UTF16LE,
	TU_ENC_UTF16BE,
	TU_ENC_UTF32,
	TU_ENC_UTF32LE,
	TU_ENC_UTF32BE,
	TU_ENC_UNSUPPORTED	/* a real encoding this port has no table for */
} tu_encoding_t;

/* Parse an -encoding argument, case insensitively, or return -1 when this port
 * cannot write it.  The reference tool also takes an NSStringEncoding number;
 * only 4, which is UTF-8, is honoured. */
int tu_encoding_parse(const char *s);

/* The canonical lower case name of an encoding, which is what the HTML writer
 * puts in its charset attribute. */
const char *tu_encoding_name(tu_encoding_t enc);

/* Re-encode UTF-8 bytes into enc.  On success stores a malloc'd buffer and its
 * length in *out and *outlen and returns 0; on failure returns -1 with *out
 * left NULL.  A leading mark is added for the two "UTF-16"/"UTF-32" forms. */
int tu_encode_bytes(const char *utf8, size_t len, tu_encoding_t enc,
    char **out, size_t *outlen);

/* The command, i.e. what the first non-option argument decides. */
typedef enum {
	CMD_HELP = 0,
	CMD_INFO,
	CMD_CONVERT,
	CMD_CAT
} tu_cmd_t;

/* A decoded document.  This port only ever holds plain text, which is all the
 * supported conversions need; the rich attributes are not modelled. */
typedef struct {
	char *text;			/* NUL terminated, UTF-8 */
	size_t len;			/* bytes, excluding the NUL */
	size_t nchars;			/* characters, as -info reports */
} tu_doc_t;

/* The run attributes -font and -fontsize choose.  A null name or a zero size
 * means the reference tool's default, Helvetica-Light at 12pt. */
typedef struct {
	const char *font;		/* requested font name, or NULL */
	int fontsize;			/* points, or 0 for the default */
} tu_style_t;

/* Everything a writer needs of the face a -font name resolves to.  family is
 * the RTF family keyword and postscript the RTF font name; wml is the family
 * wordml writes, which is a third string again: it is the family the face
 * belongs to, so "Arial Bold" is Arial and "Avenir" is Avenir Light.  See
 * src/textutil/font.c, which is where the sampled database lives. */
typedef struct {
	const char *family;
	const char *postscript;
	const char *wml;
	int italic;
	int bold;
} tu_font_t;

/* The document metadata the -keywords, -title, -author, -subject, -comment,
 * -editor, -company, -creationtime and -modificationtime options set.  A null
 * member was never given and is left out of the output entirely, which is
 * different from an empty string: -title "" writes a title group holding
 * nothing, and no -title at all writes no group.  The two times are the
 * broken-down form YYYY-MM-DDTHH:MM:SSZ, or NULL; see the note on them in
 * rtf.c.  keywords is the list already joined with ", "; see the note on it in
 * main.c. */
typedef struct {
	const char *keywords;
	const char *title;
	const char *author;
	const char *subject;
	const char *comment;
	const char *editor;
	const char *company;
	const char *creationtime;
	const char *modificationtime;
} tu_meta_t;

const char *tu_fmt_name(tu_fmt_t f);
tu_fmt_t tu_fmt_parse(const char *s);
const char *tu_fmt_extension(tu_fmt_t f);
const char *tu_fmt_type_string(tu_fmt_t f);

void tu_doc_free(tu_doc_t *d);

/* Why a path could not be read as a file.  The reference tool words each of
 * these differently, so the reason is carried out to the diagnostic rather
 * than collapsed into one message. */
enum {
	TU_READ_OK = 0,		/* the path can be read as a file */
	TU_READ_MISSING,	/* no such path */
	TU_READ_DENIED,		/* no permission to open it */
	TU_READ_UNOPENABLE,	/* it exists, but will not open as a file */
	TU_READ_WRONGFMT,	/* it exists, but is not the format asked for */
	TU_READ_ABSENTFORMAT	/* not a reason: this port has no such reader */
};

/* Read a plain text file, honouring a leading byte order mark.  Returns 0 on
 * success, otherwise one of the TU_READ_ reasons above. */
int tu_read_plain(const char *path, tu_doc_t *out);

/* Read a rich file into its text and the metadata -info reports.  A NULL out
 * is not filled in.  These two are the readers -info needs; the conversions
 * this port does start from plain text, so nothing else calls them yet.  See
 * src/textutil/NOTES.md. */
int tu_read_rtf(const void *buf, size_t len, tu_doc_t *out, tu_meta_t *meta);
int tu_read_rtfd(const char *path, tu_doc_t *out, tu_meta_t *meta);
int tu_read_html(const void *buf, size_t len, tu_doc_t *out, tu_meta_t *meta);

/* Free the strings a reader put in a tu_meta_t.  A member is left NULL rather
 * than freed twice, so this may be called on a tu_meta_t that was never filled
 * in. */
void tu_meta_free(tu_meta_t *m);

/* A reader the reference tool would use for a forced -format.  This port has
 * only the plain text reader, so the rest are grouped by what the reference
 * tool says, so that a file it would reject is rejected with the same words,
 * and a format it would happily read is reported as not implemented here
 * rather than blamed on the file. */
enum {
	TU_FMTREAD_TXT = 0,	/* reads anything, as plain text */
	TU_FMTREAD_RICH,	/* "the file couldn't be opened" */
	TU_FMTREAD_PACKAGE,	/* "isn't in the correct format" */
	TU_FMTREAD_ABSENT	/* this port has no reader for it at all */
};

/* Map a -format argument onto the reader the reference tool would use, or -1
 * for a name it does not accept.  The three groups differ only in the
 * diagnostic, so the forced-format path needs no reader of its own. */
int tu_fmtread_for(tu_fmt_t f);

/* The reader a file nothing was forced on is read with, chosen from its name
 * and its first bytes.  This only chooses.  A format that came from the name is
 * read or reported as an error, but one that came from the bytes is a guess,
 * and a guess whose read fails is read as plain text instead.  That is what
 * tu_fmt_read_falls_back says. */
tu_fmt_t tu_fmt_detect(const char *path, const struct stat *st,
    const char *head, size_t headlen);
int tu_fmt_read_falls_back(const char *path);

/* Check that a path names something this port can read as a file.  Returns 0
 * when it does, otherwise one of the TU_READ_ reasons.  A directory is
 * rejected here rather than at the read, because fopen() succeeds on one and
 * only the read then fails. */
int tu_check_file(const char *path);

/* Print the read diagnostic for a TU_READ_ reason. */
void tu_read_error(const char *path, int why);

/* Why a destination may not be written to, in the reference tool's wording. */
enum {
	TU_WRITE_OK = 0,	/* the destination can be written */
	TU_WRITE_NOFOLDER,	/* the folder holding it does not exist */
	TU_WRITE_DENIED,	/* that folder may not be written to */
	TU_WRITE_ISDIR		/* the destination is already a directory */
};

int tu_check_output(const char *path);
void tu_write_error(const char *path, int why);
void tu_write_failed(const char *path);
int tu_decode_plain(const void *buf, size_t rawlen, tu_doc_t *out);
size_t tu_utf8_seq(const unsigned char *p, size_t avail);

/* The next code point in UTF-8, and how many bytes it occupies, with a
 * malformed byte standing for U+FFFD rather than for itself.  See the note on
 * it in txt.c for how much of a bad sequence it swallows. */
unsigned long tu_utf8_strict(const unsigned char *p, size_t avail, size_t *width);

/* The direction of a line of text, for the writers that record which way it
 * reads: 1 when the first character that has a direction of its own is written
 * right to left, 0 otherwise, since a line with no such character reads left to
 * right.  The number of bytes to drop from the head of a line, which is a mark
 * that only tells a reader which way to read rather than text the reader is to
 * be shown.  See src/textutil/bidi.c. */
unsigned long tu_bidi_first(const unsigned char *p, size_t n);
size_t tu_bidi_mark(const unsigned char *p, size_t n);

/* Writers.  Each returns 0 on success and -1 on failure, having already
 * reported the reason to stderr.  -encoding applies to the text and HTML
 * writers, which hold their whole output in memory and so encode it just
 * before writing; RTF and RTFD output is 7-bit by construction and the
 * reference tool leaves it in ASCII. */
int tu_write_txt(const tu_doc_t *d, const char *path, tu_encoding_t enc);
int tu_write_rtf(const tu_doc_t *d, const char *path,
    const tu_style_t *st, const tu_meta_t *meta);
int tu_write_rtfd(const tu_doc_t *d, const char *path,
    const tu_style_t *st, const tu_meta_t *meta);
int tu_write_html(const tu_doc_t *d, const char *path,
    const tu_style_t *st, const tu_meta_t *meta, tu_encoding_t enc);
int tu_write_wordml(const tu_doc_t *d, const char *path,
    const tu_style_t *st, const tu_meta_t *meta);

/* The -font database, shared by the writers that resolve one.  See
 * src/textutil/font.c. */
void tu_font_face(const char *want, tu_font_t *f);

/* -info.  forced_fmt is the -format argument, or -1 when none was given. */
int tu_info_file(const char *path, int forced_fmt);
void tu_info_doc(const char *label, const tu_doc_t *doc);

/* A time, taken apart.  A timestamp is normalised through the epoch before it
 * is stored, so a month of 13 is not out of range by the time it gets here: it
 * is the February that follows.  unix is seconds since 1970, which is what the
 * RTF \timesinceref field is counted from. */
struct tu_time {
	long year;
	int month;
	int day;
	int hour;
	int minute;
	int second;
	long unix;
};

/* The -creationtime and -modificationtime argument.  The reference tool wants
 * YYYY-MM-DDTHH:MM:SSZ and checks only the first twenty characters, so
 * anything may follow them and nothing is checked for range.  Returns 1 and
 * fills the fields when the twenty are as asked for. */
int tu_parse_timestamp(const char *s, struct tu_time *t);

#endif /* TEXTUTIL_H */
