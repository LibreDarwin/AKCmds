/*
 * tiffutil - inspect and convert TIFF files.
 *
 * Copyright (C) 2026, LibreDarwin
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

#include "tiffutil.h"
#include "buf.h"

static const char usage_text[] =
	"Usage: tiffutil -none           infile                  [-out outfile]\n"
	"                -lzw            infile                  [-out outfile]\n"
	"                -packbits       infile                  [-out outfile]\n"
	"                -cat            infile1 [infile2 ...]   [-out outfile]\n"
	"                -catnosizecheck infile1 [infile2 ...]   [-out outfile]\n"
	"                -cathidpicheck  infile1 [infile2 ...]   [-out outfile]\n"
	"                -extract        num infile              [-out outfile]\n"
	"                -info           infile1 [infile2 ...]\n"
	"                -verboseinfo    infile1 [infile2 ...]\n"
	"                -dump           infile1 [infile2 ...]\n"
	"\n";

/* Reject a command line: print the complaint (when there is one) and the usage
   block that the reference tool always follows it with. */
static int
usage_error(const char *msg)
{
	if (msg != NULL)
		fprintf(stderr, "Error: %s\n", msg);
	fputs(usage_text, stderr);
	return 1;
}

/* The per-chroma contribution tables behind blue and red.  Each entry is
   rounded on its own, away from zero on a tie, and *not* against the luma:
   that is what puts two otherwise identical ties on opposite sides.  With the
   default blue coefficient a Cb of 3 contributes -221.5, which the reference
   rounds to -222, so 239 + -222 is 17 where rounding the whole sum would have
   given 18.  Green is the odd one out and is not built this way. */
static void
ycbcr_contrib(double k, int *tab)
{
	int c;

	for (c = 0; c < 256; c++) {
		double v = 2.0 * (1.0 - k) * (c - 128);

		tab[c] = (int)(v >= 0.0 ? floor(v + 0.5) : ceil(v - 0.5));
	}
}

/* Convert a two-by-two subsampled YCbCr buffer to RGB in place of the
   samples.  Six bytes per block go in, twelve come out, so the strip grows by
   half.  Blue and red add their table entry to the luma; green is one
   expression rounded once, and its ties fall toward zero rather than away. */
static int
ycbcr_to_rgb(unsigned char **rawp, size_t *rawlenp, uint32_t w, uint32_t h,
    double kr, double kg, double kb)
{
	int btab[256], rtab[256];
	double gcb, gcr;
	unsigned char *raw = *rawp, *out;
	size_t need = (size_t)w * h * 3;
	uint32_t bx, by, x, y;

	/* The reference only ever reads 2x2, whatever the subsampling tag says,
	   and a zero green coefficient would divide by nothing. */
	if (w == 0 || h == 0 || (w & 1) != 0 || (h & 1) != 0 || kg == 0.0)
		return -1;
	out = malloc(need);
	if (out == NULL)
		return -1;
	ycbcr_contrib(kb, btab);
	ycbcr_contrib(kr, rtab);
	gcb = 2.0 * kb * (1.0 - kb) / kg;
	gcr = 2.0 * kr * (1.0 - kr) / kg;
	for (by = 0; by < h; by += 2) {
		for (bx = 0; bx < w; bx += 2) {
			const unsigned char *in = raw +
			    ((size_t)(by / 2) * (w / 2) + bx / 2) * 6;
			int cb = in[4], cr = in[5];

			/* Four luma bytes share one chroma pair, so each of
			   the four pixels rounds its own copy of the
			   contribution tables. */
			for (y = 0; y < 2; y++) {
				for (x = 0; x < 2; x++) {
					int luma = in[y * 2 + x];
					int b = luma + btab[cb];
					int g = (int)ceil(luma -
					    gcb * (cb - 128) -
					    gcr * (cr - 128) - 0.5);
					int r = luma + rtab[cr];
					unsigned char *o = out +
					    (((size_t)by + y) * w + bx + x) * 3;

					if (b < 0)
						b = 0;
					else if (b > 255)
						b = 255;
					if (g < 0)
						g = 0;
					else if (g > 255)
						g = 255;
					if (r < 0)
						r = 0;
					else if (r > 255)
						r = 255;
					o[0] = b;
					o[1] = g;
					o[2] = r;
				}
			}
		}
	}
	free(*rawp);
	*rawp = out;
	*rawlenp = need;
	return 0;
}

/* Load directory d as a plain sample buffer in native endianness. */
static int
load_image(tiff_t *t, int d, tuimg_t *im, int *err)
{
	uint32_t w = 0, h = 0, bps = 0, spp = 1, rps = 0, comp = 1, photo = 1;
	uint32_t srcspp;
	uint32_t bpp;
	size_t rawlen = 0, need;
	unsigned char *raw = NULL;
	buf_t all = {NULL, 0, 0};
	uint32_t rowsdone = 0;

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
	srcspp = spp;
	bpp = (bps + 7) / 8;

	/* Gather the strips in order, decompressing each. */
	for (uint32_t i = 0; i < t->nstrips[d]; i++) {
		uint32_t off = t->strips[d][i], bc = t->stripbc[d][i];
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
		case COMP_G4:
			/* Facsimile is two dimensional and codes each line
			 * against the one above it, so it needs the row count
			 * of this strip: a strip starts from the imaginary
			 * line again. */
			{
				uint32_t rows = rps != 0 ? rps : h;
				int invert = photo == 0;

				if (rows > h - rowsdone)
					rows = h - rowsdone;
				if (bps != 1 || spp != 1)
					goto notimpl;
				if (g4_decode(t->data + off, bc, w, rows, invert,
				    &dec, &declen) < 0)
					goto oom;
				buf_put(&all, dec, declen);
				free(dec);
				rowsdone += rows;
			}
			break;
		default:
notimpl:
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

	/* Samples narrower than a byte arrive packed, and the reference hands
	 * them on one to a byte.  A palette's samples are indices rather than
	 * intensities, so those are simply widened; everything else is a real
	 * value and is stretched over the whole range by repeating its bits,
	 * which turns a one into ff and a two into 55.  Rows are byte aligned. */
	if (bps < 8 && bps != 0) {
		size_t srow = ((size_t)w * spp * bps + 7) / 8;
		size_t orow = (size_t)w * spp;
		unsigned char *out = malloc(orow * h);

		if (out != NULL) {
			for (uint32_t y = 0; y < h; y++) {
				const unsigned char *s = raw + (size_t)y * srow;
				unsigned char *o = out + (size_t)y * orow;
				size_t avail = rawlen > (size_t)y * srow
				    ? rawlen - (size_t)y * srow : 0;
				size_t bits = 0;

				for (size_t i = 0; i < orow; i++) {
					size_t byte = bits / 8, off = bits % 8;
					unsigned v = byte < avail
					    ? ((s[byte] >> (8 - bps - off)) &
					        ((1u << bps) - 1)) : 0;
					bits += bps;
					if (photo == 3) {
						o[i] = (unsigned char)v;
					} else {
						unsigned r = 0;
						for (uint32_t k = 0; k < 8; k += bps)
							r = (r << bps) | v;
						o[i] = (unsigned char)r;
					}
				}
			}
			free(raw);
			raw = out;
			rawlen = orow * h;
		}
	}

	/* The reference tool keeps at most one channel past the colour
	 * channels: the first ExtraSamples value when the input has such a
	 * tag, or unassociated alpha when the input has exactly one spare
	 * sample.  More than that is dropped from the samples themselves, and
	 * an input that says nothing about its extra samples keeps only the
	 * colours. */
	im->xalpha = 2;
	{
		unsigned char *xs = NULL;
		uint32_t xn = 0, ncolor = tiff_color_channels(photo);
		uint32_t keep = 0, group = 0;
		int tagged = 0, rgbout;

		if (tu_get_bytes(t, d, TAG_EXTRASAMPLES, &xs, &xn) == 0 &&
		    xn >= 1) {
			im->xalpha = t->be
			    ? (uint16_t)((uint32_t)xs[0] << 8 | xs[1])
			    : (uint16_t)((uint32_t)xs[1] << 8 | xs[0]);
			tagged = 1;
			keep = ncolor + 1;
			/* More extra samples than there are samples in a pixel
			 * is refused outright, and leaves nothing to write. */
			if (xn > spp)
				im->unusable = 1;
		} else if (ncolor != 0 && spp == ncolor + 1) {
			keep = spp;
		} else {
			keep = ncolor;
		}
		free(xs);
		if (keep != 0 && spp > keep) {
			size_t sstride = (size_t)spp * bpp;
			size_t dstride = (size_t)keep * bpp;
			size_t srow = (size_t)w * sstride;
			size_t drow = (size_t)w * dstride;

			/* Dropping the surplus is not a uniform repack.  An image
			 * that comes out as RGB has its channels copied out one
			 * pixel at a time, so every pixel keeps its own leading
			 * channels.  The other layouts are cut a row at a time
			 * instead: a tagged row is simply shortened, while an
			 * untagged one is strided as though a pixel were two
			 * samples wide for a single-channel image, or one wider
			 * than the colour channels otherwise.  CMYK and Lab
			 * both land in that second group, which is why a
			 * five-sample Lab pixel is cut four samples at a time
			 * rather than five. */
			rgbout = tiff_out_photometric(photo, srcspp, bps) == 2;
			if (rgbout)
				group = spp;
			else if (!tagged)
				group = ncolor + 1;

			/* RGB data of more than one byte per sample is dropped
			 * to an empty image once the pixel is wider than RGBA:
			 * the reference tool never fills the buffer it would
			 * have copied into. */
			if (bps > 8 && photo == 2 && spp > 4) {
				memset(raw, 0, drow * h);
			} else {
				for (size_t r = 0; r * srow < rawlen; r++) {
					unsigned char *s = raw + r * srow;
					unsigned char *o = raw + r * drow;

					if (rgbout || !tagged) {
						for (uint32_t x = 0; x < w; x++)
							memmove(o + x * dstride,
							    s + x * group * bpp,
							    dstride);
					} else {
						memmove(o, s, drow);
					}
				}
			}
			rawlen = drow * h;
			spp = keep;
		}
	}

	need = (size_t)w * h * spp * bpp;
	if (rawlen < need)
		rawlen = need;
	im->width = w;
	im->height = h;
	/* Anything narrower than a byte was already unpacked one sample to a
	 * byte, so that is the depth the rewrite has to report. */
	im->bps = bps < 8 ? 8 : bps;
	im->spp = spp;
	im->srcspp = srcspp;
	im->srcbps = bps;
	im->photometric = photo;
	/* Three colour spaces change their name on the way out, and the rest keep
	 * the name they came in with: the CIE Lab encodings all arrive as plain
	 * Lab, a palette stays a palette, and YCbCr and the log encodings are
	 * laid down as RGB.  WhiteIsZero, LogL and the bilevel gray case are the
	 * ones that depend on the pixel: a lone sample is gray, three or four
	 * are colour, and one bit goes to the bilevel compressor either way. */
	im->outphoto = tiff_out_photometric(photo, srcspp, bps);
	/* A photometric the reference tool has no channels for is refused before
	 * it looks at the samples at all, and that refusal outranks everything
	 * else.  A transparency mask is registered but has no colours to lay
	 * down, so it is dropped as an image and leaves an empty file. */
	if (tiff_color_channels(photo) == 0) {
		if (photo != 4)
			im->unopenable = 1;
		else
			im->unusable = 1;
	} else {
		/* Otherwise the samples have to be able to fill the output: RGB and
		 * the CIE Lab encodings want three, separated five, YCbCr and
		 * LogLuv are only ever read at exactly three, and LogL is either
		 * one sample of gray or three or four of colour. */
		if ((photo == 2 || photo == 8 || photo == 9 || photo == 10) &&
		    srcspp < 3)
			im->unusable = 1;
		if (photo == 5 && srcspp < 5)
			im->unusable = 1;
		if ((photo == 6 || photo == 32845) && srcspp != 3)
			im->unusable = 1;
		if (photo == 32844 && srcspp != 1 && (srcspp < 3 || srcspp > 4))
			im->unusable = 1;
	}
	im->rows_per_strip = rps;
	im->xres = 72;
	im->yres = 72;
	/* The reference tool carries the source resolution into the output as
	 * whole units per inch, discarding the fraction rather than rounding.
	 * A resolution whose numerator is zero is treated as unset and becomes
	 * the 72dpi default. */
	{
		uint32_t num = 0, den = 0;

		if (tu_get_rational(t, d, TAG_XRESOLUTION, &num, &den) == 0)
			im->xres = num == 0 || den == 0 ? 72 : num / den;
		if (tu_get_rational(t, d, TAG_YRESOLUTION, &num, &den) == 0)
			im->yres = num == 0 || den == 0 ? 72 : num / den;
	}
	im->cmap = NULL;
	im->cmapcount = 0;
	/* A palette is passed through as values, so a little-endian source has
	 * to be swapped into the big-endian form the writer emits.  tu_get_bytes
	 * counts elements, and a colour map holds SHORTs.  The reference always
	 * writes a full 256-entry table whatever the sample depth, so a short
	 * map is padded out with zeros to reach it. */
	if (photo == 3) {
		unsigned char *cm = NULL;
		uint32_t cn = 0, ents;

		if (tu_get_bytes(t, d, TAG_COLORMAP, &cm, &cn) == 0 && cm != NULL &&
		    cn > 0) {
			ents = cn / 3;
			if (ents == 0)
				ents = 1;
			if (ents < 256) {
				/* Grow to 256 entries per channel.  The map
				 * holds reds, then greens, then blues, so
				 * the padding has to be laid down in three
				 * blocks rather than appended. */
				unsigned char *big = calloc(256 * 3, 2);
				if (big != NULL) {
					for (uint32_t ch = 0; ch < 3; ch++)
						for (uint32_t i = 0; i < ents &&
						    i < 256; i++) {
							big[2 * (ch * 256 + i)] =
							    cm[2 * (ch * ents + i)];
							big[2 * (ch * 256 + i) + 1] =
							    cm[2 * (ch * ents + i) + 1];
						}
					free(cm);
					cm = big;
					cn = 256 * 3;
				}
			}
			if (t->be) {
				im->cmap = cm;
				cm = NULL;
			} else {
				im->cmap = malloc((size_t)cn * 2);
				if (im->cmap != NULL) {
					for (uint32_t i = 0; i < cn; i++) {
						im->cmap[2 * i] = cm[2 * i + 1];
						im->cmap[2 * i + 1] = cm[2 * i];
					}
					free(cm);
					cm = NULL;
				}
			}
			if (im->cmap != NULL)
				im->cmapcount = cn;
		}
		free(cm);
	}
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
	/* Three-sample photometric 6 is the one colour space the reference tool
	 * converts rather than repacks.  Its coefficients default to the ITU-R
	 * BT.601 set and tag 529 replaces them with three rationals; the
	 * subsampling and reference-black-white tags are read by the reference
	 * and then ignored, since it always treats the data as 2x2 and never
	 * scales the luma. */
	if (photo == 6 && bps == 8 && srcspp == 3 && rawlen > 0 &&
	    im->unusable == 0 && im->unopenable == 0) {
		double kc[3] = { 0.299, 0.587, 0.114 };
		unsigned char *cf = NULL;
		uint32_t nc = 0;

		if (tu_get_bytes(t, d, TAG_YCBCRCOEFFICIENT, &cf, &nc) == 0 &&
		    nc >= 3) {
			int i;

			for (i = 0; i < 3; i++) {
				uint32_t num = rd_be32(cf + i * 8, t->be);
				uint32_t den = rd_be32(cf + i * 8 + 4, t->be);

				if (den != 0)
					kc[i] = (double)num / (double)den;
			}
		}
		free(cf);
		ycbcr_to_rgb(&raw, &rawlen, w, h, kc[0], kc[1], kc[2]);
	}

	/* LogLuv is not decoded here, and nothing below is a measurement of what
	 * the reference tool does with it.  The reference hands the file to
	 * ImageIO and lets ImageIO decode it, so there is no arithmetic in it to
	 * port and the numbers have to come from the reference tool's output
	 * alone.  Nothing can be pinned until a LogLuv file can be built at all,
	 * and one cannot be built by hand: the codec reads its own bit stream, so
	 * samples laid out in any byte order we can write decode to noise.  The
	 * harness has no LogLuv file for the same reason.  Until it does, read the
	 * white below as a placeholder rather than as the reference tool's
	 * behaviour. */
	if (photo == 32845 && bps == 8 && rawlen > 0)
		memset(raw, 0xff, rawlen);

	im->px = raw;
	im->pxlen = rawlen;
	return 0;
oom:
	free(raw);
	*err = 1;
	return -1;
}

/*
 * The compression an image actually gets, which is not always the one the
 * command line asked for.  Two narrow depths override it, and both overrides
 * belong to the writer rather than to any one operation: a rewrite and a
 * concatenate of the same file come out byte for byte the same, so the
 * decision has to be made in one place both paths call.
 *
 * Returns the compression and, for the depths that change width on the way
 * out, records it on the image for the writer.
 */
static int
select_compression(tuimg_t *im, int requested)
{
	/* One bit of palette is the one narrow depth whose width the
	 * reference tool widens without ever packing the result back
	 * down, so a compressed request is quietly dropped and the strip
	 * goes out whole.  One bit of colour is widened like any other
	 * colour, so it does not land here either. */
	if (im->srcbps == 1 && im->photometric == 3)
		return COMP_NONE;
	/* One bit of gray instead keeps its own width, and always goes
	 * to the bilevel compressor: every output mode collapses to the
	 * same Group 4 strip whatever was asked for.  Restricted to a
	 * lone colour sample, the only shape that has been pinned down,
	 * because g4_encode reads one byte per pixel and anything wider
	 * would hand it the wrong stride. */
	if (im->srcbps == 1 && im->spp == 1 &&
	    (im->photometric == 0 || im->photometric == 1)) {
		im->outbps = 1;
		return COMP_G4;
	}
	return requested;
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

	if (compression == COMP_G4) {
		/* One bit of gray is written one bit wide as Group 4, whatever
		 * output mode was asked for.  load_image has already widened its
		 * samples to a byte each, which is what g4_encode takes. */
		if (g4_encode(im->px, im->width, im->height,
		    im->photometric == 0, &st, &stlen) < 0)
			return -1;
		*predictor = 1;
		*out = st;
		*outlen = stlen;
		return 0;
	}

	if (tiff_build_strip(im->px, im->pxlen, im->bps, im->spp, im->width,
	    pred, swap, &raw, &rawlen) < 0)
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
 * exists but is not a TIFF, and words the two failures differently.  blame is
 * the name it names in the follow-up line, which is not always the file that
 * failed: with several inputs it always names the first one. */
static void
report_missing_source(const char *path)
{
	fprintf(stderr, "Error: Failed to create image source for file "
	    "%s. Either it isn't a TIFF file, or there are unrecognized "
	    "tags; try tiffutil -dump for more info.\n", path);
}

/* The reference tool says nothing about *why* an input was refused in the
 * write operations: only the refusal itself is reported, and only a file
 * that is not there at all is named as missing. */
static int
report_open_failure(const tiff_t *t, const char *path, const char *blame)
{
	if (t->openerc == TUFF_ENOENT || t->openerc == TUFF_EDIR) {
		report_missing_source(path);
		fprintf(stderr, "Error: Can't read from file %s.\n", blame);
		fprintf(stderr, "No output file created due to errors.\n");
	} else {
		fprintf(stderr, "Error: Can't open %s. Either it isn't a TIFF "
		    "file, or there are unrecognized tags; try tiffutil -dump for "
		    "more info.\n", path);
		fprintf(stderr, "No output file created due to errors.\n");
	}
	return 5;
}

/* A file whose every directory carries a photometric the reference tool does
 * not know is refused the same way a file that is not a TIFF at all is, even
 * though its directories parse cleanly. */
static int
report_unrecognized_photometric(const char *path)
{
	fprintf(stderr, "Error: Can't open %s. Either it isn't a TIFF "
	    "file, or there are unrecognized tags; try tiffutil -dump for "
	    "more info.\n", path);
	fprintf(stderr, "No output file created due to errors.\n");
	return 5;
}

static int
write_operations(const char *cmd, const char *inpath, const char *outpath)
{
	tiff_t t;
	int err = 0, compression = COMP_NONE, rc = 0;
	int nd = 0, nkeep = 0, i, unopenable = 0;
	tuwrite_t *items = NULL;
	unsigned char **strips = NULL;
	tuimg_t *ims = NULL;

	if (strcmp(cmd, "-lzw") == 0)
		compression = COMP_LZW;
	else if (strcmp(cmd, "-packbits") == 0)
		compression = COMP_PACKBITS;

	if (tiff_open_file(&t, inpath) < 0) {
		int rc = report_open_failure(&t, inpath, inpath);
		tiff_close(&t);
		return rc;
	}

	/* A rewrite keeps every directory, not just the first, so the file comes
	 * out with the same number of images it went in with. */
	nd = t.ndir;
	items = calloc((size_t)nd, sizeof(*items));
	strips = calloc((size_t)nd, sizeof(*strips));
	ims = calloc((size_t)nd, sizeof(*ims));
	if (items == NULL || strips == NULL || ims == NULL) {
		rc = 1;
		goto done;
	}
	/* A directory whose photometric means nothing to the reference tool is
	 * not an image at all: it never becomes a source, so it is left out of
	 * both the file and the count.  A file made up of nothing but those is
	 * refused outright rather than written empty. */
	for (i = 0; i < nd; i++) {
		unsigned char *strip = NULL;
		size_t striplen = 0;
		int predictor = 1;
		int thiscomp = compression;

		if (load_image(&t, i, &ims[i], &err) < 0) {
			rc = err;
			goto done;
		}
		if (ims[i].unopenable) {
			unopenable = 1;
			continue;
		}
		/* The narrow depths get a compression of their own, chosen
		 * the same way a concatenate chooses it. */
		thiscomp = select_compression(&ims[i], compression);
		if (encode_one(&ims[i], thiscomp, &strip, &striplen,
		    &predictor) < 0) {
			rc = 1;
			goto done;
		}
		strips[nkeep] = strip;
		items[nkeep].im = ims[i];
		items[nkeep].strip = strip;
		items[nkeep].striplen = striplen;
		items[nkeep].compression = thiscomp;
		items[nkeep].predictor = predictor;
		items[nkeep].desc = NULL;
		items[nkeep].software = NULL;
		nkeep++;
	}
	if (nkeep == 0 && unopenable) {
		rc = report_unrecognized_photometric(inpath);
		goto done;
	}
	if (tiff_write_images(outpath, items, nkeep) < 0)
		rc = 1;
done:
	for (i = 0; i < nd; i++) {
		if (strips != NULL)
			free(strips[i]);
		if (ims != NULL) {
			free(ims[i].px);
			free(ims[i].cmap);
		}
	}
	free(items);
	free(strips);
	free(ims);
	tiff_close(&t);
	if (rc == 0)
		fprintf(stderr, "%d image%s written to %s.\n", nkeep,
		    nkeep == 1 ? "" : "s", outpath);
	return rc;
}

enum tu_op {
	OP_NONE, OP_LZW, OP_PACKBITS, OP_CAT, OP_CATNOSIZECHECK,
	OP_CATHIDPICHECK, OP_EXTRACT, OP_INFO, OP_VERBOSEINFO, OP_DUMP
};

/* The reference tool does not relocate a source file's directories into place.
   Concatenating a single file reproduces its rewrite byte for byte, so cat
   collects every directory of every input and writes them out through the same
   writer a -none rewrite uses.  What differs per mode is the encoding and the
   size advice: the hidpi variant LZW-encodes and records provenance, and the
   two size-checking modes differ in which complaint they make. */
typedef struct {
	const char *path;
	int index;
	uint32_t width, height, xres, yres;
} catsrc_t;

/* The point size decides whether two images are "the same size", so the
   comparison is done on the resolution rather than the pixel count. */
static int
cat_same_points(const catsrc_t *a, const catsrc_t *b)
{
	unsigned long long aw, bw, ah, bh;

	if (a->xres == 0 || b->xres == 0 || a->yres == 0 || b->yres == 0)
		return 0;
	aw = (unsigned long long)a->width * b->xres;
	bw = (unsigned long long)b->width * a->xres;
	ah = (unsigned long long)a->height * b->yres;
	bh = (unsigned long long)b->height * a->yres;
	return aw == bw && ah == bh;
}

/* The hidpi layout the tool asks for: two images, one exactly twice the
   other's pixel width and height, in either order. */
static int
cat_hidpi_pair(const catsrc_t *a, const catsrc_t *b)
{
	return (a->width == b->width * 2 && a->height == b->height * 2) ||
	    (b->width == a->width * 2 && b->height == a->height * 2);
}

/* refmissing: the first name on the command line was not a TIFF at all, so
 * the run has no reference size to compare against and every image is
 * reported. */
static void
cat_report_sizes(const catsrc_t *srcs, int n, int op, int refmissing)
{
	int i, bad = 0;

	if (op == OP_CATNOSIZECHECK)
		return;
	if (op == OP_CATHIDPICHECK) {
		bad = refmissing
		    ? 1
		    : n > 1 && (n != 2 || !cat_hidpi_pair(&srcs[0], &srcs[1]));
		if (bad)
			fprintf(stderr, "Warning: Sizes of concatenated images do "
			    "not follow Aqua guidelines for resolution independent "
			    "multi-image TIFFs.\n         Please provide two images, "
			    "one with exactly twice the pixel width as the "
			    "other.\n");
	} else {
		for (i = 1; i < n; i++)
			if (!cat_same_points(&srcs[0], &srcs[i]))
				bad = 1;
		if (refmissing)
			bad = n > 0;
		if (bad)
			fprintf(stderr, "Warning: Sizes of concatenated images are "
			    "not the same; this will lead to problems in choosing "
			    "the appropriate image in some cases.\n");
	}
	if (!bad)
		return;
	/* Height is reported before width, and points are scaled by 72. */
	for (i = 0; i < n; i++)
		fprintf(stderr, " Image %d in file %s: %gx%g points (%ux%u pixels, "
		    "%ux%u dpi)\n", srcs[i].index, srcs[i].path,
		    (double)srcs[i].height * 72.0 / (double)srcs[i].yres,
		    (double)srcs[i].width * 72.0 / (double)srcs[i].xres,
		    srcs[i].height, srcs[i].width, srcs[i].xres, srcs[i].yres);
}

static int
cat_operations(int op, char **paths, int npaths, const char *outpath)
{
	tuwrite_t *items = NULL;
	unsigned char **strips = NULL;
	tuimg_t *ims = NULL;
	catsrc_t *srcs = NULL;
	const char **skipped = NULL;
	int total = 0, cap = 0, nskip = 0, refmissing = 0, failed = 0, rc = 0, i;
	int comp = op == OP_CATHIDPICHECK ? COMP_LZW : COMP_NONE;

	for (i = 0; i < npaths; i++) {
		tiff_t t;
		int err = 0;

		if (tiff_open_file(&t, paths[i]) < 0) {
			/* A name that is not a TIFF is passed over and the
			 * rest of the command carries on; a name that is not
			 * there at all is reported on the spot but does not
			 * stop the rest from being read either -- it only
			 * means nothing gets written.  The diagnostics for
			 * the passed-over names come out after the size
			 * report, so they are held until then. */
			if (t.openerc != TUFF_ENOENT && t.openerc != TUFF_EDIR) {
				const char **ns = realloc(skipped,
				    (size_t)(nskip + 1) * sizeof(*ns));

				if (ns == NULL) {
					tiff_close(&t);
					rc = 1;
					goto done;
				}
				skipped = ns;
				skipped[nskip++] = paths[i];
				if (i == 0)
					refmissing = 1;
				tiff_close(&t);
				continue;
			}
			report_missing_source(paths[i]);
			failed = 1;
			tiff_close(&t);
			continue;
		}
		if (t.ndir > 0 && total + t.ndir > cap) {
			int ncap = cap == 0 ? 8 : cap * 2;

			while (ncap < total + t.ndir)
				ncap *= 2;
			items = realloc(items, (size_t)ncap * sizeof(*items));
			strips = realloc(strips, (size_t)ncap * sizeof(*strips));
			ims = realloc(ims, (size_t)ncap * sizeof(*ims));
			srcs = realloc(srcs, (size_t)ncap * sizeof(*srcs));
			if (items == NULL || strips == NULL || ims == NULL ||
			    srcs == NULL) {
				tiff_close(&t);
				rc = 1;
				goto done;
			}
			cap = ncap;
		}
		for (int d = 0; d < t.ndir; d++) {
			unsigned char *strip = NULL;
			size_t striplen = 0;
			int predictor = 1;
			int thiscomp;

			if (load_image(&t, d, &ims[total], &err) < 0) {
				rc = err;
				tiff_close(&t);
				goto done;
			}
			/* The same per-image override a rewrite applies, so
			 * that concatenating one file reproduces its rewrite
			 * byte for byte. */
			thiscomp = select_compression(&ims[total], comp);
			if (encode_one(&ims[total], thiscomp, &strip, &striplen,
			    &predictor) < 0) {
				tiff_close(&t);
				rc = 1;
				goto done;
			}
			srcs[total].path = paths[i];
			srcs[total].index = d + 1;
			srcs[total].width = ims[total].width;
			srcs[total].height = ims[total].height;
			srcs[total].xres = ims[total].xres;
			srcs[total].yres = ims[total].yres;
			strips[total] = strip;
			items[total].im = ims[total];
			items[total].strip = strip;
			items[total].striplen = striplen;
			items[total].compression = thiscomp;
			items[total].predictor = predictor;
			/* The hidpi writer records where each image came from
			 * and which release produced the file. */
			items[total].desc = comp == COMP_LZW ? paths[i] : NULL;
			items[total].software =
			    comp == COMP_LZW ? "tiffutil v350" : NULL;
			total++;
		}
		tiff_close(&t);
	}

	if (total > 0)
		cat_report_sizes(srcs, total, op, refmissing);
	for (i = 0; i < nskip; i++)
		fprintf(stderr, "Error: Can't open %s. Either it isn't a TIFF "
		    "file, or there are unrecognized tags; try tiffutil -dump "
		    "for more info.\n", skipped[i]);

	/* Nothing loaded at all, or a name that was not there, is the one case
	 * with no output to show. */
	if (failed || total == 0) {
		if (failed)
			fprintf(stderr, "Error: Can't read from file %s.\n",
			    paths[0]);
		fprintf(stderr, "No output file created due to errors.\n");
		rc = 5;
		goto done;
	}
	if (tiff_write_images(outpath, items, total) < 0)
		rc = 1;
	else
		fprintf(stderr, "%d image%s written to %s.\n", total,
		    total == 1 ? "" : "s", outpath);
done:
	for (i = 0; i < total; i++) {
		if (strips != NULL)
			free(strips[i]);
		if (ims != NULL) {
			free(ims[i].px);
			free(ims[i].cmap);
		}
	}
	free(items);
	free(strips);
	free(ims);
	free(srcs);
	free(skipped);
	return rc;
}


/* Same order as the usage block.  is_report: -info/-verboseinfo/-dump, which
   read every remaining argument and reject -out.  is_cat: the concatenation
   family, which is the only kind that takes more than one input name. */
static const struct {
	const char *name;
	int is_report;
	int is_cat;
} op_table[] = {
	{ "-none",           0, 0 },
	{ "-lzw",            0, 0 },
	{ "-packbits",       0, 0 },
	{ "-cat",            0, 1 },
	{ "-catnosizecheck", 0, 1 },
	{ "-cathidpicheck",  0, 1 },
	{ "-extract",        0, 0 },
	{ "-info",           1, 0 },
	{ "-verboseinfo",    1, 0 },
	{ "-dump",           1, 0 }
};

static int
lookup_op(const char *s)
{
	unsigned int i;

	for (i = 0; i < sizeof(op_table) / sizeof(op_table[0]); i++)
		if (strcmp(s, op_table[i].name) == 0)
			return (int)i;
	return -1;
}

static int
run_reports(int op, int argc, char **argv, int first)
{
	int i, rc = 0;

	for (i = first; i < argc; i++) {
		int r;

		if (op == OP_DUMP)
			r = tu_cmd_dump(argv[i]);
		else
			r = tu_cmd_info(argv[i], op == OP_VERBOSEINFO);
		if (r != 0)
			rc = r;
	}
	/* -info and -verboseinfo report their failures inline and still exit 0;
	   -dump is the one that lets a failure reach the exit status. */
	return rc;
}

int
main(int argc, char **argv)
{
	const char *cmd, *inpath = NULL, *outpath = "out.tiff";
	int have_extract = 0, is_extract, op, i, ninfile = 0;

	/* A lone argument is never enough to name an operation, so the reference
	   tool answers it with the bare usage block rather than a complaint --
	   even when that one argument looks like an option. */
	if (argc < 3)
		return usage_error(NULL);

	op = lookup_op(argv[1]);
	if (op < 0)
		return usage_error("No valid command provided.");
	cmd = op_table[op].name;

	i = 2;
	if (op == OP_EXTRACT) {
		char *end;
		long n;

		if (i >= argc)
			return usage_error(NULL);
		n = strtol(argv[i], &end, 10);
		if (end == argv[i] || *end != '\0' || n < 0)
			return usage_error("Image number to be extracted expected.");
		have_extract = (int)n;
		is_extract = 1;
		i++;
	} else {
		is_extract = 0;
	}

	if (i >= argc)
		return is_extract ? usage_error("Input file name expected.") :
		    usage_error(NULL);

	if (op_table[op].is_report) {
		/* The first name is taken positionally even when it looks like
		   an option, so it is never mistaken for a rejected -out; the
		   reference tool only objects to -out further along. */
		for (; i < argc; i++)
			if (i > 2 && strcmp(argv[i], "-out") == 0)
				return usage_error("Can't specify output file name for -info, -verboseinfo, or -dump.");
		return run_reports(op, argc, argv, 2);
	}

	/* Everything else writes a file.  The names are positional, so -out is
	   only ever read as a keyword, never as an input; a second name past the
	   limit is an error rather than something to be silently dropped. */
	{
		int max_infile = op_table[op].is_cat ? 0x7fffffff : 1;

		for (; i < argc;) {
			if (strcmp(argv[i], "-out") == 0) {
				if (i + 1 >= argc)
					return usage_error("One input file name expected.");
				outpath = argv[i + 1];
				i += 2;
				/* -out closes the command line: anything after the
				   file name it introduces is a spare input. */
				if (i < argc)
					return usage_error("One input file name expected.");
				continue;
			}
			if (ninfile == 0)
				inpath = argv[i];
			ninfile++;
			if (ninfile > max_infile)
				return usage_error("One input file name expected.");
			i++;
		}
		if (ninfile == 0)
			return usage_error(is_extract ?
			    "Input file name expected." :
			    "One input file name expected.");
	}

	if (op == OP_INFO || op == OP_VERBOSEINFO || op == OP_DUMP)
		return run_reports(op, argc, argv, 2);
	if (op == OP_CAT || op == OP_CATNOSIZECHECK || op == OP_CATHIDPICHECK) {
		char **paths;
		int n = 0, r;

		paths = calloc((size_t)ninfile, sizeof(*paths));
		if (paths == NULL)
			return 1;
		/* Names are positional, so -out and the name it introduces are
		 * the only arguments to skip. */
		for (int i = 2; i < argc; i++) {
			if (strcmp(argv[i], "-out") == 0) {
				i++;
				continue;
			}
			if (argv[i][0] != '-')
				paths[n++] = argv[i];
		}
		r = cat_operations(op, paths, n, outpath);
		free(paths);
		return r;
	}
	if (is_extract) {
		tiff_t t;
		if (tiff_open_file(&t, inpath) < 0) {
			int rc = report_open_failure(&t, inpath, inpath);
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
			/* The narrow depth overrides still apply, and they are
			 * made in one place so that an extract and a rewrite
			 * of the same directory agree. */
			src_comp = select_compression(&im, src_comp);
			/* The pixels are in host order here, so they still have to
			 * go through the strip builder; that is what puts 16-bit
			 * samples back into big-endian. */
			if (encode_one(&im, src_comp, &strip, &striplen,
			    &pred) < 0) {
				free(im.px);
				free(im.cmap);
				tiff_close(&t);
				return 1;
			}
			if (tiff_write_image(outpath, &im, strip, striplen,
			    src_comp, pred) < 0) {
				free(strip);
				free(im.px);
				free(im.cmap);
				tiff_close(&t);
				return 1;
			}
			free(strip);
			free(im.px);
			free(im.cmap);
		fprintf(stderr, "1 image written to %s.\n", outpath);
		}
		tiff_close(&t);
		return 0;
	}
	return write_operations(cmd, inpath, outpath);
}
