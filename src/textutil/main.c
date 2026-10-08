/*
 * textutil - convert between plain text and rich text file formats.
 *
 * Argument handling follows the reference tool closely, including the parts
 * that are easy to get wrong:
 *
 *   - An argument that is not a recognised option, and starts with a dash,
 *     prints the usage and exits 0.  It is not an error.
 *   - A second command option is an error, and so is an option whose value is
 *     missing, but each reports its own message and exits 1.
 *   - "-" is an ordinary file name.  Reading standard input has to be asked
 *     for with -stdin.
 *   - A file that cannot be read is reported and skipped, and does not by
 *     itself make textutil fail; the exit status is still 0.
 *
 * Output naming: without -output, each output is written beside its input with
 * the extension replaced, so a.txt converted to rtf is a.rtf.  -output names
 * the first output only, and -stdout sends it to standard output.  -cat has
 * one output, which defaults to out with the format's extension.
 *
 * Copyright (C) 2026, LibreDarwin
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <unistd.h>

#include "textutil.h"

struct opt {
	const char *name;
	int arity;
	int is_command;
	const char *missing;
};

static const struct opt options[] = {
	{ "help",               0, 1, NULL },
	{ "info",               0, 1, NULL },
	{ "convert",            1, 1, "No output format specified." },
	{ "cat",                1, 1, "No output format specified." },
	{ "extension",          1, 0, "No extension specified." },
	{ "output",             1, 0, "No output file name specified." },
	{ "stdin",              0, 0, NULL },
	{ "stdout",             0, 0, NULL },
	{ "encoding",           1, 0, "No output encoding specified." },
	{ "inputencoding",      1, 0, "No input encoding specified." },
	{ "format",             1, 0, "No input format specified." },
	{ "font",               1, 0, "No font specified." },
	{ "fontsize",           1, 0, "No font size specified." },
	{ "noload",             0, 0, NULL },
	{ "nostore",            0, 0, NULL },
	{ "baseurl",            1, 0, "No base URL specified." },
	{ "timeout",            1, 0, "No timeout value specified." },
	{ "textsizemultiplier", 1, 0, "No multiplier value specified." },
	{ "excludedelements",   1, 0, "No excluded elements list specified." },
	{ "prefixspaces",       1, 0, "No prefix value specified." },
	{ "strip",              0, 0, NULL },
	{ "title",              1, 0, "No title specified." },
	{ "author",             1, 0, "No author specified." },
	{ "subject",            1, 0, "No subject specified." },
	{ "keywords",           1, 0, "No keyword list specified." },
	{ "comment",            1, 0, "No comment specified." },
	{ "editor",             1, 0, "No editor specified." },
	{ "company",            1, 0, "No company specified." },
	{ "creationtime",       1, 0, "No creation time specified." },
	{ "modificationtime",   1, 0, "No modification time specified." },
};

/* The -keywords argument, which the reference tool hands to an old style
 * property list parser and takes a list from: ("one", "two"), or (one,two), or
 * (one).  Whitespace around the list and around each item is passed over, the
 * items are written back out joined with a comma and a space, and an item of its
 * own may hold a comma or a space as long as it is quoted.  A quoted item
 * takes a backslash in front of any character, and the backslash is dropped.
 *
 * Nothing is returned on a failure.  What the reference tool does with a
 * malformed list is not worth copying: a bare word is refused politely, and
 * anything it cannot read -- a brace, a square bracket pair, a semicolon, an
 * equals sign, a space inside a bare item, a non-ASCII character, or a list
 * with an empty item in it -- takes the tool down with an uncaught exception,
 * and one case is stranger still, an item quoted with a byte outside ASCII in
 * it, which comes back out as that byte written in hex.  So a list this cannot
 * read is refused the way a bare word is, which is the one diagnostic the
 * reference tool is willing to give. */
static char *
keyword_list(const char *v)
{
	const char *end, *p;
	char *out, *w;
	size_t n;

	while (*v == ' ' || *v == '\t')
		v++;
	end = v + strlen(v);
	while (end > v && (end[-1] == ' ' || end[-1] == '\t'))
		end--;
	n = (size_t)(end - v);
	if (n < 2 || v[0] != '(' || v[n - 1] != ')')
		return NULL;
	out = malloc(n * 4 + 1);
	if (out == NULL)
		return NULL;
	w = out;
	p = v + 1;
	for (;;) {
		while (*p == ' ' || *p == '\t')
			p++;
		if (p[0] == ',' || p[0] == ')') {
			/* An item that is not there at all. */
			free(out);
			return NULL;
		}
		if (*p == '"') {
			for (p++; *p != '\0' && *p != '"'; p++) {
				if ((unsigned char)*p >= 0x80) {
					free(out);
					return NULL;
				}
				if (*p != '\\' || p[1] == '\0') {
					*w++ = *p;
					continue;
				}
				p++;
				/* The three the parser spells out are the ones
				 * that mean something; any other backslash
				 * is dropped and the character kept. */
				if (*p == 'n')
					*w++ = '\n';
				else if (*p == 't')
					*w++ = '\t';
				else if (*p == 'r')
					*w++ = '\r';
				else
					*w++ = *p;
			}
			if (*p != '"') {
				free(out);
				return NULL;
			}
			p++;
		} else {
			for (; *p != '\0' && *p != ',' && *p != ')' &&
			    *p != '"' && *p != ';' && *p != '=' &&
			    *p != ' ' && *p != '\t'; p++)
				*w++ = *p;
			if (p[0] == '"' || p[0] == ';' || p[0] == '=' ||
			    p[0] == ' ' || p[0] == '\t') {
				free(out);
				return NULL;
			}
		}
		while (*p == ' ' || *p == '\t')
			p++;
		if (p[0] != ',')
			break;
		p++;
		while (*p == ' ' || *p == '\t')
			p++;
		if (p[0] == ')')
			break;		/* a comma before the end is allowed */
		*w++ = ',';
		*w++ = ' ';
	}
	/* The list ends at the argument's own closing bracket. */
	if (p != end - 1) {
		free(out);
		return NULL;
	}
	*w = '\0';
	return out;
}

static void
print_usage(void)
{
	for (int i = 0; tu_usage_lines[i] != NULL; i++)
		printf("%s\n", tu_usage_lines[i]);
}

/* An empty value is refused for these options, and for each one the diagnostic
 * is its own rather than the one given for a missing argument.  -title,
 * -inputencoding and -baseurl are the exceptions: an empty value is a
 * legitimate one, and the reference tool accepts it.  An option named in
 * neither list accepts an empty value too, which is how -excludedelements and
 * the metadata options come out of here. */
static const struct {
	const char *name;
	const char *empty;
} empty_values[] = {
	{ "output",		"No output file name specified." },
	{ "extension",		"No extension specified." },
	{ "font",		"No font specified." },
	{ "fontsize",		"Invalid font size." },
	{ "textsizemultiplier", "Invalid multiplier value." },
	{ "prefixspaces",	"Invalid prefix value." },
};

/* The metadata options may each be given once, and a second one is an error
 * rather than a replacement.  Every one of them is named in the diagnostic
 * rather than shared, so the table carries the wording too.  The flags say
 * which of them have been seen, and are filled in as the table is walked. */
static struct {
	const char *name;
	const char *many;
	int seen;
} once_only[] = {
	{ "title",		"Multiple titles specified.",		0 },
	{ "author",		"Multiple authors specified.",		0 },
	{ "subject",		"Multiple subjects specified.",		0 },
	{ "keywords",		"Multiple keyword lists specified.",	0 },
	{ "comment",		"Multiple comments specified.",		0 },
	{ "editor",		"Multiple editors specified.",		0 },
	{ "company",		"Multiple companies specified.",		0 },
	{ "creationtime",	"Multiple creation times specified.",	0 },
	{ "modificationtime",	"Multiple modification times specified.", 0 },
};

/* The run attributes and title that -font, -fontsize and -title select, handed
 * to whichever writer runs. */
static tu_style_t style;
static tu_meta_t meta;
/* -encoding and -format, kept beside the other file-wide options so that the
 * writers can reach them without threading them through every call.  A forced
 * input format of -1 means none was given, in which case the format is taken
 * from the file's own contents. */
static tu_encoding_t enc = TU_ENC_UTF8;
static const char *enc_given = "UTF-8";	/* the spelling -encoding was given */
static int forced = -1;

/* Replace a path's extension, keeping any directory prefix.  ext may be NULL,
 * in which case the format's own extension is used. */
static void
output_name(const char *in, char *buf, size_t buflen, const char *ext)
{
	const char *base = strrchr(in, '/');
	const char *dot;
	size_t dirlen = base != NULL ? (size_t)(base - in) + 1 : 0;
	size_t stem;

	base = base != NULL ? base + 1 : in;
	dot = strrchr(base, '.');
	/* A leading dot is part of the name, not an extension. */
	stem = (dot != NULL && dot != base) ? (size_t)(dot - base) : strlen(base);
	if (dirlen >= buflen)
		dirlen = buflen - 1;
	memcpy(buf, in, dirlen);
	if (dirlen + stem + strlen(ext) + 2 > buflen)
		stem = 0;
	memcpy(buf + dirlen, base, stem);
	snprintf(buf + dirlen + stem, buflen - dirlen - stem, ".%s", ext);
}

/* Write to a named destination.  A bundle is exempt from the check, because
 * its destination is a directory and so is created rather than refused, and
 * because the reference tool makes a missing parent for it.  Standard output
 * arrives here as /dev/stdout, which is not a destination to be checked, so
 * that is exempt as well. */
/* Whether a forced -format is one this port can read.  A forced plain text
 * format is the one it can, and means "read these bytes as text" rather than
 * "guess".  For the rest, returns a TU_READ_ reason so that the caller can use
 * the reference tool's own wording, or TU_READ_ABSENTFORMAT to say plainly
 * that the reader is missing. */
static int
forced_read_group(void)
{
	int group = tu_fmtread_for((tu_fmt_t)forced);

	return group == TU_FMTREAD_ABSENT ? TU_READ_ABSENTFORMAT : group;
}

/* Read one input, honouring a forced -format.  Returns TU_READ_OK when the
 * file has been read, otherwise the reason, without having said anything: the
 * caller reports it, because -cat and -convert differ in what a failure does
 * to the exit status.
 *
 * With nothing forced the file's own name and first bytes choose the reader,
 * which is the same choice -info makes and reports as its Type line.  RTF and
 * HTML are read here; the rich and HTML readers take a buffer rather than a
 * path, so the file is read once and handed over. */
static int
read_input(const char *path, tu_doc_t *d)
{
	struct stat st;
	tu_meta_t found;
	tu_fmt_t fmt;
	FILE *fp;
	char *buf;
	size_t cap, len;
	int why;

	memset(&found, 0, sizeof(found));

	if (forced >= 0) {
		int group = forced_read_group();

		if (group != TU_FMTREAD_TXT)
			return group == TU_FMTREAD_RICH ? TU_READ_UNOPENABLE :
			    group == TU_FMTREAD_PACKAGE ? TU_READ_WRONGFMT :
			    TU_READ_ABSENTFORMAT;
		return tu_read_plain(path, d);
	}
	if (stat(path, &st) != 0)
		return errno == EACCES || errno == EPERM ?
		    TU_READ_DENIED : TU_READ_MISSING;
	if (S_ISDIR(st.st_mode)) {
		const char *dot = strrchr(path, '.');

		if (dot == NULL || strcasecmp(dot, ".rtfd") != 0)
			return TU_READ_UNOPENABLE;
		why = tu_read_rtfd(path, d, &found);
		tu_meta_free(&found);
		return why;
	}
	if ((fp = fopen(path, "rb")) == NULL)
		return errno == EACCES || errno == EPERM ?
		    TU_READ_DENIED : TU_READ_UNOPENABLE;
	/* The whole file is wanted, not a prefix of it, because the reader is
	 * what decides whether the file is RTF or HTML at all, and a mark
	 * followed by a group that closes fifty kilobytes later is RTF while
	 * the same mark with nothing after it is not.  A size is a hint and not
	 * a promise, so the buffer grows if the file is larger than it says. */
	cap = 65536;
	if (S_ISREG(st.st_mode) && st.st_size > 0 &&
	    (size_t)st.st_size + 1 > cap)
		cap = (size_t)st.st_size + 1;
	if ((buf = malloc(cap)) == NULL) {
		fclose(fp);
		return TU_READ_UNOPENABLE;
	}
	len = 0;
	for (;;) {
		size_t got = fread(buf + len, 1, cap - len, fp);

		len += got;
		if (len < cap)
			break;
		{
			char *bigger = realloc(buf, cap * 2);

			if (bigger == NULL) {
				free(buf);
				fclose(fp);
				return TU_READ_UNOPENABLE;
			}
			buf = bigger;
		}
		cap *= 2;
	}
	if (ferror(fp)) {
		free(buf);
		fclose(fp);
		return TU_READ_UNOPENABLE;
	}
	fclose(fp);
	fmt = tu_fmt_detect(path, &st, buf, len);
	/* A web archive is believed as a name, so the Type line says so, but
	 * there is no reader for one here: the reference tool reads a file
	 * with that name which is not an archive as plain text, and so does
	 * this.  A real archive, whose contents are a binary property list,
	 * comes back as its own bytes.  Recorded in src/textutil/NOTES.md. */
	if (fmt == FMT_TXT || fmt == FMT_WEBARCHIVE) {
		why = tu_read_plain(path, d);
	} else {
		/* A format that came from the bytes rather than the name is a
		 * guess, and a guess whose read fails is read as plain text.  That
		 * is what makes an unclosed RTF group come back as text without
		 * changing what a file called notes.rtf does. */
		int fall_back = tu_fmt_read_falls_back(path);

		if (fmt == FMT_RTF)
			why = tu_read_rtf(buf, len, d, &found);
		else
			why = tu_read_html(buf, len, d, &found);
		tu_meta_free(&found);
		if (why != TU_READ_OK && fall_back)
			why = tu_read_plain(path, d);
	}
	free(buf);
	return why;
}

/* Report a reason from the gate above.  The reference tool's two wordings go
 * out as themselves; a missing reader is this port's own message. */
static void
report_read(const char *path, int why)
{
	if (why == TU_READ_ABSENTFORMAT)
		fprintf(stderr, "textutil: reading %s input is not implemented\n",
		    tu_fmt_name((tu_fmt_t)forced));
	else
		tu_read_error(path, why);
}

static int
write_output(tu_fmt_t fmt, const tu_doc_t *d, const char *path,
    const tu_style_t *st, const tu_meta_t *m)
{
	if (fmt != FMT_RTFD && strcmp(path, "/dev/stdout") != 0) {
		int why = tu_check_output(path);

		if (why != TU_WRITE_OK) {
			tu_write_error(path, why);
			return -1;
		}
	}
	/* A single byte encoding needs a conversion table this port does not
	 * carry, and writing UTF-8 in its place would be a silent answer to a
	 * question that was not asked.  RTF, RTFD and wordml are written in
	 * UTF-8 whatever the text was decoded from, and the reference tool
	 * leaves them there, so nothing is declined for them. */
	if (enc == TU_ENC_UNSUPPORTED &&
	    (fmt == FMT_TXT || fmt == FMT_HTML || fmt == FMT_WEBARCHIVE)) {
		fprintf(stderr, "textutil: -encoding %s is not implemented\n",
		    enc_given);
		return -1;
	}
	switch (fmt) {
	case FMT_TXT:
		return tu_write_txt(d, path, enc);
	case FMT_RTF:
		return tu_write_rtf(d, path, st, m);
	case FMT_HTML:
		return tu_write_html(d, path, st, m, enc);
	case FMT_WEBARCHIVE:
		return tu_write_webarchive(d, path, st, m, enc);
	case FMT_RTFD:
		return tu_write_rtfd(d, path, st, m);
	case FMT_WORDML:
		return tu_write_wordml(d, path, st, m);
	case FMT_DOCX:
		return tu_write_docx(d, path, st, m);
	case FMT_ODT:
		return tu_write_odt(d, path, st, m);
	default:
		/* Recognised so that the parser matches, but not written by this
		 * port; see src/textutil/NOTES.md. */
		fprintf(stderr, "textutil: %s output is not implemented\n",
		    tu_fmt_name(fmt));
		return -1;
	}
}

static int
write_to_stdout(tu_fmt_t fmt, const tu_doc_t *d)
{
	return write_output(fmt, d, "/dev/stdout", &style, &meta);
}

int
main(int argc, char **argv)
{
	tu_cmd_t cmd = CMD_HELP;
	int cmd_seen = 0;
	tu_fmt_t fmt = (tu_fmt_t)-1;
	const char *output = NULL, *extension = NULL;
	int use_stdout = 0, use_stdin = 0;
	int past_dashdash = 0;
	const char *files[4096];
	int nfiles = 0;
	int failed = 0;
	char name[1024];

	for (int i = 1; i < argc; i++) {
		const char *a = argv[i];
		const struct opt *o = NULL;

		if (past_dashdash || a[0] != '-' || a[1] == '\0') {
			if (nfiles < (int)(sizeof(files) / sizeof(files[0])))
				files[nfiles++] = a;
			continue;
		}
		if (strcmp(a, "--") == 0) {
			past_dashdash = 1;
			continue;
		}
		for (size_t j = 0; j < sizeof(options) / sizeof(options[0]); j++)
			if (strcmp(a + 1, options[j].name) == 0) {
				o = &options[j];
				break;
			}
		if (o == NULL) {
			/* Not an option this tool knows: show the usage, as the
			 * reference tool does, and succeed. */
			print_usage();
			return 0;
		}
		if (o->arity != 0) {
			if (i + 1 >= argc) {
				fprintf(stderr, "%s\n", o->missing);
				return 1;
			}
			if (argv[i + 1][0] == '\0')
				for (size_t j = 0;
				     j < sizeof(empty_values) / sizeof(empty_values[0]);
				     j++)
					if (strcmp(o->name, empty_values[j].name) == 0) {
						fprintf(stderr, "%s\n",
						    empty_values[j].empty);
						return 1;
					}
		}
		/* A metadata option may only be given once.  The check is here,
		 * with the other per-option checks, because the diagnostic is
		 * about the option rather than about the value that follows
		 * it. */
		for (size_t j = 0; j < sizeof(once_only) / sizeof(once_only[0]);
		     j++)
			if (strcmp(o->name, once_only[j].name) == 0) {
				if (once_only[j].seen) {
					fprintf(stderr, "%s\n",
					    once_only[j].many);
					return 1;
				}
				once_only[j].seen = 1;
			}
		if (o->is_command) {
			if (cmd_seen) {
				fprintf(stderr, "Multiple commands specified.\n");
				return 1;
			}
			cmd_seen = 1;
			if (strcmp(o->name, "help") == 0)
				cmd = CMD_HELP;
			else if (strcmp(o->name, "info") == 0)
				cmd = CMD_INFO;
			else {
				const char *v = argv[++i];

				fmt = tu_fmt_parse(v);
				if ((int)fmt < 0) {
					fprintf(stderr, "Invalid output format.\n");
					return 1;
				}
				cmd = strcmp(o->name, "convert") == 0 ?
				    CMD_CONVERT : CMD_CAT;
			}
			continue;
		}
		if (strcmp(o->name, "output") == 0)
			output = argv[++i];
		else if (strcmp(o->name, "extension") == 0)
			extension = argv[++i];
		else if (strcmp(o->name, "stdout") == 0)
			use_stdout = 1;
		else if (strcmp(o->name, "stdin") == 0)
			use_stdin = 1;
		else if (strcmp(o->name, "font") == 0)
			style.font = argv[++i];
		else if (strcmp(o->name, "fontsize") == 0)
			style.fontsize = atoi(argv[++i]);
		else if (strcmp(o->name, "title") == 0)
			meta.title = argv[++i];
		else if (strcmp(o->name, "author") == 0)
			meta.author = argv[++i];
		else if (strcmp(o->name, "subject") == 0)
			meta.subject = argv[++i];
		else if (strcmp(o->name, "comment") == 0)
			meta.comment = argv[++i];
		else if (strcmp(o->name, "editor") == 0)
			meta.editor = argv[++i];
		else if (strcmp(o->name, "company") == 0)
			meta.company = argv[++i];
		else if (strcmp(o->name, "keywords") == 0) {
			char *list = keyword_list(argv[++i]);

			if (list == NULL) {
				fprintf(stderr, "Invalid keyword list.\n");
				return 1;
			}
			meta.keywords = list;
		} else if (strcmp(o->name, "creationtime") == 0) {
			struct tu_time tm;

			meta.creationtime = argv[++i];
			if (!tu_parse_timestamp(meta.creationtime, &tm)) {
				fprintf(stderr, "Invalid creation time.\n");
				return 1;
			}
		} else if (strcmp(o->name, "modificationtime") == 0) {
			struct tu_time tm;

			meta.modificationtime = argv[++i];
			if (!tu_parse_timestamp(meta.modificationtime, &tm)) {
				fprintf(stderr, "Invalid modification time.\n");
				return 1;
			}
		} else if (strcmp(o->name, "encoding") == 0) {
			enc_given = argv[++i];
			int e = tu_encoding_parse(enc_given);

			if (e < 0) {
				fprintf(stderr, "Invalid output encoding.\n");
				return 1;
			}
			enc = (tu_encoding_t)e;
		} else if (strcmp(o->name, "format") == 0) {
			/* Validated as it is read, so that a bad name is
			 * refused before any work, and ahead of -help. */
			tu_fmt_t f = tu_fmt_parse(argv[++i]);

			if ((int)f < 0) {
				fprintf(stderr, "Invalid input format.\n");
				return 1;
			}
			forced = (int)f;
		} else if (o->arity != 0)
			(void)argv[++i];	/* accepted, not acted on */
	}

	/* The stdin/file conflict is diagnosed before the "no command" fallback,
	 * so `textutil -stdin somefile` reports the conflict rather than usage. */
	if (use_stdin && nfiles != 0) {
		fprintf(stderr, "Input files and stdin both specified.\n");
		return 1;
	}
	if (!cmd_seen) {
		print_usage();
		return 0;
	}
	if (cmd == CMD_HELP) {
		print_usage();
		return 0;
	}
	if (cmd == CMD_CONVERT || cmd == CMD_CAT)
		; /* format already validated while parsing */
	if (nfiles == 0 && !use_stdin) {
		fprintf(stderr, "No input files specified.\n");
		return 1;
	}

	/* -stdin replaces the file list, and the bytes are decoded exactly as a
	 * file's would be, so a byte order mark on the redirected data is
	 * honoured just as it is in a file on disk. */
	if (use_stdin) {
		unsigned char *raw = NULL;
		size_t rawlen = 0, cap = 0;
		tu_doc_t d = { NULL, 0, 0 };

		for (;;) {
			size_t got;

			if (rawlen + 65536 > cap) {
				unsigned char *b;

				cap = cap ? cap * 2 : 131072;
				b = realloc(raw, cap);
				if (b == NULL) {
					free(raw);
					fprintf(stderr,
					    "Error reading stdin.\n");
					return 1;
				}
				raw = b;
			}
			got = fread(raw + rawlen, 1, 65536, stdin);
			rawlen += got;
			if (got < 65536)
				break;
		}
		if (ferror(stdin)) {
			free(raw);
			fprintf(stderr, "Error reading stdin.\n");
			return 1;
		}
		/* An empty read still has to give the writers a valid, empty
		 * document, so decode a 1-byte buffer rather than a NULL one. */
		if (tu_decode_plain(raw != NULL ? raw :
		    (unsigned char *)"", rawlen, &d) != 0) {
			free(raw);
			fprintf(stderr, "Error reading stdin.\n");
			return 1;
		}
		free(raw);

		if (cmd == CMD_INFO) {
			tu_info_doc("stdin", &d);
			tu_doc_free(&d);
			return 0;
		}
		if (use_stdout && output == NULL) {
			if (write_to_stdout(fmt, &d) != 0)
				failed = 1;
		} else {
			if (output != NULL)
				snprintf(name, sizeof(name), "%s", output);
			else if (extension != NULL)
				snprintf(name, sizeof(name), "out.%s", extension);
			else
				snprintf(name, sizeof(name), "out.%s",
				    tu_fmt_extension(fmt));
			if (write_output(fmt, &d, name, &style, &meta) != 0)
				failed = 1;
		}
		tu_doc_free(&d);
		return failed ? 1 : 0;
	}

	if (cmd == CMD_INFO) {
		for (int i = 0; i < nfiles; i++)
			tu_info_file(files[i], forced);
		return 0;
	}

	if (cmd == CMD_CAT) {
		tu_doc_t all = { NULL, 0, 0 };
		size_t cap = 0;

		for (int i = 0; i < nfiles; i++) {
			tu_doc_t d;
			int why;

			if ((why = read_input(files[i], &d)) != TU_READ_OK) {
				report_read(files[i], why);
				/* -cat is the one command whose status reports a
				 * missing input; -convert and -info carry on. */
				failed = 1;
				continue;
			}
			if (all.len + d.len + 1 > cap) {
				char *b;

				cap = (all.len + d.len + 1) * 2;
				b = realloc(all.text, cap);
				if (b == NULL) {
					tu_doc_free(&d);
					break;
				}
				all.text = b;
			}
			memcpy(all.text + all.len, d.text, d.len);
			all.len += d.len;
			all.nchars += d.nchars;
			tu_doc_free(&d);
		}
		if (output != NULL)
			snprintf(name, sizeof(name), "%s", output);
		else if (extension != NULL)
			snprintf(name, sizeof(name), "out.%s", extension);
		else
			snprintf(name, sizeof(name), "out.%s", tu_fmt_extension(fmt));
		/* A missing input stops -cat writing anything at all, though it
		 * still reports the file and exits 1. */
		if (failed) {
			free(all.text);
			return 1;
		}
		if (use_stdout) {
			if (write_to_stdout(fmt, &all) != 0)
				failed = 1;
		} else if (write_output(fmt, &all, name, &style, &meta) != 0)
			failed = 1;
		free(all.text);
		return failed ? 1 : 0;
	}

	/* -convert */
	for (int i = 0; i < nfiles; i++) {
		tu_doc_t d;
		const char *ext = extension != NULL ? extension :
		    tu_fmt_extension(fmt);
		int why;

		if ((why = read_input(files[i], &d)) != TU_READ_OK) {
			report_read(files[i], why);
			continue;
		}
		if (i == 0 && output != NULL)
			snprintf(name, sizeof(name), "%s", output);
		else
			output_name(files[i], name, sizeof(name), ext);
		if (i == 0 && use_stdout) {
			if (write_to_stdout(fmt, &d) != 0)
				failed = 1;
		} else if (write_output(fmt, &d, name, &style, &meta) != 0)
			failed = 1;
		tu_doc_free(&d);
	}
	return failed ? 1 : 0;
}
