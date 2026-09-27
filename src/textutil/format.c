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

#include <string.h>

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
	[FMT_WORDML]    = { "wordml",    "wordml",    "Word XML format" },
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
