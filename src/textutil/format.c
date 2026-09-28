/*
 * textutil - the format table.
 *
 * The names, extensions and -info type strings are all spelled the way the
 * reference tool spells them, including the two that do not follow the
 * obvious pattern: webarchive's type string is lower case, and html's is
 * simply "HTML".
 *
 * Copyright (C) 2026, LibreDarwin
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include <ctype.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>

#include "textutil.h"

static const struct {
	const char *name;
	const char *ext;
	const char *type;
} formats[FMT_COUNT] = {
	[FMT_TXT]       = { "txt",       "txt",       "plain text" },
	[FMT_RTF]       = { "rtf",       "rtf",       "rich text format (RTF)" },
	[FMT_RTFD]      = { "rtfd",      "rtfd",      "rich text with graphics format (RTFD)" },
	[FMT_HTML]      = { "html",      "html",      "HTML" },
	[FMT_DOC]       = { "doc",       "doc",       "Word format" },
	[FMT_DOCX]      = { "docx",      "docx",      "Office Open XML format" },
	[FMT_ODT]       = { "odt",       "odt",       "Open Document format" },
	/* wordml is the one format whose name is not the extension an output
	 * file of it is given: -convert wordml writes in.xml. */
	[FMT_WORDML]    = { "wordml",    "xml",       "Word XML format" },
	[FMT_WEBARCHIVE]= { "webarchive","webarchive","web archive" },
};

const char *
tu_fmt_name(tu_fmt_t f)
{
	if ((int)f < 0 || f >= FMT_COUNT || formats[f].name == NULL)
		return "txt";
	return formats[f].name;
}

const char *
tu_fmt_extension(tu_fmt_t f)
{
	if ((int)f < 0 || f >= FMT_COUNT || formats[f].ext == NULL)
		return "txt";
	return formats[f].ext;
}

const char *
tu_fmt_type_string(tu_fmt_t f)
{
	if ((int)f < 0 || f >= FMT_COUNT || formats[f].type == NULL)
		return "plain text";
	return formats[f].type;
}

/* Names are matched without regard to case, as the reference tool does, and
 * the comparison is over the whole string, so a trailing space or a UTI such
 * as public.rtf is not a name. */
tu_fmt_t
tu_fmt_parse(const char *s)
{
	if (s == NULL)
		return (tu_fmt_t)-1;
	for (int i = 0; i < FMT_COUNT; i++) {
		const char *a = s, *b = formats[i].name;

		if (b == NULL)
			continue;
		for (;;) {
			unsigned char ca = (unsigned char)*a;
			unsigned char cb = (unsigned char)*b;

			if (ca >= 'A' && ca <= 'Z')
				ca = (unsigned char)(ca - 'A' + 'a');
			if (cb >= 'A' && cb <= 'Z')
				cb = (unsigned char)(cb - 'A' + 'a');
			if (ca != cb)
				break;
			if (ca == '\0')
				return (tu_fmt_t)i;
			a++;
			b++;
		}
	}
	return (tu_fmt_t)-1;
}

/* Which reader the reference tool would use for a forced -format, and so which
 * of its two "this is not that kind of file" wordings appears.  Plain text
 * reads anything and RTF and RTFD report that the file could not be opened;
 * the office containers report that it is not in the correct format.  HTML and
 * web archive are ones the reference tool reads, and this port has no reader
 * for, so they are named apart: reporting a file that is perfectly good as
 * unreadable would be worse than saying the format is missing. */
int
tu_fmtread_for(tu_fmt_t f)
{
	switch (f) {
	case FMT_TXT:
		return TU_FMTREAD_TXT;
	case FMT_RTF:
	case FMT_RTFD:
		return TU_FMTREAD_RICH;
	case FMT_HTML:
	case FMT_WEBARCHIVE:
		return TU_FMTREAD_ABSENT;
	default:
		return TU_FMTREAD_PACKAGE;
	}
}

	/* Which reader to use for a file nothing was forced on, from its name and
	 * its first few bytes.  Most of the extensions pick a reader outright, and
	 * having picked one no sniffing happens at all, which is why a file called
	 * notes.txt holding RTF comes back as RTF text rather than as the RTF it
	 * holds.  Only a name that is not a format at all, like the .foo of a file
	 * with no extension, has its bytes read to decide.
	 *
	 * The two gates are not alike.  RTF must begin at the first byte with the
	 * five characters {\rtf, spelled that way and no other: "{\RTF}" is text,
	 * and so is " {\rtf}" with a space in front, while a newline in front is
	 * not.  A version digit is not wanted, because "{\rtf}" and "{\rtfa}" are
	 * both read as RTF.  HTML is the opposite about both, taking any leading
	 * whitespace and ignoring case, and matching on "<html" alone, so a
	 * document that opens with a tag whose name merely starts with those five
	 * letters is read as HTML.  A doctype is enough only when it says html,
	 * so "<!doctype x>" is text.
	 *
	 * Passing the gate is not the last word, though.  The reference tool reads
	 * the file with the reader the gate chose and falls back to plain text
	 * when that read fails, which is why "{\rtf1 x}", an unclosed group, is
	 * plain text and "{\rtf1 \bogus x}" is RTF.  The gates here only choose;
	 * tu_fmt_read_falls_back is what reports a read that did not survive. */
tu_fmt_t
tu_fmt_detect(const char *path, const struct stat *st, const char *head,
    size_t headlen)
{
	const char *dot = strrchr(path, '.');
	size_t i = 0;

	/* A directory is an RTFD bundle because of what it is called.  What it
	 * holds decides whether it can be read, not what it is. */
	if (S_ISDIR(st->st_mode))
		return dot != NULL && strcasecmp(dot, ".rtfd") == 0 ?
		    FMT_RTFD : FMT_TXT;
	if (dot != NULL) {
		tu_fmt_t f = tu_fmt_parse(dot + 1);

		/* Five names are believed, and the rest are not.  A .txt, .rtf,
		 * .html, .htm or .webarchive file is what it is called, whatever
		 * it holds, so a .doc or a .docx full of RTF is read as the RTF
		 * it is, and named RTF in -info as well.  Those are the only
		 * names believed: a .doc holding plain text is reported as plain
		 * text, and so is one holding HTML, its name notwithstanding. */
		if (f == FMT_TXT || f == FMT_RTF || f == FMT_HTML ||
		    f == FMT_WEBARCHIVE)
			return f;
		/* .htm is a second spelling of HTML that the reference tool reads
		 * but does not accept from -convert, so it is a name here and
		 * not one in the table, and the parser is left alone. */
		if (strlen(dot + 1) == 3 &&
		    tolower((unsigned char)dot[1]) == 'h' &&
		    tolower((unsigned char)dot[2]) == 't' &&
		    tolower((unsigned char)dot[3]) == 'm')
			return FMT_HTML;
	}
	/* The RTF gate, after any newlines the reader would skip over anyway.
	 *
	 * Six bytes are wanted, not the five the mark is spelled in: the mark
	 * on its own is plain text and only becomes RTF once a sixth byte
	 * follows it, so "{\rtf}" is RTF and "{\rtf" is not.  Six is also what
	 * the reader itself wants, which is why the two agree. */
	while (i < headlen && (head[i] == '\n' || head[i] == '\r'))
		i++;
	if (headlen - i >= 6 && memcmp(head + i, "{\\rtf", 5) == 0)
		return FMT_RTF;
	i = 0;
	/* The HTML gate, which takes any leading whitespace and either of two
	 * openings.  Both of them want a byte past the spelling as well, and
	 * they do not want the same number: "<!doctype html" is fourteen
	 * characters and is plain text until a fifteenth byte arrives, while
	 * "<html" is five and needs two, so that every six byte document
	 * beginning "<html" is text and every seven byte one is HTML, whether
	 * the sixth byte is a digit, a space or the end of the tag. */
	while (i < headlen && isspace((unsigned char)head[i]))
		i++;
	if (headlen - i >= 7 && strncasecmp(head + i, "<html", 5) == 0)
		return FMT_HTML;
	if (headlen - i >= 15 && strncasecmp(head + i, "<!doctype html", 14) == 0)
		return FMT_HTML;
	return FMT_TXT;
}

/* Whether a read that was only a guess may be given up on.
 *
 * A name the reference tool believes is never given up on, so a file called
 * notes.rtf holding something the RTF reader will not take is an error rather
 * than plain text.  A format that came from a gate is a guess, and a guess
 * that fails is read as plain text instead, which is what makes an unclosed
 * RTF group come back as text. */
int
tu_fmt_read_falls_back(const char *path)
{
	const char *dot = strrchr(path, '.');

	if (dot == NULL)
		return 1;
	if (strcasecmp(dot, ".rtf") == 0 || strcasecmp(dot, ".html") == 0 ||
	    strcasecmp(dot, ".htm") == 0)
		return 0;
	return 1;
}
