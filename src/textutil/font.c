/*
 * textutil - the font database -font is resolved against.
 *
 * The reference tool resolves -font through a font database, taking the family
 * a face belongs to and the PostScript name to write for it, and that database
 * holds every font installed on the machine, so it cannot be reproduced from
 * black-box probing.  What can be reproduced is the shape of the lookup:
 *
 *   - a name is matched against the families installed, without regard to case,
 *     so "courier" and "COURIER" are the same font;
 *   - some names are aliases, and an alias is matched only as it is spelled, so
 *     "Helvetica-Light" is a font but "helvetica-light" is not;
 *   - the PostScript name is an *output*, never an input, so "ArialMT" and
 *     "CourierNewPSMT" are not fonts to it;
 *   - a face that is bold or italic also sets a run attribute, and each writer
 *     writes those two in its own order;
 *   - a name that resolves to nothing is not an error but is not the default
 *     either: RTF writes the swiss family under plain "Helvetica", where no
 *     -font at all writes "Helvetica-Light".
 *
 * The two tables below are the part of that database that was sampled, chosen
 * to cover the faces a document is likely to name.  See the last item under
 * Divergences in src/textutil/NOTES.md for what happens outside them.
 *
 * Copyright (C) 2026, LibreDarwin
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include <stddef.h>
#include <string.h>

#include "textutil.h"

static const struct {
	const char *name;
	const char *family;		/* the RTF family keyword */
	const char *postscript;
	int italic;
	int bold;
	const char *wml;			/* the family wordml writes */
} font_families[] = {
	{ "Helvetica",		"\\fswiss",	"Helvetica-Light",	0, 0, "Helvetica Light" },
	{ "Arial",		"\\fswiss",	"ArialMT",		0, 0, "Arial" },
	{ "Arial Bold",		"\\fswiss",	"Arial-BoldMT",		0, 1, "Arial" },
	{ "Arial Bold Italic",	"\\fswiss",	"Arial-BoldItalicMT",	1, 1, "Arial" },
	{ "Arial Unicode MS",	"\\fswiss",	"ArialUnicodeMS",	0, 0, "Arial Unicode MS" },
	{ "Optima",		"\\fswiss",	"Optima-Regular",	0, 0, "Optima" },
	{ "Times",		"\\froman",	"Times-Roman",		0, 0, "Times" },
	{ "Times New Roman",	"\\froman",	"TimesNewRomanPSMT",	0, 0, "Times New Roman" },
	{ "Palatino",		"\\froman",	"Palatino-Roman",	0, 0, "Palatino" },
	{ "Palatino Bold",	"\\froman",	"Palatino-Bold",	0, 1, "Palatino" },
	{ "Palatino Italic",	"\\froman",	"Palatino-Italic",	1, 0, "Palatino" },
	{ "Courier",		"\\fmodern",	"Courier",		0, 0, "Courier" },
	{ "Courier New",	"\\fmodern",	"CourierNewPSMT",	0, 0, "Courier New" },
	{ "Monaco",		"\\fnil",	"Monaco",		0, 0, "Monaco" },
	{ "Menlo",		"\\fnil",	"Menlo-Regular",	0, 0, "Menlo" },
	{ "Andale Mono",	"\\fnil",	"AndaleMono",		0, 0, "Andale Mono" },
	{ "Verdana",		"\\fnil",	"Verdana",		0, 0, "Verdana" },
	{ "Geneva",		"\\fnil",	"Geneva",		0, 0, "Geneva" },
	{ "Zapfino",		"\\fnil",	"Zapfino",		0, 0, "Zapfino" },
	{ "Apple Chancery",	"\\fnil",	"Apple-Chancery",	0, 0, "Apple Chancery" },
	{ "Baskerville",	"\\fnil",	"Baskerville",		0, 0, "Baskerville" },
	{ "American Typewriter","\\fnil",	"AmericanTypewriter-Light", 0, 0, "American Typewriter Light" },
	{ "Charter",		"\\fnil",	"Charter-Roman",	0, 0, "Charter" },
	{ "Cochin",		"\\fnil",	"Cochin",		0, 0, "Cochin" },
	{ "Didot",		"\\fnil",	"Didot",		0, 0, "Didot" },
	{ "Futura",		"\\fnil",	"Futura-Medium",	0, 0, "Futura" },
	{ "Avenir",		"\\fnil",	"Avenir-Light",		0, 0, "Avenir Light" },
	{ "Avenir Next",	"\\fnil",	"AvenirNext-UltraLight", 0, 0, "Avenir Next" },
	{ "Gill Sans",		"\\fnil",	"GillSans-Light",	0, 0, "Gill Sans Light" },
	{ "Savoye LET",		"\\fnil",	"SavoyeLetPlain",	0, 0, "Savoye LET" }
};

/* Aliases, matched as spelled.  Several carry no weight the family table does
 * not already give, and exist here because the reference tool accepts them. */
static const struct {
	const char *name;
	const char *family;
	const char *postscript;
	int italic;
	int bold;
	const char *wml;
} font_aliases[] = {
	{ "Helvetica-Light",	"\\fswiss",	"Helvetica-Light",	0, 0, "Helvetica Light" },
	{ "Helvetica Bold",	"\\fswiss",	"Helvetica-Bold",	0, 1, "Helvetica" },
	{ "Helvetica Oblique",	"\\fswiss",	"Helvetica-Oblique",	1, 0, "Helvetica" },
	{ "Helvetica Bold Oblique", "\\fswiss",	"Helvetica-BoldOblique", 1, 1, "Helvetica" },
	{ "Palatino-Roman",	"\\froman",	"Palatino-Roman",	0, 0, "Palatino" },
	{ "Palatino-Bold",	"\\froman",	"Palatino-Bold",	0, 1, "Palatino" }
};

#define	DEFAULT_FONT	"Helvetica-Light"
#define	DEFAULT_FAMILY	"\\fswiss"
#define	DEFAULT_WML	"Helvetica Light"
/* What a name that resolves to no font at all writes, which is a different
 * string from the default: a face was asked for and none was found, so the
 * PostScript name falls back to plain Helvetica rather than to the default's
 * Helvetica-Light. */
#define	UNKNOWN_FONT	"Helvetica"

static int
ci_equal(const char *a, const char *b)
{
	while (*a != '\0' && *b != '\0') {
		int ca = (unsigned char)*a++, cb = (unsigned char)*b++;

		if (ca >= 'A' && ca <= 'Z')
			ca += 'a' - 'A';
		if (cb >= 'A' && cb <= 'Z')
			cb += 'a' - 'A';
		if (ca != cb)
			return 0;
	}
	return *a == *b;
}

/* Resolve a -font argument to everything a writer needs of the face it names.
 * A name that matches nothing keeps the default family and loses the default
 * name: see UNKNOWN_FONT.  The wordml name is the family the face belongs to
 * rather than the face, which is why "Arial Bold" is Arial there and "Avenir"
 * is Avenir Light: the face Avenir resolves to is Avenir-Light. */
void
tu_font_face(const char *want, tu_font_t *f)
{
	f->family = DEFAULT_FAMILY;
	f->postscript = DEFAULT_FONT;
	f->wml = DEFAULT_WML;
	f->italic = 0;
	f->bold = 0;
	if (want == NULL)
		return;
	for (size_t i = 0; i < sizeof(font_families) / sizeof(font_families[0]); i++)
		if (ci_equal(want, font_families[i].name)) {
			f->family = font_families[i].family;
			f->postscript = font_families[i].postscript;
			f->wml = font_families[i].wml;
			f->italic = font_families[i].italic;
			f->bold = font_families[i].bold;
			return;
		}
	for (size_t i = 0; i < sizeof(font_aliases) / sizeof(font_aliases[0]); i++)
		if (strcmp(want, font_aliases[i].name) == 0) {
			f->family = font_aliases[i].family;
			f->postscript = font_aliases[i].postscript;
			f->wml = font_aliases[i].wml;
			f->italic = font_aliases[i].italic;
			f->bold = font_aliases[i].bold;
			return;
		}
	f->postscript = UNKNOWN_FONT;
	f->wml = "Helvetica";
}
