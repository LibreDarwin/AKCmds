/*
 * tiffutil - inspect and convert TIFF files.
 *
 * Copyright (C) 2026, LibreDarwin
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "tiffutil.h"
#include "buf.h"

static const char usage_text[] =
"usage: tiffutil [operation] file ...\n"
"\n"
"Operations:\n"
"  -none              Rewrite the file uncompressed.\n"
"  -lzw               Rewrite the file with LZW compression.\n"
"  -packbits          Rewrite the file with PackBits compression.\n"
"  -cat               Concatenate multiple TIFF files.\n"
"  -catnosizecheck    Concatenate multiple TIFF files, suppressing size check.\n"
"  -cathidpicheck     Concatenate multiple TIFF files, hiding the DPI check.\n"
"  -extract N         Extract the Nth image from a file.\n"
"  -info              Print basic information about a file.\n"
"  -verboseinfo       Print verbose information about a file.\n"
"  -dump              Print a dump of the file's TIFF structure.\n"
"\n"
"Options:\n"
"  -out file          Write the output to the named file.\n";

static int
print_usage(void)
{
	fputs(usage_text, stderr);
	return 1;
}

/* Load directory d as a plain sample buffer in native endianness. */
static int
load_image(tiff_t *t, int d, tuimg_t *im, int *err)
{
	uint32_t w = 0, h = 0, bps = 0, spp = 1, rps = 0, comp = 1, photo = 1;
	uint32_t bpp;
	size_t rawlen = 0, need;
	unsigned char *raw = NULL;
	buf_t all = {NULL, 0, 0};

	memset(im, 0, sizeof(*im));
	*err = 0;
	if (tu_get_uint(t, d, TAG_IMAGEWIDTH, &w) < 0 ||
	    tu_get_uint(t, d, TAG_IMAGELENGTH, &h) < 0 ||
	    tu_get_uint(t, d, TAG_BITSPERSAMPLE, &bps) < 0) {
		*err = 1;
		return -1;
	}
	tu_get_uint(t, d, TAG_SAMPLESPERPIXEL, &spp);
	tu_get_uint(t, d, TAG_ROWSPERSTRIP, &rps);
	tu_get_uint(t, d, TAG_COMPRESSION, &comp);
	tu_get_uint(t, d, TAG_PHOTOMETRIC, &photo);
	if (spp == 0)
		spp = 1;
	bpp = (bps + 7) / 8;

	/* Gather the strips in order, decompressing each. */
	for (uint32_t i = 0; i < t->nstrips; i++) {
		uint32_t off = t->strips[i], bc = t->stripbc[i];
		unsigned char *dec = NULL;
		size_t declen = 0;

		if (off > t->len || bc > t->len - off) {
			tu_warn("TIFFReadEncodedStrip: Read error.\n");
			*err = 1;
			buf_free(&all);
			return -1;
		}
		switch (comp) {
		case COMP_NONE:
			if (buf_put(&all, t->data + off, bc) < 0)
				goto oom;
			break;
		case COMP_LZW:
			if (lzw_decode(t->data + off, bc, &dec, &declen) < 0)
				goto oom;
			buf_put(&all, dec, declen);
			free(dec);
			break;
		case COMP_PACKBITS:
			if (packbits_decode(t->data + off, bc, &dec, &declen) < 0)
				goto oom;
			buf_put(&all, dec, declen);
			free(dec);
			break;
		default:
			tu_warn("TIFFReadEncodedStrip: %s compression not "
			    "implemented.\n", tu_compression_name(comp));
			*err = 1;
			buf_free(&all);
			return -1;
		}
	}
	raw = all.p;
	rawlen = all.len;

	/* Undo horizontal differencing, which runs across each row and never
	 * carries from one row into the next. */
	{
		uint32_t pred = 0;
		if (tu_get_uint(t, d, TAG_PREDICTOR, &pred) == 0 && pred == 2 &&
		    bps == 8) {
			size_t bpl = (size_t)w * spp;
			if (bpl == 0 || bpl > rawlen)
				bpl = rawlen;
			for (size_t base = 0; base + bpl <= rawlen; base += bpl)
				for (size_t i = spp; i < bpl; i++)
					raw[base + i] = (unsigned char)
					    (raw[base + i] + raw[base + i - spp]);
		}
	}

	need = (size_t)w * h * spp * bpp;
	if (rawlen < need)
		rawlen = need;
	im->width = w;
	im->height = h;
	im->bps = bps;
	im->spp = spp;
	im->photometric = photo;
	im->rows_per_strip = rps;
	/* Samples reach the writer in host order; a big-endian source has to
	 * be converted, or the writer's swap back to big-endian double-swaps
	 * it. Little-endian sources need nothing. */
	if (bps == 16 && t->be) {
		size_t i;
		for (i = 0; i + 1 < rawlen; i += 2) {
			unsigned char u = raw[i];
			raw[i] = raw[i + 1];
			raw[i + 1] = u;
		}
	}

	im->px = raw;
	im->pxlen = rawlen;
	return 0;
oom:
	free(raw);
	*err = 1;
	return -1;
}

/*
 * Produce the strip to write: undo the endianness and predictor, then
 * compress if asked.  The predictor is applied before compression because
 * that is the order the reference tool uses, and it is what makes 8-bit LZW
 * round-trip.
 */
static int
encode_one(const tuimg_t *im, int compression, unsigned char **out,
    size_t *outlen, int *predictor)
{
	int pred = tiff_predictor_for(compression, im->bps);
	int swap = im->bps == 16;
	unsigned char *raw = NULL, *st = NULL;
	size_t rawlen = 0, stlen = 0;

	if (tiff_build_strip(im->px, im->pxlen, im->bps, im->spp, im->width,
	    im->height, pred, swap, &raw, &rawlen) < 0)
		return -1;
	if (compression == COMP_LZW) {
		if (lzw_encode(raw, rawlen, &st, &stlen) < 0) {
			free(raw);
			return -1;
		}
	} else if (compression == COMP_PACKBITS) {
		if (packbits_encode(raw, rawlen,
		    (size_t)im->width * im->spp * ((im->bps + 7) / 8),
		    &st, &stlen) < 0) {
			free(raw);
			return -1;
		}
	} else {
		st = raw;
		stlen = rawlen;
		raw = NULL;
	}
	free(raw);
	*predictor = pred;
	*out = st;
	*outlen = stlen;
	return 0;
}

/* The reference tool distinguishes a file that is absent from one that
 * exists but is not a TIFF, and words the two failures differently. */
static int
report_open_failure(const tiff_t *t, const char *path)
{
	if (t->openerc == TUFF_ENOENT) {
		fprintf(stderr, "Error: Failed to create image source for file "
		    "%s. Either it isn't a TIFF file, or there are unrecognized "
		    "tags; try tiffutil -dump for more info.\n", path);
		fprintf(stderr, "Error: Can't read from file %s.\n", path);
		fprintf(stderr, "No output file created due to errors.\n");
	} else {
		fprintf(stderr, "Error: Can't open %s. Either it isn't a TIFF "
		    "file, or there are unrecognized tags; try tiffutil -dump for "
		    "more info.\n", path);
		fprintf(stderr, "No output file created due to errors.\n");
	}
	return 5;
}

static int
write_operations(const char *cmd, const char *inpath, const char *outpath)
{
	tiff_t t;
	tuimg_t im;
	int err = 0, compression = COMP_NONE, predictor = 1, rc = 0;
	unsigned char *strip = NULL;
	size_t striplen = 0;

	if (strcmp(cmd, "-lzw") == 0)
		compression = COMP_LZW;
	else if (strcmp(cmd, "-packbits") == 0)
		compression = COMP_PACKBITS;

	if (tiff_open_file(&t, inpath) < 0) {
		int rc = report_open_failure(&t, inpath);
		tiff_close(&t);
		return rc;
	}
	if (load_image(&t, 0, &im, &err) < 0) {
		tiff_close(&t);
		return err;
	}
	if (encode_one(&im, compression, &strip, &striplen, &predictor) < 0) {
		free(im.px);
		tiff_close(&t);
		return 1;
	}
	if (tiff_write_image(outpath, &im, strip, striplen, compression,
	    predictor) < 0)
		rc = 1;
	free(strip);
	free(im.px);
	tiff_close(&t);
	if (rc == 0)
		printf("1 image written to %s.\n", outpath);
	return rc;
}

int
main(int argc, char **argv)
{
	const char *cmd = NULL, *inpath = NULL, *outpath = "out.tiff";
	int have_extract = -1;
	int i;

	for (i = 1; i < argc; i++) {
		const char *a0 = argv[i];
		if (a0[0] == '-' && a0[1] != '\0' &&
		    (strcmp(a0, "-info") == 0 || strcmp(a0, "-verboseinfo") == 0 ||
		    strcmp(a0, "-dump") == 0))
			tu_set_chatter(1);
		const char *a = argv[i];
		if (a[0] == '-' && a[1] != '\0') {
			if (strcmp(a, "-out") == 0) {
				if (i + 1 >= argc) {
					fprintf(stderr, "Error: -out requires a file name.\n");
					return 1;
				}
				outpath = argv[++i];
				continue;
			}
			if (strcmp(a, "-extract") == 0) {
				if (i + 1 >= argc) {
					fprintf(stderr, "Error: -extract requires an image number.\n");
					return 1;
				}
				have_extract = atoi(argv[++i]);
				if (cmd == NULL)
					cmd = a;
				continue;
			}
			if (strcmp(a, "-none") == 0 || strcmp(a, "-lzw") == 0 ||
			    strcmp(a, "-packbits") == 0 || strcmp(a, "-cat") == 0 ||
			    strcmp(a, "-catnosizecheck") == 0 ||
			    strcmp(a, "-cathidpicheck") == 0 ||
			    strcmp(a, "-info") == 0 ||
			    strcmp(a, "-verboseinfo") == 0 ||
			    strcmp(a, "-dump") == 0) {
				if (cmd != NULL) {
					fprintf(stderr, "Error: One input file name expected.\n");
					return 1;
				}
				cmd = a;
				continue;
			}
			fprintf(stderr, "Error: No valid command provided.\n");
			return 1;
		}
		if (inpath != NULL) {
			fprintf(stderr, "Error: One input file name expected.\n");
			return 1;
		}
		inpath = a;
	}
	if (cmd == NULL)
		return print_usage();
	if (inpath == NULL) {
		fprintf(stderr, "usage: tiffutil [operation] file ...\n");
		return 1;
	}
	if (have_extract >= 0) {
		tiff_t t;
		if (tiff_open_file(&t, inpath) < 0) {
			int rc = report_open_failure(&t, inpath);
			tiff_close(&t);
			return rc;
		}
		if (have_extract >= t.ndir) {
			fprintf(stderr, "Error: %s has only %d image%s.\n", inpath,
			    t.ndir, t.ndir == 1 ? "" : "s");
			fprintf(stderr, "No output file created due to errors.\n");
			tiff_close(&t);
			return 5;
		}
		{
			tuimg_t im;
			int err = 0;
			uint32_t src_comp = COMP_NONE;
			unsigned char *strip = NULL;
			size_t striplen = 0;
			int pred = 0;

			if (load_image(&t, have_extract, &im, &err) < 0) {
				tiff_close(&t);
				return err;
			}
			/* -extract keeps whatever the source directory used,
			 * rather than rewriting uncompressed. */
			tu_get_uint(&t, have_extract, TAG_COMPRESSION, &src_comp);
			if (src_comp != COMP_NONE && src_comp != COMP_LZW &&
			    src_comp != COMP_PACKBITS)
				src_comp = COMP_NONE;
			/* The pixels are in host order here, so they still have to
			 * go through the strip builder; that is what puts 16-bit
			 * samples back into big-endian. */
			if (encode_one(&im, src_comp, &strip, &striplen,
			    &pred) < 0) {
				free(im.px);
				tiff_close(&t);
				return 1;
			}
			if (tiff_write_image(outpath, &im, strip, striplen,
			    src_comp, pred) < 0) {
				free(strip);
				free(im.px);
				tiff_close(&t);
				return 1;
			}
			free(strip);
			free(im.px);
			printf("1 image written to %s.\n", outpath);
		}
		tiff_close(&t);
		return 0;
	}
	if (strcmp(cmd, "-info") == 0)
		return tu_cmd_info(inpath, 0) == 0 ? 0 : 0;
	if (strcmp(cmd, "-verboseinfo") == 0)
		return tu_cmd_info(inpath, 1) == 0 ? 0 : 0;
	if (strcmp(cmd, "-dump") == 0)
		return tu_cmd_dump(inpath) == 0 ? 0 : 0;
	if (strcmp(cmd, "-cat") == 0 || strcmp(cmd, "-catnosizecheck") == 0 ||
	    strcmp(cmd, "-cathidpicheck") == 0) {
		fprintf(stderr, "Error: -cat is not supported in this build.\n");
		return 1;
	}
	return write_operations(cmd, inpath, outpath);
}
