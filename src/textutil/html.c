/*
 * textutil - HTML output.
 *
 * The reference tool emits a fixed HTML 4.01 Strict envelope with an empty
 * <title> and a Cocoa HTML Writer generator tag, then one paragraph per line
 * of input.
 *
 * Paragraphs come in two flavours: a line with text on it, and a line with
 * none.  Each flavour gets a class, and the classes are numbered in the order
 * the paragraphs first introduced them, so a document of nothing but blank
 * lines calls that first class p1 rather than p2.  A line with nothing on it
 * is emitted as <br>, or as no-break spaces when it holds only blanks, and
 * its class is given a min-height so that it still occupies a line.  Only the
 * classes actually used appear in the stylesheet, which is therefore
 * assembled after the body is known.
 *
 * Within a line, a run of blanks is rebuilt inside an Apple-converted-space
 * span, every other space becoming a no-break space so that the run keeps its
 * width, a tab becomes an Apple-tab-span, and &, < and > are escaped.
 * Everything else, including text outside ASCII, is written through as UTF-8
 * unless -encoding asks for one of the wide encodings, in which case the whole
 * document is re-encoded and the charset attribute names the encoding that was
 * used.
 *
 * Copyright (C) 2026, LibreDarwin
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "textutil.h"

static const char head_before_style[] =
    "<!DOCTYPE html PUBLIC \"-//W3C//DTD HTML 4.01//EN\""
    " \"http://www.w3.org/TR/html4/strict.dtd\">\n"
    "<html>\n"
    "<head>\n";
/* The charset attribute names the encoding the document is written in, so it
 * follows -encoding rather than always saying utf-8. */
static const char meta_prefix[] =
    "  <meta http-equiv=\"Content-Type\" content=\"text/html; charset=";
static const char meta_suffix[] = "\">\n";
static const char meta_after_charset[] =
    "  <meta http-equiv=\"Content-Style-Type\" content=\"text/css\">\n";

/* -title fills the empty <title> the reference tool writes by default. */
static const char title_open[] = "  <title>";
static const char title_close[] = "</title>\n";
static const char title_empty[] = "  <title></title>\n";

static const char head_after_title[] =
    "  <meta name=\"Generator\" content=\"Cocoa HTML Writer\">\n"
    "  <meta name=\"CocoaVersion\" content=\"2685.6\">\n"
    "  <style type=\"text/css\">\n";

static const char head_after_style[] = "  </style>\n</head>\n<body>\n";
static const char tail[] = "</body>\n</html>\n";

struct sink {
	char *buf;
	size_t len, cap;
	int failed;
};

static void
sink_put(struct sink *s, const char *p, size_t n)
{
	if (s->failed)
		return;
	if (s->len + n + 1 > s->cap) {
		size_t cap = s->cap ? s->cap : 512;
		char *b;

		while (cap < s->len + n + 1)
			cap *= 2;
		b = realloc(s->buf, cap);
		if (b == NULL) {
			s->failed = 1;
			return;
		}
		s->buf = b;
		s->cap = cap;
	}
	memcpy(s->buf + s->len, p, n);
	s->len += n;
}

static void
sink_str(struct sink *s, const char *p)
{
	sink_put(s, p, strlen(p));
}

static void
emit_escaped(struct sink *s, const char *p, size_t n)
{
	for (size_t i = 0; i < n; i++) {
		switch (p[i]) {
		case '&': sink_str(s, "&amp;"); break;
		case '<': sink_str(s, "&lt;"); break;
		case '>': sink_str(s, "&gt;"); break;
		default:  sink_put(s, &p[i], 1); break;
		}
	}
}

/* A copy of v with every ill-formed piece of UTF-8 put to U+FFFD.  A command
 * line argument is meant to be text, so the reference tool repairs it this way
 * before it reaches the document; a file read as plain text is a different
 * matter and is taken as MacRoman instead, which is why this is not also done
 * to the body. */
static char *
repaired(const char *v)
{
	size_t n = strlen(v), i = 0, w = 0;
	char *out = malloc(n * 3 + 1);

	if (out == NULL)
		return NULL;
	while (i < n) {
		unsigned long cp;
		size_t seq;

		cp = tu_utf8_strict((const unsigned char *)v + i, n - i, &seq);
		i += seq;
		if (cp < 0x80)
			out[w++] = (char)cp;
		else if (cp < 0x800) {
			out[w++] = (char)(0xc0 | (cp >> 6));
			out[w++] = (char)(0x80 | (cp & 0x3f));
		} else {
			out[w++] = (char)(0xe0 | (cp >> 12));
			out[w++] = (char)(0x80 | ((cp >> 6) & 0x3f));
			out[w++] = (char)(0x80 | (cp & 0x3f));
		}
	}
	out[w] = '\0';
	return out;
}

/* As emit_escaped(), but for the inside of an attribute, where a quote would
 * otherwise end the value. */
static void
emit_escaped_attr(struct sink *s, const char *p, size_t n)
{
	for (size_t i = 0; i < n; i++) {
		switch (p[i]) {
		case '&': sink_str(s, "&amp;"); break;
		case '<': sink_str(s, "&lt;"); break;
		case '>': sink_str(s, "&gt;"); break;
		case '"': sink_str(s, "&quot;"); break;
		default:  sink_put(s, &p[i], 1); break;
		}
	}
}

/* One metadata meta tag. */
static void
emit_meta(struct sink *s, const char *name, const char *value)
{
	char tmp[48];

	snprintf(tmp, sizeof(tmp), "  <meta name=\"%s\" content=\"", name);
	sink_str(s, tmp);
	emit_escaped_attr(s, value, strlen(value));
	sink_str(s, "\">\n");
}

/* The metadata the head carries, in the order the reference tool writes it.  The
 * names are not the option names: -editor is LastAuthor and -comment is
 * Description.  -keywords is last of the plain values, and the two times come
 * after it. */
static const struct {
	size_t offset;
	const char *name;
} head_metas[] = {
	{ offsetof(tu_meta_t, author),	"Author" },
	{ offsetof(tu_meta_t, editor),	"LastAuthor" },
	{ offsetof(tu_meta_t, company),	"Company" },
	{ offsetof(tu_meta_t, subject),	"Subject" },
	{ offsetof(tu_meta_t, comment),	"Description" },
	{ offsetof(tu_meta_t, keywords),	"Keywords" },
};

/* Where a run of spaces sits in its line, which decides what becomes a no-break
 * space.  The run is taken apart in pairs, each pair becoming a no-break space
 * followed by the space as typed, and any lone space left over is treated
 * according to where it fell. */
enum space_where {
	SPACE_LEAD,		/* the run opens the line */
	SPACE_MID,		/* the run has text on both sides */
	SPACE_TAIL,		/* the run closes the line */
	SPACE_BLANK,		/* the run is the whole of a blank line */
};

/* What a whitespace region does with its spaces, worked out before any of it is
 * written.  The spaces fall into pairs, each pair a no-break space followed by
 * the space as typed, and the odd one over is what tells the cases apart: at the
 * head of a line it becomes a no-break space leading the span, so three spaces
 * there give NBSP+NBSP+space; between two words it is left as typed and falls
 * outside the span, so one space there is just a space; and at the end of a line
 * it moves past the pairs to close the span as a no-break space, so a run of two
 * there gives a space followed by a span of one no-break space.  A line of
 * nothing but blanks closes the same way but keeps the no-break space that would
 * otherwise be a plain one, so two such spaces are both no-break. */
struct space_plan {
	size_t pairs;		/* no-break space and space, repeated */
	size_t extra;		/* no-break spaces written before the pairs */
	size_t closing;		/* no-break space closing the span */
	int literal;		/* a space left outside the span */
	int inside;		/* whether any space falls inside the span */
};

/* The no-break space or the plain space that the i'th space inside the span
 * becomes.  The pairs alternate, and anything counted apart from them is a
 * no-break space, as is the one that closes the span. */
static int
space_is_breaks(const struct space_plan *pl, size_t i)
{
	if (i < pl->extra)
		return 1;
	if (i < pl->extra + 2 * pl->pairs)
		return ((i - pl->extra) % 2) == 0;
	return 1;
}

static void
plan_space_run(size_t n, enum space_where where, struct space_plan *pl)
{
	int odd = (int)(n % 2);

	pl->pairs = n / 2;
	pl->extra = 0;
	pl->closing = 0;
	pl->literal = where == SPACE_MID && odd;
	pl->inside = 0;

	if (where == SPACE_LEAD && odd)
		pl->extra = 1;		/* the leading space becomes no-break */
	else if (where == SPACE_TAIL || where == SPACE_BLANK) {
		pl->closing = 1;
		if (!odd) {
			if (where == SPACE_TAIL)
				pl->literal = 1;	/* a plain space before the span */
			else
				pl->extra = 1;	/* a blank line keeps it no-break */
			pl->pairs--;
		}
	}
	pl->inside = pl->extra + 2 * pl->pairs + pl->closing > 0;
}

/* The levels a run is under, the spans that are open for them, and the classes
 * the levels a run opens with are written as.  The outermost level of a run is
 * given a class of its own, because that is the one a reader is most likely to
 * meet again; the levels inside it are written as a style, since they name
 * nothing the outer level has not already said.  The classes are handed out in
 * the order the four controls are first seen, so s1 is whichever direction the
 * first embedding of the document reads. */
struct html_embed {
	unsigned long *stack;	/* the levels the text is under */
	unsigned long *open;	/* the levels the open spans are for */
	size_t dep;		/* how deep the stack is */
	size_t room;		/* how deep it can be */
	size_t shown;		/* how many spans are open */
	int cls[4];		/* the class each of the four openers has */
	int ncls;		/* how many of them there are */
};

/* The four controls that open a level, in code point order. */
static const unsigned long embed_openers[4] = {
	0x202A, 0x202B, 0x202D, 0x202E,
};

static int
embed_which(unsigned long cp)
{
	for (int k = 0; k < 4; k++)
		if (embed_openers[k] == cp)
			return k;
	return -1;
}

/* One more level, keeping room for it.  The nesting is not capped, so the
 * stack grows with it.  The list the open spans are for is kept the same size
 * so that a level in force can be written down as one that is open without
 * either list having to grow again. */
static void
embed_push(struct html_embed *e, unsigned long cp)
{
	if (e->dep == e->room) {
		size_t room = e->room + 8;
		unsigned long *stack = realloc(e->stack, room * sizeof(*stack));
		unsigned long *open = stack ? realloc(e->open, room * sizeof(*open))
					    : NULL;

		if (open == NULL) {
			free(stack);
			return;		/* out of memory; the level is lost */
		}
		e->stack = stack;
		e->open = open;
		e->room = room;
	}
	e->stack[e->dep++] = cp;
}

/* The span for a level: a class for the outermost, a style for the rest. */
static void
embed_open(struct sink *s, struct html_embed *e, unsigned long cp, int outer)
{
	if (!outer) {
		sink_str(s, "<span style=\"");
		sink_str(s, tu_bidi_css(cp));
		sink_str(s, "\">");
		return;
	}
	{
		int k = embed_which(cp);

		if (e->cls[k] == 0)
			e->cls[k] = ++e->ncls;
		sink_str(s, "<span class=\"s");
		sink_put(s, (const char *)&(char){ (char)('0' + e->cls[k]) }, 1);
		sink_str(s, "\">");
	}
}

static void
embed_close(struct sink *s, struct html_embed *e)
{
	while (e->shown > 0) {
		sink_str(s, "</span>");
		e->shown--;
	}
}

/* Bring the spans that are open into line with the levels that are.  The
 * reference tool writes none of the levels a run and the next one share, so
 * this closes every span and opens every level again from the outermost.  Text
 * that is under the levels the open spans already say goes on inside them, so
 * a run of text is not broken up by a control that says nothing about it, and
 * neither is it broken up by a level that opens and closes around it. */
static void
embed_sync(struct sink *s, struct html_embed *e)
{
	size_t k = 0;

	while (k < e->dep && k < e->shown && e->open[k] == e->stack[k])
		k++;
	if (k == e->dep && k == e->shown)
		return;
	embed_close(s, e);
	for (k = 0; k < e->dep; k++)
		embed_open(s, e, e->stack[k], k == 0);
	memcpy(e->open, e->stack, e->dep * sizeof(*e->stack));
	e->shown = e->dep;
}

/* Whether the levels in force are not the ones the open spans are for, which is
 * what a call to embed_sync would act on.  A level that opens and one that
 * closes around nothing leave the two the same, so a run of text between them
 * goes on inside the spans it was already in. */
static int
embed_stale(const struct html_embed *e)
{
	size_t k = 0;

	while (k < e->dep && k < e->shown && e->open[k] == e->stack[k])
		k++;
	return k != e->dep || k != e->shown;
}

/* One region of blanks and tabs, p[i] through p[j), which may hold levels
 * between its blanks.  A tab keeps the place it was typed in and is written as a
 * tab of its own, which the reference does too, but it is not a space, so it
 * does not enter the pairs: it neither makes one more nor turns an odd run
 * even.  A tab also moves the region off the head of its line, so a run that
 * would have led a line is treated as one between two words instead, and a blank
 * line that starts with a tab closes like a run at the end of a line rather than
 * filling.
 *
 * A level in the middle of a region says nothing about how its blanks are
 * counted, so the pairs are worked out over the whole of it; only the markup is
 * cut at the level.  The level is not acted on when it is read, since a level
 * that opens and one that closes around nothing but spaces leave the spans as
 * they were, and the spans are brought into line only when a space is written
 * and the stack has moved.  A space that is written as typed opens no span of
 * its own, so one that follows a level lands outside the span the level left
 * open, while one that follows a no-break space stays inside it. */
static void
emit_space_region(struct sink *s, const char *p, size_t i, size_t j,
    enum space_where where, int *used_tab, struct html_embed *e)
{
	struct space_plan pl;
	size_t nspaces = 0, written = 0, inside = 0;
	int open = 0, moved = 0;

	for (size_t k = i; k < j; k++)
		if (p[k] == ' ')
			nspaces++;
	if (i < j && p[i] == '\t') {
		if (where == SPACE_LEAD)
			where = SPACE_MID;
		else if (where == SPACE_BLANK)
			where = SPACE_TAIL;
	}
	plan_space_run(nspaces, where, &pl);
	/* text earlier in the line may have left a level the spans have not
	 * caught up with yet */
	embed_sync(s, e);

	for (size_t k = i; k < j; k++) {
		unsigned long cp = tu_bidi_embed((const unsigned char *)p + k, j - k);
		int breaks;

		if (cp != 0) {
			if (cp == 0x202C) {
				if (e->dep > 0)
					e->dep--;
			} else {
				embed_push(e, cp);
			}
			moved = 1;
			k += 2;
			continue;
		}
		if (moved) {
			/* the span of no-break spaces ends at the level, and
			 * the spans that say which level this is open only now
			 * that a space is written into them; a level that
			 * opened and one that closed leave both as they were */
			if (embed_stale(e)) {
				if (open) {
					sink_str(s, "</span>");
					open = 0;
				}
				embed_sync(s, e);
			}
			moved = 0;
		}
		if (p[k] == '\t') {
			/* the tab is inside the span of no-break spaces only
			 * while that span is still open, and is left there
			 * after the last space of it */
			sink_str(s, "<span class=\"Apple-tab-span\">\t</span>");
			*used_tab = 1;
			continue;
		}
		if (pl.literal && written == 0) {
			if (open) {
				sink_str(s, "</span>");
				open = 0;
			}
			sink_put(s, " ", 1);
			written++;
			continue;
		}
		if (!pl.inside)
			continue;	/* a single space between two words */
		breaks = space_is_breaks(&pl, inside);
		if (!breaks && !open) {
			/* a space as typed is left outside the span, so one that
			 * follows a level stays out of the one just closed; it
			 * still counts towards the pairs that follow it */
			sink_put(s, " ", 1);
			inside++;
			written++;
			continue;
		}
		if (!open) {
			sink_str(s, "<span class=\"Apple-converted-space\">");
			open = 1;
		}
		sink_str(s, breaks ? "\xc2\xa0" : " ");
		inside++;
		written++;
	}
	if (moved && embed_stale(e)) {
		/* a level with no space after it in this region, and the
		 * reference writes none of it.  A level that opened and one
		 * that closed inside the region leave the spans as they were,
		 * and those the reference leaves in place too, so only a
		 * level still open at the end of the region closes them. */
		embed_close(s, e);
	}
	if (open)
		sink_str(s, "</span>");
}

/* Whether anything the reader is shown comes before p[k].  A tab is something
 * the reader is shown and a level or a control is not, and the answer is what
 * tells a word gap from the indent at the front of a line. */
static int
shown_before(const char *p, size_t k)
{
	size_t j = 0;

	while (j < k) {
		if (tu_bidi_embed((const unsigned char *)p + j, k - j) != 0 ||
		    tu_bidi_open((const unsigned char *)p + j, k - j) != 0) {
			j += 3;
			continue;
		}
		if ((unsigned char)p[j] < 0x20 && p[j] != '\t' && p[j] != '\n' &&
		    p[j] != '\r') {
			j++;
			continue;
		}
		return 1;
	}
	return 0;
}

/* Whether p[i] is nothing but the levels a run is under, and whether p[j] is
 * followed by nothing but the levels a run is under and blanks.  A run of
 * spaces is at the head of a line when only levels come before it, and at the
 * tail when neither levels nor blanks come after it.
 *
 * A control of no account to the writer arms the head of the line again, so a
 * run of blanks that follows one leads its line however much text came before
 * it.  It also closes the run before it, since ends_line stops at one, so that
 * run is never the one that fills a blank line.
 *
 * A NUL arms the head on any count, which is the first of the two readings and
 * the one that came first.  The other twenty-six need a word gap between them
 * and the run, where a NUL does not: one space, and one space only, with
 * something the reader is shown in front of it.  So 'A <control>   B' has its
 * run at the head, and 'A  <control>   B', ' <control>   B' and
 * '<level> <control>   B' have theirs between two words, the one in the middle
 * of those because a space at the head of a line is an indent rather than a
 * word gap.  A tab in front of the space leaves it a word gap and does arm the
 * head, and a tab after it does not, which is what shown_before is for.
 *
 * A sweep of the twenty-seven controls over blank runs either side of them, in
 * every shape of lead, of blank count and of what follows, is what settles
 * this: it is 12960 shapes, and taking the twenty-six to arm the head on any
 * count at all, which is the simplest reading to try, turns fifteen wrong
 * shapes per control into a hundred and five. */
static int
arms_line(const char *p, size_t i)
{
	if (i == 0)
		return 1;
	if (p[i - 1] == '\0')
		return 1;
	if (p[i - 1] < 0x20 && p[i - 1] != '\t' && p[i - 1] != '\n' &&
	    p[i - 1] != '\r')
		return i >= 2 && p[i - 2] == ' ' && (i < 3 || p[i - 3] != ' ') &&
		    shown_before(p, i - 2);
	return 0;
}

static int
opens_line(const char *p, size_t i)
{
	while (i >= 3 && tu_bidi_embed((const unsigned char *)p + i - 3, 3) != 0)
		i -= 3;
	return arms_line(p, i);
}

static int
ends_line(const char *p, size_t n, size_t j)
{
	while (j < n) {
		if (tu_bidi_embed((const unsigned char *)p + j, n - j) != 0) {
			j += 3;
			continue;
		}
		if (p[j] != ' ' && p[j] != '\t')
			return 0;
		j++;
	}
	return 1;
}

/* Whether a line has anything the reader is shown once the levels in it are
 * taken out for the spans that will say what each run of text was under.  A
 * line of levels and nothing else is a blank line to the reference tool, and
 * one of them and blanks is a blank line with a run of text in it, so neither
 * needs a paragraph of its own kind.  The other C0 controls are different: the
 * line they are on is a paragraph that is empty once they are gone, and it
 * takes a class of its own, so they are not looked for here. */
static int
has_shown(const char *p, size_t n)
{
	size_t i = 0;

	while (i < n) {
		if (tu_bidi_embed((const unsigned char *)p + i, n - i) == 0)
			return 1;
		i += 3;
	}
	return 0;
}

/* Whether the three bytes of one of the two Unicode separators, U+2028 and
 * U+2029, start at k.  They are the only two characters the reference tool
 * spells out, one as a break within a paragraph and one as the end of it, and
 * neither is shown.  Which of the two it is does not change the three bytes,
 * so the caller says. */
static int
at_separator(const char *text, size_t len, size_t k, unsigned char last)
{
	return len - k >= 3 && (unsigned char)text[k] == 0xE2 &&
	    (unsigned char)text[k + 1] == 0x80 &&
	    (unsigned char)text[k + 2] == last;
}

/* One paragraph's worth of line content.  Sets *used_tab when a tab was
 * written, because that adds a rule to the stylesheet.
 *
 * The C0 controls are dropped here rather than before the line was measured,
 * because a line of nothing but controls is not a blank line to the reference
 * tool: it asks for a class of its own and writes a paragraph that is empty
 * once the controls are gone.  Only tab, line feed and carriage return are text
 * of their own, and U+007F is kept.
 *
 * The levels are dropped too, and what each run of text was under is written as
 * the spans around it.  A level that opens and one that closes say nothing
 * about the text that follows until some text does follow, so the spans they
 * would have changed are closed when that text comes rather than when the level
 * is read.  A form feed is no account to any of this: it is one of the C0
 * controls, and neither ends a run of text nor changes a level. */
static void
emit_line_body(struct sink *s, const char *p, size_t n, struct html_embed *e,
    int *used_tab)
{
	size_t i = 0;

	while (i < n) {
		unsigned char c = (unsigned char)p[i];
		unsigned long cp = tu_bidi_embed((const unsigned char *)p + i, n - i);

		if (cp != 0) {
			if (cp == 0x202C) {
				if (e->dep > 0)
					e->dep--;
			} else {
				embed_push(e, cp);
			}
			i += 3;
			continue;
		}
		if (c == ' ' || c == '\t') {
			size_t j = i;
			enum space_where where;
			int head, tail;

			/* a level between two blanks is of no account to
			 * them, so a region runs on through one */
			while (j < n) {
				if (p[j] == ' ' || p[j] == '\t') {
					j++;
					continue;
				}
				if (tu_bidi_embed((const unsigned char *)p + j,
				    n - j) == 0)
					break;
				j += 3;
			}
			head = opens_line(p, i);
			tail = ends_line(p, n, j);
			if (head && tail)
				where = SPACE_BLANK;
			else if (head)
				where = SPACE_LEAD;
			else if (tail)
				where = SPACE_TAIL;
			else
				where = SPACE_MID;
			emit_space_region(s, p, i, j, where, used_tab, e);
			i = j;
		} else if (c < 0x20 && c != '\t' && c != '\n' && c != '\r') {
			/* A control of no account to the writer, and none to the
			 * levels either: the spans the levels want are opened
			 * as they are and left with nothing in them. */
			embed_sync(s, e);
			i++;
		} else if (at_separator(p, n, i, 0xA8)) {
			/* A line separator ends a line inside the paragraph
			 * rather than the paragraph itself, and is written as
			 * the break that is, which carries a line ending of
			 * its own after it.  It is room the line takes up,
			 * so the levels are stated before it, and the break
			 * falls inside whatever span they want. */
			embed_sync(s, e);
			sink_str(s, "<br>\n");
			i += 3;
		} else {
			embed_sync(s, e);
			emit_escaped(s, &p[i], 1);
			i++;
		}
	}
	embed_close(s, e);
	/* A paragraph ends with no levels open, however many were open at its
	 * end: the next paragraph begins at the left whatever this one did. */
	e->dep = 0;
}

/* The first byte of the line at or after k that a reader is shown, which is
 * the byte after any run of controls and marks: the reference tool takes those
 * out before it looks for the end of a line, so a carriage return and a line
 * feed with nothing but a level between them are still one terminator. */
static size_t
line_shown(const char *text, size_t len, size_t k)
{
	while (k < len) {
		if (tu_bidi_embed((const unsigned char *)text + k, len - k) != 0 ||
		    tu_bidi_open((const unsigned char *)text + k, len - k) != 0) {
			k += 3;
			continue;
		}
		break;
	}
	return k;
}

/* Whether a paragraph separator, U+2029, starts at k.  It is a line terminator
 * in its own right, and unlike a CR it pairs with nothing: a CR before it ends
 * a line and so does the separator, which is two terminators where a CRLF pair
 * is one. */
static int
at_para_sep(const char *text, size_t len, size_t k)
{
	return at_separator(text, len, k, 0xA9);
}

/* Where a CR and an LF that pair with each other stop being between them.  A CR
 * begins a line, and the head of a line may name a direction, so a mark
 * immediately after it is that mark and is gone.  What is left is the CR/LF
 * question proper, and it goes over the controls and no further: a second mark,
 * or a mark that a control came before, is text between the two halves of what
 * would have been a pair, and so keeps them apart. */
static size_t
pair_shown(const char *text, size_t len, size_t k)
{
	k += tu_bidi_mark((const unsigned char *)text + k, len - k);
	while (tu_bidi_embed((const unsigned char *)text + k, len - k) != 0)
		k += 3;
	return k;
}

/* Where the line starting at pos ends, and how many bytes it occupies.  A
 * CR, an LF, a CRLF pair and a U+2029 each end a line and each count once, and
 * the levels and marks in between are of no account to which of them it is. */
static size_t
line_at(const char *text, size_t len, size_t pos, size_t *adv)
{
	size_t i = line_shown(text, len, pos), end;
	int sep;

	while (i < len && text[i] != '\n' && text[i] != '\r' &&
	    !at_para_sep(text, len, i))
		i = line_shown(text, len, i + 1);
	end = i;
	sep = at_para_sep(text, len, i);
	*adv = i - pos + (sep ? 3 : 1);
	if (i < len && text[i] == '\r') {
		size_t j = pair_shown(text, len, i + 1);

		if (j < len && text[j] == '\n')
			*adv = j - pos + 1;	/* the pair is one terminator */
	}
	return end - pos;
}

/* How many lines the text holds.  A trailing terminator closes the last line
 * rather than opening an empty one, so "a\n" is one line and "a\n\n" is
 * two: the second is empty. */
static size_t
count_lines(const char *text, size_t len)
{
	size_t n = 0, pos = 0, adv;

	if (len == 0)
		return 0;
	while (pos < len) {
		line_at(text, len, pos, &adv);
		pos += adv;
		n++;
	}
	return n;
}

int
tu_html_build(const tu_doc_t *d, const tu_style_t *st, const tu_meta_t *meta,
    tu_encoding_t enc, char **out, size_t *outlen)
{
	(void)st;			/* the HTML writer takes no run options */
	struct sink s = { NULL, 0, 0, 0 };
	struct sink body = { NULL, 0, 0, 0 };
	const char *text = d->text;
	size_t len = d->len, pos = 0, want = count_lines(text, len);
	size_t n = 0;
	/* Class numbers are handed out in order of first use, so a document of
	 * nothing but blank lines calls the first of them p1.  Format 0 is a
	 * line with text on it, format 1 a line with none. */
	int cls[2] = { 0, 0 }, ncls = 0;
	int used_tab = 0;
	/* The levels are counted for the whole document, because the classes the
	 * outermost of them are written as are handed out in the order the
	 * document first needed them, but a paragraph always starts and ends
	 * with none of them open. */
	struct html_embed embed = { NULL, NULL, 0, 0, 0, { 0, 0, 0, 0 }, 0 };
	char *outbuf = NULL;
	size_t lenout = 0;

	/* The body is built first, because the stylesheet lists the classes in
	 * the order the paragraphs introduced them, and the stylesheet has to
	 * close before <body> can open. */
	while (n < want) {
		size_t adv, linelen = line_at(text, len, pos, &adv);
		size_t mark, shown;
		int blank = 1, fmt, c, rtl, drop, vis;

		/* A line that begins with a mark saying which way to read is written
		 * with the mark left out and, since the mark is what named the
		 * direction, that of the line as it was before the mark went.  A line
		 * that was nothing but the mark is then empty, and is written as the
		 * empty line is, unless it is the last line and there is no terminator
		 * to close it: a mark that leaves the last line empty takes that line
		 * away with it, as a trailing terminator would not open it. */
		rtl = tu_bidi_first((const unsigned char *)text + pos, linelen) != 0;
		mark = tu_bidi_mark((const unsigned char *)text + pos, linelen);
		shown = linelen - mark;
		vis = has_shown(text + pos + mark, shown);
		drop = !vis && pos + linelen >= len;

		/* Whether the line is blank is asked of what is left of it, the mark
		 * not being something the reader is shown and so not room the line
		 * takes up: a line of one mark and one space is a blank line.  A
		 * level is not room a line takes up either, so a line of levels
		 * and nothing else is a blank line as well -- and so is one of
		 * levels and blanks, which is where the levels differ from the
		 * other C0 controls, a line of those being a paragraph empty but
		 * for them. */
		for (size_t k = mark; k < linelen; ) {
			if (tu_bidi_embed((const unsigned char *)text + pos + k,
			    linelen - k) != 0) {
				k += 3;
				continue;
			}
			if (text[pos + k] != ' ' && text[pos + k] != '\t') {
				blank = 0;
				break;
			}
			k++;
		}
		fmt = blank ? 1 : 0;
		if (!drop) {
			if (cls[fmt] == 0)
				cls[fmt] = ++ncls;
			c = cls[fmt];

			sink_str(&body, "<p");
			if (rtl)
				sink_str(&body, " dir=\"rtl\"");
			sink_str(&body, " class=\"p");
			sink_put(&body, (const char *)&(char){ (char)('0' + c) }, 1);
			if (!vis) {
				sink_str(&body, "\"><br></p>\n");
			} else {
				sink_str(&body, "\">");
				emit_line_body(&body, text + pos + mark, shown,
				    &embed, &used_tab);
				sink_str(&body, "</p>\n");
			}
		}
		pos += adv;
		n++;
	}

	sink_str(&s, head_before_style);
	sink_str(&s, meta_prefix);
	sink_str(&s, tu_encoding_name(enc));
	sink_str(&s, meta_suffix);
	sink_str(&s, meta_after_charset);
	if (meta != NULL && meta->title != NULL) {
		char *title = repaired(meta->title);

		if (title != NULL) {
			sink_str(&s, title_open);
			emit_escaped(&s, title, strlen(title));
			sink_str(&s, title_close);
			free(title);
		} else
			sink_str(&s, title_empty);
	} else
		sink_str(&s, title_empty);
	for (size_t i = 0;
	    i < sizeof(head_metas) / sizeof(head_metas[0]); i++) {
		const char *v = *(const char *const *)((const char *)meta +
		    head_metas[i].offset);

		if (v != NULL) {
			char *fixed = repaired(v);

			if (fixed != NULL) {
				emit_meta(&s, head_metas[i].name, fixed);
				free(fixed);
			}
		}
	}
	/* A time is written in the same normalised form the RTF writer uses, so
	 * a month of 13 is the February that follows.  The tag named
	 * ModificationTime is a copy of the creation time, or of nothing at
	 * all when there was no -creationtime and so is all zeroes.  That is
	 * what the reference tool writes, and it is not what -modificationtime
	 * was given. */
	if (meta != NULL) {
		struct tu_time ct;
		char buf[32];
		const char *shown = NULL;

		if (meta->creationtime != NULL &&
		    tu_parse_timestamp(meta->creationtime, &ct)) {
			snprintf(buf, sizeof(buf), "%04ld-%02d-%02dT%02d:%02d:%02dZ",
			    ct.year, ct.month, ct.day, ct.hour, ct.minute,
			    ct.second);
			shown = buf;
		}
		if (meta->creationtime != NULL)
			emit_meta(&s, "CreationTime",
			    shown != NULL ? shown : "0000-00-00T00:00:00Z");
		if (meta->modificationtime != NULL)
			emit_meta(&s, "ModificationTime",
			    shown != NULL ? shown : "0000-00-00T00:00:00Z");
	}
	sink_str(&s, head_after_title);
	for (int c = 1; c <= ncls; c++) {
		int fmt = cls[0] == c ? 0 : 1;

		sink_str(&s, "    p.p");
		sink_put(&s, (const char *)&(char){ (char)('0' + c) }, 1);
		sink_str(&s, " {margin: 0.0px 0.0px 0.0px 0.0px;"
		    " font: 12.0px 'Helvetica Light'");
		/* A line with nothing on it still has to take up room. */
		if (fmt == 1)
			sink_str(&s, "; min-height: 14.0px");
		sink_str(&s, "}\n");
	}
	/* A level is only preserved if the stylesheet says so.  The classes are
	 * in the order the document first needed them, like the paragraphs'. */
	for (int c = 1; c <= embed.ncls; c++) {
		for (int k = 0; k < 4; k++) {
			if (embed.cls[k] != c)
				continue;
			sink_str(&s, "    span.s");
			sink_put(&s, (const char *)&(char){ (char)('0' + c) }, 1);
			sink_str(&s, " {");
			sink_str(&s, tu_bidi_css(embed_openers[k]));
			sink_str(&s, "}\n");
		}
	}
	/* A tab is only preserved if the stylesheet says so. */
	if (used_tab)
		sink_str(&s, "    span.Apple-tab-span {white-space:pre}\n");
	sink_str(&s, head_after_style);
	sink_put(&s, body.buf != NULL ? body.buf : "", body.len);
	sink_str(&s, tail);
	free(embed.stack);
	free(embed.open);
	free(body.buf);

	if (s.failed) {
		free(s.buf);
		return -1;
	}

	/* The document is held whole, so -encoding is applied to it as one
	 * piece: a wide encoding gets a mark and wide units, and the head and
	 * tail markup travels with it. */
	if (tu_encode_bytes(s.buf, s.len, enc, &outbuf, &lenout) != 0) {
		free(s.buf);
		return -1;
	}
	free(s.buf);

	*out = outbuf;
	*outlen = lenout;
	return 0;
}

int
tu_write_html(const tu_doc_t *d, const char *path, const tu_style_t *st,
    const tu_meta_t *meta, tu_encoding_t enc)
{
	char *outbuf = NULL;
	size_t outlen = 0;
	FILE *fp;

	if (tu_html_build(d, st, meta, enc, &outbuf, &outlen) != 0) {
		tu_write_failed(path);
		return -1;
	}
	if ((fp = fopen(path, "wb")) == NULL) {
		free(outbuf);
		tu_write_failed(path);
		return -1;
	}
	if (fwrite(outbuf, 1, outlen, fp) != outlen) {
		fclose(fp);
		free(outbuf);
		tu_write_failed(path);
		return -1;
	}
	if (fclose(fp) != 0) {
		free(outbuf);
		tu_write_failed(path);
		return -1;
	}
	free(outbuf);
	return 0;
}

/* ------------------------------------------------------------------------
 * The reader.
 *
 * -info is the one command that has to read HTML back, and all it wants from
 * it is the text and the metadata.  The tags that carry formatting are passed
 * over, and so are the elements whose contents are text for somebody else to
 * read, which is why a script's body and a style sheet's do not become
 * document text.
 *
 * The layout is the one the reference tool produces.  An element that starts a
 * paragraph breaks the line before it, but only when something has been
 * written since the last break, so an empty document does not open with a
 * blank line; an element that ends a paragraph always breaks.  A <br> breaks
 * at once.  A run of spaces becomes one space, dropped at the start of a line
 * and at the ends of the document.  Inside <pre> the spaces are kept as they
 * lie, and an image is not turned into an attachment.  The body bytes are code
 * page 1252, the same page RTF's hexadecimal escapes are read with, and not
 * UTF-8, so a byte that is the start of a UTF-8 character is two characters
 * here rather than one.
 *
 * A list is laid out with a tab, a mark and a tab in front of every item, and
 * a nested one with a mark of its own; what is written here is one tab, a
 * bullet and a tab, so the length of a document with a nested list in it is
 * short by that much.  See src/textutil/NOTES.md.
 *
 * Copyright (C) 2026, LibreDarwin
 * SPDX-License-Identifier: BSD-3-Clause
 */

struct html {
	const char *p, *end;
	struct sink text;
	size_t nchars;
	tu_meta_t *m;
	/* The name and the value of the meta tag being read.  They may come
	 * in either order, so both are kept until the tag ends. */
	char *mname;
	size_t mnamelen, mcap;
	char *mval;
	size_t mvallen, mvcap;
	int cname;			/* the last attribute was a name */
	int skip;			/* inside head, script or style */
	int pre;				/* inside pre or textarea */
	int space;			/* a run of spaces is being held */
	int brk;			/* a <br> has ended the line already */
	int started;			/* text written since the last break */
	int item;			/* the number the next item is */
	int items;			/* the items the list open now has */
	int listdepth;
	int ordered;			/* the list open now counts its items */
};

/* The page's own characters above 0x7f, which the body is read with. */
static unsigned long
html_cp1252(unsigned char b)
{
	static const unsigned long above[] = {
		0x20ac, 0x0081, 0x201a, 0x0192, 0x201e, 0x2026, 0x2020,
		0x2021, 0x02c6, 0x2030, 0x0160, 0x2039, 0x0152, 0x008d,
		0x017d, 0x008f, 0x0090, 0x2018, 0x2019, 0x201c, 0x201d,
		0x2022, 0x2013, 0x2014, 0x02dc, 0x2122, 0x0161, 0x203a,
		0x0153, 0x009d, 0x017e, 0x0178
	};

	if (b < 0xa0)
		return b;
	return above[b - 0xa0];
}

/* A paragraph ends with a line separator rather than a newline, which is how
 * a break inside a paragraph is written. */
#define HTML_BREAK 0x2028

/* One character of text, counted the way -info counts. */
static void
html_cp(struct html *h, unsigned long cp)
{
	char b[4];
	size_t n;

	if (cp == 0 || cp > 0x10ffff)
		return;
	if (cp < 0x80) {
		b[0] = (char)cp;
		n = 1;
	} else if (cp < 0x800) {
		b[0] = (char)(0xc0 | (cp >> 6));
		b[1] = (char)(0x80 | (cp & 0x3f));
		n = 2;
	} else if (cp < 0x10000) {
		b[0] = (char)(0xe0 | (cp >> 12));
		b[1] = (char)(0x80 | ((cp >> 6) & 0x3f));
		b[2] = (char)(0x80 | (cp & 0x3f));
		n = 3;
	} else {
		b[0] = (char)(0xf0 | (cp >> 18));
		b[1] = (char)(0x80 | ((cp >> 12) & 0x3f));
		b[2] = (char)(0x80 | ((cp >> 6) & 0x3f));
		b[3] = (char)(0x80 | (cp & 0x3f));
		n = 4;
	}
	if (cp != HTML_BREAK)
		h->brk = 0;
	sink_put(&h->text, b, n);
	/* -info counts a character outside the basic plane as two, which is
	 * how many it takes to write one in UTF-16. */
	h->nchars += cp > 0xffff ? 2 : 1;
	h->started = 1;
}

/* The end of a line.  Nothing written since the last break means there is
 * nothing to break away from. */
static void
html_break(struct html *h)
{
	if (h->started)
		html_cp(h, HTML_BREAK);
	h->space = 0;
	h->started = 0;
}

/* The end of a paragraph, which is a break whether or not there was a line to
 * break. */
static void
html_endpara(struct html *h)
{
	/* A <br> has ended the line already, and a paragraph that ends after
	 * one is the paragraph that <br> ended. */
	if (!h->brk)
		html_cp(h, HTML_BREAK);
	h->brk = 0;
	h->space = 0;
	h->started = 0;
}

/* The spaces of a run, which are one space, held until something shows whether
 * there is one worth keeping. */
static void
html_space(struct html *h)
{
	if (h->pre || !h->started)
		return;
	h->space = 1;
}

static void
html_flush_space(struct html *h)
{
	if (h->space) {
		html_cp(h, ' ');
		h->space = 0;
	}
}

/* Is this one of the spaces a browser folds together? */
static int
html_iswhite(unsigned char c)
{
	return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\f' ||
	    c == '\v';
}

/* What the name of a reference stands for, the name being without its & and ;
 * or 0 for a name the page leaves undefined, which is then left as it lies. */
static unsigned long
html_entity(const char *p, size_t len)
{
	static const struct {
		const char *name;
		unsigned long cp;
	} named[] = {
		{ "quot", 0x0022 }, { "amp", 0x0026 }, { "apos", 0x0027 },
		{ "lt", 0x003c }, { "gt", 0x003e }, { "nbsp", 0x00a0 },
		{ "iexcl", 0x00a1 }, { "cent", 0x00a2 }, { "pound", 0x00a3 },
		{ "curren", 0x00a4 }, { "yen", 0x00a5 }, { "brvbar", 0x00a6 },
		{ "sect", 0x00a7 }, { "uml", 0x00a8 }, { "copy", 0x00a9 },
		{ "ordf", 0x00aa }, { "laquo", 0x00ab }, { "not", 0x00ac },
		{ "reg", 0x00ae }, { "macr", 0x00af }, { "deg", 0x00b0 },
		{ "plusmn", 0x00b1 }, { "sup2", 0x00b2 }, { "sup3", 0x00b3 },
		{ "acute", 0x00b4 }, { "micro", 0x00b5 }, { "para", 0x00b6 },
		{ "middot", 0x00b7 }, { "cedil", 0x00b8 }, { "sup1", 0x00b9 },
		{ "ordm", 0x00ba }, { "raquo", 0x00bb }, { "frac14", 0x00bc },
		{ "frac12", 0x00bd }, { "frac34", 0x00be }, { "iquest", 0x00bf },
		{ "Agrave", 0x00c0 }, { "Aacute", 0x00c1 }, { "Acirc", 0x00c2 },
		{ "Atilde", 0x00c3 }, { "Auml", 0x00c4 }, { "Aring", 0x00c5 },
		{ "AElig", 0x00c6 }, { "Ccedil", 0x00c7 }, { "Egrave", 0x00c8 },
		{ "Eacute", 0x00c9 }, { "Ecirc", 0x00ca }, { "Euml", 0x00cb },
		{ "Igrave", 0x00cc }, { "Iacute", 0x00cd }, { "Icirc", 0x00ce },
		{ "Iuml", 0x00cf }, { "ETH", 0x00d0 }, { "Ntilde", 0x00d1 },
		{ "Ograve", 0x00d2 }, { "Oacute", 0x00d3 }, { "Ocirc", 0x00d4 },
		{ "Otilde", 0x00d5 }, { "Ouml", 0x00d6 }, { "times", 0x00d7 },
		{ "Oslash", 0x00d8 }, { "Ugrave", 0x00d9 }, { "Uacute", 0x00da },
		{ "Ucirc", 0x00db }, { "Uuml", 0x00dc }, { "Yacute", 0x00dd },
		{ "THORN", 0x00de }, { "szlig", 0x00df }, { "agrave", 0x00e0 },
		{ "aacute", 0x00e1 }, { "acirc", 0x00e2 }, { "atilde", 0x00e3 },
		{ "auml", 0x00e4 }, { "aring", 0x00e5 }, { "aelig", 0x00e6 },
		{ "ccedil", 0x00e7 }, { "egrave", 0x00e8 }, { "eacute", 0x00e9 },
		{ "ecirc", 0x00ea }, { "euml", 0x00eb }, { "igrave", 0x00ec },
		{ "iacute", 0x00ed }, { "icirc", 0x00ee }, { "iuml", 0x00ef },
		{ "eth", 0x00f0 }, { "ntilde", 0x00f1 }, { "ograve", 0x00f2 },
		{ "oacute", 0x00f3 }, { "ocirc", 0x00f4 }, { "otilde", 0x00f5 },
		{ "ouml", 0x00f6 }, { "divide", 0x00f7 }, { "oslash", 0x00f8 },
		{ "ugrave", 0x00f9 }, { "uacute", 0x00fa }, { "ucirc", 0x00fb },
		{ "uuml", 0x00fc }, { "yacute", 0x00fd }, { "thorn", 0x00fe },
		{ "yuml", 0x00ff }, { "OElig", 0x0152 }, { "oelig", 0x0153 },
		{ "Scaron", 0x0160 }, { "scaron", 0x0161 }, { "Yuml", 0x0178 },
		{ "fnof", 0x0192 }, { "circ", 0x02c6 }, { "tilde", 0x02dc },
		{ "ensp", 0x2002 }, { "emsp", 0x2003 }, { "thinsp", 0x2009 },
		{ "zwnj", 0x200c }, { "zwj", 0x200d }, { "lrm", 0x200e },
		{ "rlm", 0x200f }, { "ndash", 0x2013 }, { "mdash", 0x2014 },
		{ "lsquo", 0x2018 }, { "rsquo", 0x2019 }, { "sbquo", 0x201a },
		{ "ldquo", 0x201c }, { "rdquo", 0x201d }, { "bdquo", 0x201e },
		{ "dagger", 0x2020 }, { "Dagger", 0x2021 }, { "bull", 0x2022 },
		{ "hellip", 0x2026 }, { "permil", 0x2030 }, { "prime", 0x2032 },
		{ "Prime", 0x2033 }, { "lsaquo", 0x2039 }, { "rsaquo", 0x203a },
		{ "oline", 0x203e }, { "frasl", 0x2044 }, { "euro", 0x20ac },
		{ "trade", 0x2122 }, { "larr", 0x2190 }, { "uarr", 0x2191 },
		{ "rarr", 0x2192 }, { "darr", 0x2193 }, { "harr", 0x2194 },
		{ "crarr", 0x21b5 }, { "lArr", 0x21d0 }, { "uArr", 0x21d1 },
		{ "rArr", 0x21d2 }, { "dArr", 0x21d3 }, { "hArr", 0x21d4 },
		{ "forall", 0x2200 }, { "part", 0x2202 }, { "exist", 0x2203 },
		{ "empty", 0x2205 }, { "nabla", 0x2207 }, { "isin", 0x2208 },
		{ "notin", 0x2209 }, { "ni", 0x220b }, { "prod", 0x220f },
		{ "sum", 0x2211 }, { "minus", 0x2212 }, { "lowast", 0x2217 },
		{ "radic", 0x221a }, { "prop", 0x221d }, { "infin", 0x221e },
		{ "ang", 0x2220 }, { "and", 0x2227 }, { "or", 0x2228 },
		{ "cap", 0x2229 }, { "cup", 0x222a }, { "int", 0x222b },
		{ "there4", 0x2234 }, { "sim", 0x223c }, { "cong", 0x2245 },
		{ "asymp", 0x2248 }, { "ne", 0x2260 }, { "equiv", 0x2261 },
		{ "le", 0x2264 }, { "ge", 0x2265 }, { "sub", 0x2282 },
		{ "sup", 0x2283 }, { "nsub", 0x2284 }, { "sube", 0x2286 },
		{ "supe", 0x2287 }, { "oplus", 0x2295 }, { "otimes", 0x2297 },
		{ "perp", 0x22a5 }, { "sdot", 0x22c5 }, { "lceil", 0x2308 },
		{ "rceil", 0x2309 }, { "lfloor", 0x230a }, { "rfloor", 0x230b },
		{ "lang", 0x2329 }, { "rang", 0x232a }, { "loz", 0x25ca },
		{ "spades", 0x2660 }, { "clubs", 0x2663 }, { "hearts", 0x2665 },
		{ "diams", 0x2666 }
	};
	size_t i;

	if (len > 1 && p[0] == '#') {
		unsigned long v = 0;
		size_t k = 1;
		int any = 0, hex = len > 2 && (p[1] == 'x' || p[1] == 'X');

		if (hex)
			k = 2;
		for (; k < len; k++) {
			int d;

			if (p[k] >= '0' && p[k] <= '9')
				d = p[k] - '0';
			else if (hex && p[k] >= 'a' && p[k] <= 'f')
				d = p[k] - 'a' + 10;
			else if (hex && p[k] >= 'A' && p[k] <= 'F')
				d = p[k] - 'A' + 10;
			else
				return 0;
			if (v < 0x200000)
				v = v * (hex ? 16 : 10) + (unsigned long)d;
			any = 1;
		}
		return any ? v : 0;
	}
	for (i = 0; i < sizeof named / sizeof *named; i++)
		if (strlen(named[i].name) == len &&
		    memcmp(named[i].name, p, len) == 0)
			return named[i].cp;
	return 0;
}

/* A run of bytes with nothing in it that needs looking at. */
static void
html_raw(struct html *h, const char *p, size_t len)
{
	size_t i;

	for (i = 0; i < len; i++) {
		unsigned long cp = html_cp1252((unsigned char)p[i]);

		html_flush_space(h);
		html_cp(h, cp);
	}
}

/* Text, with the references turned into the characters they stand for and the
 * runs of spaces folded into one. */
static void
html_text(struct html *h, const char *p, size_t len)
{
	size_t i, run = 0;

	for (i = 0; i < len; i++) {
		unsigned char c = (unsigned char)p[i];

		if (c == '&') {
			const char *semi = memchr(p + i, ';', len - i);
			size_t n;

			if (semi != NULL && (n = (size_t)(semi - (p + i))) >= 2 &&
			    n <= 11) {
				unsigned long cp = html_entity(p + i + 1, n - 1);

				if (cp != 0) {
					if (run != 0) {
						html_raw(h, p + i - run, run);
						run = 0;
					}
					html_flush_space(h);
					html_cp(h, cp);
					i += n;
					continue;
				}
			}
		} else if (html_iswhite(c) && !h->pre) {
			if (run != 0) {
				html_raw(h, p + i - run, run);
				run = 0;
			}
			html_space(h);
			continue;
		}
		run++;
	}
	if (run != 0)
		html_raw(h, p + len - run, run);
}

/* The elements that start and end a paragraph.  The ones that only hold other
 * paragraphs, such as a table or a list, are not among them: what they hold
 * breaks the line by itself. */
static int
html_isblock(const char *n, size_t len)
{
	static const char *const names[] = {
		"p", "div", "h1", "h2", "h3", "h4", "h5", "h6", "blockquote",
		"pre", "li", "dt", "dd", "td", "th", "form", "fieldset",
		"address", "center", "dir", "menu", "hr", "article", "aside",
		"footer", "header", "main", "nav", "section", "figure",
		"figcaption", "noscript", "caption"
	};
	size_t i;

	for (i = 0; i < sizeof names / sizeof *names; i++)
		if (strlen(names[i]) == len && memcmp(names[i], n, len) == 0)
			return 1;
	return 0;
}

/* The elements whose contents are not document text.  A <meta> is not one of
 * them: it has no body, and the ones inside a head are where the metadata of
 * the page is written, which is exactly what is wanted here. */
static int
html_isskip(const char *n, size_t len)
{
	static const char *const names[] = {
		"script", "style", "title"
	};
	size_t i;

	for (i = 0; i < sizeof names / sizeof *names; i++)
		if (strlen(names[i]) == len && memcmp(names[i], n, len) == 0)
			return 1;
	return 0;
}

/* Does this begin a tag? */
static int
html_isname(unsigned char c)
{
	return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '/' ||
	    c == '!' || c == '?';
}

/* A tag name, compared without regard to case. */
static int
html_is(const char *n, size_t len, const char *want)
{
	size_t i;

	if (strlen(want) != len)
		return 0;
	for (i = 0; i < len; i++) {
		int a = n[i], b = want[i];

		if (a >= 'A' && a <= 'Z')
			a += 'a' - 'A';
		if (b >= 'A' && b <= 'Z')
			b += 'a' - 'A';
		if (a != b)
			return 0;
	}
	return 1;
}

/* Copy a name or a value into one of the two scratch buffers, growing it. */
static int
html_save(char **buf, size_t *len, size_t *cap, const char *p, size_t n)
{
	if (*len < n + 1) {
		size_t want = n + 64;
		char *b = realloc(*buf, want);

		if (b == NULL)
			return 0;
		*buf = b;
		*cap = want;
	}
	memcpy(*buf, p, n);
	(*buf)[n] = '\0';
	*len = n + 1;
	return 1;
}

/* A <meta> tag, whose name and content are metadata.  Either attribute may
 * come first, so both are collected and placed when the tag ends.  A name with
 * no content is metadata whose value is empty, which -info prints as a line
 * with nothing after the colon. */
static void
html_meta(struct html *h)
{
	const char *name = h->mname, *val = h->mval;
	size_t nlen = h->mnamelen - 1, vlen = h->mvallen - 1;
	const char **slot = NULL;
	int isdate = 0;

	h->mnamelen = 0;
	h->mvallen = 0;
	if (h->m == NULL || name == NULL || val == NULL)
		return;
	/* The name is what says which field this is, and the reference tool
	 * takes the two attributes in either order.  A name it does not
	 * report, such as Generator, is read and then left out. */
	if (html_is(name, nlen, "author"))
		slot = &h->m->author;
	else if (html_is(name, nlen, "lastauthor"))
		slot = &h->m->editor;
	else if (html_is(name, nlen, "company"))
		slot = &h->m->company;
	else if (html_is(name, nlen, "subject"))
		slot = &h->m->subject;
	else if (html_is(name, nlen, "keywords"))
		slot = &h->m->keywords;
	else if (html_is(name, nlen, "description"))
		slot = &h->m->comment;
	else if (html_is(name, nlen, "creationtime"))
		slot = &h->m->creationtime, isdate = 1;
	else if (html_is(name, nlen, "modificationtime"))
		slot = &h->m->modificationtime, isdate = 1;
	if (slot == NULL)
		return;
	/* A time is kept as the broken-down form the option parser uses,
	 * which is the form a Cocoa HTML page writes, and -info turns it
	 * into the form it prints.  A value too short to hold one is not a
	 * time, and is left out rather than shown as it lies. */
	if (isdate) {
		struct tu_time t;
		char buf[32];

		if (vlen < 20)
			return;
		{
			char in[24];

			memcpy(in, val, 20);
			in[20] = '\0';
			if (!tu_parse_timestamp(in, &t))
				return;
		}
		snprintf(buf, sizeof(buf), "%04ld-%02d-%02dT%02d:%02d:%02dZ",
		    t.year, t.month, t.day, t.hour, t.minute, t.second);
		free((char *)*slot);
		*slot = strdup(buf);
		return;
	}
	free((char *)*slot);
	*slot = strdup(val);
}

/* The attributes of a tag, and what the tag itself does. */
static void
html_tag(struct html *h, const char *p, const char *end, int closing)
{
	const char *name = p, *attrs;
	size_t namelen, i;
	int ismeta = 0, selfclose = 0;

	while (name < end && !html_iswhite((unsigned char)*name) &&
	    *name != '/' && *name != '>')
		name++;
	namelen = (size_t)(name - p);
	if (namelen == 0)
		return;
	attrs = name;
	if (html_is(p, namelen, "meta"))
		ismeta = 1;
	if (h->mname != NULL || h->mval != NULL) {
		h->mnamelen = 0;
		h->mvallen = 0;
	}
	/* The attributes, which is where the metadata of a meta tag is. */
	if (ismeta && h->m != NULL) {
		const char *q = attrs;

		while (q < end) {
			const char *an, *av;
			size_t anlen, avlen;

			while (q < end && html_iswhite((unsigned char)*q))
				q++;
			if (q >= end || *q == '/')
				break;
			an = q;
			while (q < end && !html_iswhite((unsigned char)*q) &&
			    *q != '=' && *q != '/')
				q++;
			anlen = (size_t)(q - an);
			av = NULL;
			avlen = 0;
			if (q < end && *q == '=') {
				char quote;

				q++;
				if (q < end && (*q == '"' || *q == '\'')) {
					quote = *q++;
					av = q;
					while (q < end && *q != quote)
						q++;
					avlen = (size_t)(q - av);
					if (q < end)
						q++;
				} else {
					av = q;
					while (q < end && !html_iswhite(
					    (unsigned char)*q) && *q != '/')
						q++;
					avlen = (size_t)(q - av);
				}
			}
			/* A meta tag names a field and gives its value, and
			 * the two attributes may come in either order, so
			 * they are picked out by what they are called
			 * rather than by where they lie. */
			if (anlen != 0 && (html_is(an, anlen, "name") ||
			    html_is(an, anlen, "http-equiv")) &&
			    !html_save(&h->mname, &h->mnamelen, &h->mcap,
			    av != NULL ? av : "", av != NULL ? avlen : 0))
				return;
			if (anlen != 0 && html_is(an, anlen, "content") &&
			    !html_save(&h->mval, &h->mvallen, &h->mvcap,
			    av != NULL ? av : "", av != NULL ? avlen : 0))
				return;
		}
	}
	/* An empty element is one that ends in a slash, which is not the
	 * same as a slash anywhere in the tag: a value may hold one. */
	for (i = (size_t)(end - p); i > 0; i--)
		if (!html_iswhite((unsigned char)p[i - 1])) {
			selfclose = p[i - 1] == '/';
			break;
		}
	if (ismeta && !closing)
		html_meta(h);
	/* Inside a part that is passed over only the end of it is of any
	 * interest, and the metadata of a head is read wherever it lies. */
	if (h->skip != 0 && !ismeta) {
		if (html_isskip(p, namelen) && closing && h->skip > 0)
			h->skip--;
		else if (html_isskip(p, namelen) && !closing)
			h->skip++;
		return;
	}
	/* The elements that have no end of their own, and so are taken as
	 * they open: a <br> breaks the line, an <hr> breaks it twice, and
	 * an <img> is an attachment in the text. */
	if (!closing && (html_is(p, namelen, "br") ||
	    html_is(p, namelen, "hr") || html_is(p, namelen, "img") ||
	    html_is(p, namelen, "input") || selfclose)) {
		if (html_is(p, namelen, "br")) {
			html_flush_space(h);
			html_cp(h, HTML_BREAK);
			h->brk = 1;
			h->started = 0;
		} else if (html_is(p, namelen, "hr")) {
			html_break(h);
			html_cp(h, HTML_BREAK);
		} else if (html_is(p, namelen, "img") ||
		    html_is(p, namelen, "input"))
			html_cp(h, 0xfffc);
		return;
	}
	if (closing) {
		if (html_isskip(p, namelen)) {
			if (h->skip > 0)
				h->skip--;
		} else if (html_is(p, namelen, "pre") ||
		    html_is(p, namelen, "textarea")) {
			h->pre = 0;
			if (html_is(p, namelen, "pre"))
				html_endpara(h);
		}
		else if (html_is(p, namelen, "q"))
			html_cp(h, 0x201d);
		else if (html_is(p, namelen, "li") || html_is(p, namelen, "dd") ||
		    html_is(p, namelen, "dt"))
			html_endpara(h);
		else if (html_is(p, namelen, "ul") || html_is(p, namelen, "ol")) {
			/* A list with nothing in it is laid out as a
			 * mark and nothing else, which is one more than
			 * leaving it out would be. */
			if (h->items == 0) {
				html_break(h);
				html_cp(h, '\t');
				if (h->ordered) {
					char num[16];

					snprintf(num, sizeof(num), "%d",
					    h->item);
					html_raw(h, num, strlen(num));
				} else
					html_cp(h, 0x2022);
				html_cp(h, '\t');
				h->started = 0;
				html_endpara(h);
			} else
				html_break(h);
			if (h->listdepth > 0)
				h->listdepth--;
		} else if (html_isblock(p, namelen))
			html_endpara(h);
		return;
	}
	if (html_isskip(p, namelen)) {
		h->skip++;
		return;
	}
	if (html_is(p, namelen, "pre") || html_is(p, namelen, "textarea"))
		h->pre = 1;
	if (html_is(p, namelen, "q")) {
		html_cp(h, 0x201c);
		return;
	}
	if (html_is(p, namelen, "ul") || html_is(p, namelen, "ol")) {
		html_break(h);
		if (h->listdepth++ == 0) {
			h->item = 1;
			h->items = 0;
			h->ordered = html_is(p, namelen, "ol");
		}
		return;
	}
	if (html_is(p, namelen, "li") || html_is(p, namelen, "dd") ||
	    html_is(p, namelen, "dt")) {
		html_break(h);
		h->items++;
		if (h->listdepth > 0) {
			/* A tab, a mark and a tab, then the item.  A
			 * mark is a bullet, or the item's number in a
			 * list that counts.  What the mark is written as
			 * is not something written, so a break asked for
			 * next does not fall between it and the item. */
			html_cp(h, '\t');
			if (h->ordered) {
				char num[16];

				snprintf(num, sizeof(num), "%d", h->item++);
				html_raw(h, num, strlen(num));
			} else
				html_cp(h, 0x2022);
			html_cp(h, '\t');
			h->started = 0;
		}
		return;
	}
	if (html_isblock(p, namelen))
		html_break(h);
}

static void
html_run(struct html *h, const char *p, const char *end)
{
	h->p = p;
	h->end = end;
	h->item = 1;
	while (p < end) {
		if (*p == '<' && p + 1 < end && html_isname((unsigned char)p[1])) {
			const char *q = p + 1;
			int closing = 0;

			if (q < end && *q == '!') {
				/* A comment, a declaration or a processing
				 * instruction: none of it is text. */
				if (end - q >= 3 && q[1] == '-' && q[2] == '-') {
					const char *stop = q + 3;

					while (stop + 2 < end &&
					    !(stop[0] == '-' && stop[1] == '-' &&
					    stop[2] == '>'))
						stop++;
					p = stop + 2 < end ? stop + 3 : end;
				} else {
					while (q < end && *q != '>')
						q++;
					p = q < end ? q + 1 : end;
				}
				continue;
			}
			if (q < end && *q == '?') {
				while (q < end && *q != '>')
					q++;
				p = q < end ? q + 1 : end;
				continue;
			}
			if (q < end && *q == '/') {
				closing = 1;
				q++;
			}
			{
				/* A > inside a quoted value does not end the
				 * tag, and the name itself ends at the first
				 * space. */
				const char *name = q;
				int empty = 0;
				char quote = 0;

				while (q < end) {
					if (quote != 0) {
						if (*q == quote)
							quote = 0;
					} else if (*q == '"' || *q == '\'')
						quote = *q;
					else if (*q == '>')
						break;
					q++;
				}
				if (q >= end)
					empty = 1;
				else if (q > name && q[-1] == '/')
					empty = 1;
				if (!empty)
					html_tag(h, name, q, closing);
				p = q < end ? q + 1 : end;
			}
			continue;
		}
		{
			const char *q = p;
			size_t len;

			while (q < end && !(*q == '<' && q + 1 < end &&
			    html_isname((unsigned char)q[1])))
				q++;
			len = (size_t)(q - p);
			if (h->skip == 0) {
				if (h->pre)
					html_raw(h, p, len);
				else
					html_text(h, p, len);
			}
			p = q;
		}
	}
}

/* Read an HTML file into its text and the metadata -info reports.  A NULL out
 * is not filled in.  Returns 0 on success. */
int
tu_read_html(const void *buf, size_t len, tu_doc_t *out, tu_meta_t *meta)
{
	struct html h;

	memset(&h, 0, sizeof(h));
	h.m = meta;
	html_run(&h, buf, (const char *)buf + len);
	if (h.text.buf == NULL)
		sink_put(&h.text, "", 0);
	if (h.text.failed) {
		free(h.text.buf);
		free(h.mname);
		free(h.mval);
		/* A read that fails leaves out empty, for the same reason and
		 * with the same rule as tu_read_rtf. */
		if (out != NULL) {
			out->text = NULL;
			out->len = 0;
			out->nchars = 0;
		}
		return TU_READ_UNOPENABLE;
	}
	if (out != NULL) {
		out->text = h.text.buf;
		out->len = h.text.len;
		out->nchars = h.nchars;
	} else
		free(h.text.buf);
	free(h.mname);
	free(h.mval);
	return 0;
}
