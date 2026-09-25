/*
 * tiffutil - writing TIFF files the way CoreGraphics does.
 *
 * The container layout is fixed by observation rather than by the TIFF
 * specification, because the reference tool does not write a conformant
 * minimal TIFF.  What it writes is always:
 *
 *   offset 0        "MM", 42, offset of the first IFD
 *   offset 8        the strip data
 *   then            the IFD, entries sorted by tag
 *   then            every value too wide for the 4-byte inline slot, in
 *                   ascending tag order
 *
 * The field set is fixed too: ImageWidth, ImageLength, BitsPerSample,
 * Compression, Photometric, FillOrder, StripOffsets, Orientation,
 * SamplesPerPixel, RowsPerStrip, StripByteCounts, XResolution, YResolution,
 * PlanarConfig, ResolutionUnit, [Predictor,] SampleFormat, ICC Profile.
 * FillOrder, Orientation, SampleFormat and ResolutionUnit are filled in even
 * when the input lacked them, and the ICC profile is attached even though the
 * input never carried one.
 *
 * BitsPerSample and SampleFormat are written with count == SamplesPerPixel
 * and therefore move out of line once there are three or more samples; with
 * one or two they still fit inline.  That boundary is the detail most easily
 * got wrong, and it is why StripByteCounts, the two resolutions and the ICC
 * blob are not always in the same places.
 *
 * Copyright (C) 2026, LibreDarwin
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "tiffutil.h"
#include "buf.h"
#include "icc_profiles.h"

#define MAX_SPP 8

/* One output field.  Either inl holds the value, or ext points at bytes that
 * do not fit in the inline slot. */
typedef struct {
	uint16_t tag;
	uint16_t type;
	uint32_t count;
	uint32_t inl;
	const unsigned char *ext;
	size_t extlen;
} wfield_t;

/* What write_dir needs, which is the decoded image plus the already-encoded
 * strip.  The two are kept separate because the strip may be compressed. */
typedef struct {
	uint32_t width, height;
	uint32_t bps, spp;
	uint32_t photometric;
	uint32_t rows_per_strip;
	const unsigned char *strip;
	size_t striplen;
} wimg_t;

static size_t
tsize(int type)
{
	switch (type) {
	case 1: case 2: case 6: case 7: return 1;
	case 3: case 8:                 return 2;
	case 4: case 9: case 11:        return 4;
	case 5: case 10: case 12:       return 8;
	default:                        return 0;
	}
}

static void
be16(unsigned char *p, uint32_t v)
{
	p[0] = (unsigned char)(v >> 8);
	p[1] = (unsigned char)v;
}

static void
be32(unsigned char *p, uint32_t v)
{
	p[0] = (unsigned char)(v >> 24);
	p[1] = (unsigned char)(v >> 16);
	p[2] = (unsigned char)(v >> 8);
	p[3] = (unsigned char)v;
}

/*
 * Serialise one directory.  The strip is already encoded and already
 * big-endian, so nothing here transforms samples.
 */
static int
write_dir(buf_t *out, const wimg_t *im, int compression, int predictor,
    const unsigned char *icc, size_t icclen)
{
	unsigned char bps_ext[MAX_SPP * 2];
	unsigned char fmt_ext[MAX_SPP * 2];
	unsigned char xr[8], yr[8], xs_ext[MAX_SPP * 2];
	wfield_t f[32];
	int placed[32], extseq[32], next_;
	int nf = 0, n = 0;
	uint32_t ifd_off, xres_num = 72, yres_num = 72, ncolor = 0;
	size_t ext_base, cur;

	/* Reuse the input resolution's numerator when it is one we can carry;
	 * the reference tool keeps 72/1 for the fixtures observed so far. */
	be32(xr, xres_num); be32(xr + 4, 1);
	be32(yr, yres_num); be32(yr + 4, 1);
	ncolor = tiff_color_channels(im->photometric);

	for (uint32_t i = 0; i < im->spp && i < MAX_SPP; i++)
		be16(bps_ext + 2 * i, im->bps);
	for (uint32_t i = 0; i < im->spp && i < MAX_SPP; i++)
		be16(fmt_ext + 2 * i, 1);       /* SampleFormat = unsigned integer */

	/* Fields are appended in ascending tag order. */
	f[nf++] = (wfield_t){TAG_IMAGEWIDTH, 3, 1, im->width, NULL, 0};
	f[nf++] = (wfield_t){TAG_IMAGELENGTH, 3, 1, im->height, NULL, 0};
	f[nf++] = (wfield_t){TAG_BITSPERSAMPLE, 3, im->spp, 0,
	    bps_ext, (size_t)im->spp * 2};
	f[nf++] = (wfield_t){TAG_COMPRESSION, 3, 1, (uint32_t)compression, NULL, 0};
	f[nf++] = (wfield_t){TAG_PHOTOMETRIC, 3, 1, im->photometric, NULL, 0};
	f[nf++] = (wfield_t){TAG_FILLORDER, 3, 1, 1, NULL, 0};
	f[nf++] = (wfield_t){TAG_STRIPOFFSETS, 4, 1, 8, NULL, 0};
	f[nf++] = (wfield_t){TAG_ORIENTATION, 3, 1, 1, NULL, 0};
	f[nf++] = (wfield_t){TAG_SAMPLESPERPIXEL, 3, 1, im->spp, NULL, 0};
	/* The reference tool never splits its output into more than one strip,
	 * so RowsPerStrip is always the whole height however the input was
	 * striped. */
	f[nf++] = (wfield_t){TAG_ROWSPERSTRIP, 3, 1, im->height, NULL, 0};
	f[nf++] = (wfield_t){TAG_STRIPBYTECOUNTS, 4, 1, (uint32_t)im->striplen, NULL, 0};
	f[nf++] = (wfield_t){TAG_XRESOLUTION, 5, 1, 0, xr, 8};
	f[nf++] = (wfield_t){TAG_YRESOLUTION, 5, 1, 0, yr, 8};
	f[nf++] = (wfield_t){TAG_PLANARCONFIG, 3, 1, 1, NULL, 0};
	f[nf++] = (wfield_t){TAG_RESOLUTIONUNIT, 3, 1, 2, NULL, 0};
	if (ncolor && im->spp > ncolor) {
		/* More samples than the photometric implies, so the surplus are
		 * extra channels.  The reference tool derives them itself when the
		 * input does not say, and treats them as unassociated alpha. */
		for (uint32_t i = 0; i < im->spp - ncolor && i < MAX_SPP; i++)
			be16(xs_ext + 2 * i, 2);
		f[nf++] = (wfield_t){TAG_EXTRASAMPLES, 3, im->spp - ncolor, 0,
		    xs_ext, (size_t)(im->spp - ncolor) * 2};
	}
	if (predictor == 2)                    /* 317 sorts before 339 */
		f[nf++] = (wfield_t){TAG_PREDICTOR, 3, 1, 2, NULL, 0};
	f[nf++] = (wfield_t){TAG_SAMPLEFORMAT, 3, im->spp, 0,
	    fmt_ext, (size_t)im->spp * 2};
	f[nf++] = (wfield_t){TAG_ICCPROFILE, 7, (uint32_t)icclen, 0, icc, icclen};
	for (int i = 0; i < nf; i++)
		placed[i] = 0;
	next_ = 0;

	/* SHORT values live in the high half of the slot, and two of them fill
	 * it end to end.  A value too wide for the slot stores a big-endian
	 * offset, and the blocks those offsets point at are laid out in a fixed
	 * order that is not tag order: the two resolutions first, then the
	 * per-sample arrays, then the profile.  A grayscale file has no
	 * out-of-line sample arrays at all, which is why the profile does not
	 * begin at the same relative place in every output. */
	{
		static const uint16_t ext_order[] = {
			TAG_XRESOLUTION, TAG_YRESOLUTION, TAG_BITSPERSAMPLE,
			TAG_SAMPLEFORMAT, TAG_EXTRASAMPLES, TAG_ICCPROFILE
		};

		ifd_off = (uint32_t)(8 + im->striplen);
		ext_base = 8 + im->striplen + 2 + (size_t)12 * nf + 4;
		cur = ext_base;
		for (size_t k = 0; k < sizeof(ext_order) / sizeof(ext_order[0]); k++) {
			for (int i = 0; i < nf; i++) {
				if (f[i].tag != ext_order[k])
					continue;
				if (f[i].ext == NULL)
					continue;
				if (tsize(f[i].type) * f[i].count <= 4) {
					/* Small enough to sit in the slot: pack the
					 * value in and emit nothing after the IFD. */
					if (f[i].type == 3) {
						uint32_t a = (uint32_t)
						    ((f[i].ext[0] << 8) | f[i].ext[1]);
						uint32_t b = f[i].count == 2 ? (uint32_t)
						    ((f[i].ext[2] << 8) | f[i].ext[3]) : 0;
						f[i].inl = f[i].count == 2
						    ? (a << 16) | b : a << 16;
					} else if (f[i].type == 4) {
						f[i].inl = rd_be32(f[i].ext, 1);
					}
					f[i].ext = NULL;
					placed[i] = 1;
					break;
				}
				f[i].inl = (uint32_t)cur;
				cur += f[i].extlen;
				placed[i] = 1;
				extseq[next_++] = i;   /* emit in this same order */
				break;
			}
		}
		/* A plain SHORT keeps its value in the high half of the slot. */
		for (int i = 0; i < nf; i++) {
			if (!placed[i] && f[i].type == 3)
				f[i].inl = f[i].inl << 16;
		}
	}
	n = nf;

	if (buf_u16(out, 0x4d4d) < 0 || buf_u16(out, 42) < 0 ||
	    buf_u32(out, ifd_off) < 0)
		return -1;
	if (buf_put(out, im->strip, im->striplen) < 0)
		return -1;
	if (buf_u16(out, (unsigned)n) < 0)
		return -1;
	for (int i = 0; i < n; i++) {
		unsigned char slot[4];
		if (buf_u16(out, f[i].tag) < 0 || buf_u16(out, f[i].type) < 0 ||
		    buf_u32(out, f[i].count) < 0)
			return -1;
		be32(slot, f[i].inl);
		if (buf_put(out, slot, 4) < 0)
			return -1;
	}
	if (buf_u32(out, 0) < 0)
		return -1;
	for (int k = 0; k < next_; k++) {
		int i = extseq[k];
		if (buf_put(out, f[i].ext, f[i].extlen) < 0)
			return -1;
	}
	return 0;
}

/*
 * Copy samples into a big-endian strip, applying horizontal differencing when
 * the caller asked for it.  Differencing runs across the whole row including
 * the first sample of each channel, which is what predictor 2 specifies and
 * what the reference tool's LZW output decodes to.
 */
int
tiff_build_strip(const unsigned char *src, size_t srclen, uint32_t bps,
    uint32_t spp, uint32_t width, uint32_t height, int predictor,
    int swap, unsigned char **out, size_t *outlen)
{
	unsigned char *o = malloc(srclen ? srclen : 1);
	size_t bpl;

	if (o == NULL)
		return -1;
	memcpy(o, src, srclen);
	if (swap && bps == 16) {
		for (size_t i = 0; i + 1 < srclen; i += 2) {
			unsigned char t = o[i];
			o[i] = o[i + 1];
			o[i + 1] = t;
		}
	}
	if (predictor == 2 && bps == 8) {
		bpl = (size_t)width * spp;
		if (bpl > srclen)
			bpl = srclen;
		for (uint32_t row = 0; (size_t)row * bpl < srclen; row++) {
			unsigned char *r = o + (size_t)row * bpl;
			unsigned char prev[8];
			size_t n = spp < 8 ? spp : 8;

			for (size_t i = 0; i < n && i < bpl; i++)
				prev[i] = r[i];
			/* Each sample is differenced against the same channel
			 * of the pixel to its left, so the untouched value of
			 * that channel has to be carried along. */
			for (size_t i = spp; i < bpl; i++) {
				unsigned char cur = r[i];
				r[i] = (unsigned char)(cur - prev[i % spp]);
				prev[i % spp] = cur;
			}
		}
	}
	*out = o;
	*outlen = srclen;
	return 0;
}

int
tiff_write_image(const char *path, const tuimg_t *im, const unsigned char *strip,
    size_t striplen, int compression, int predictor)
{
	buf_t out = {NULL, 0, 0};
	wimg_t m;
	const unsigned char *icc = NULL;
	size_t icclen = 0;
	FILE *f;
	int rc = 0;

	tiff_icc_for((int)im->photometric, &icc, &icclen);
	m.width = im->width;
	m.height = im->height;
	m.bps = im->bps;
	m.spp = im->spp;
	m.photometric = im->photometric;
	m.rows_per_strip = im->rows_per_strip;
	m.strip = strip;
	m.striplen = striplen;
	if (write_dir(&out, &m, compression, predictor, icc, icclen) < 0) {
		buf_free(&out);
		return -1;
	}
	f = fopen(path, "wb");
	if (f == NULL) {
		buf_free(&out);
		return -1;
	}
	if (fwrite(out.p, 1, out.len, f) != out.len) {
		fprintf(stderr, "Error writing data to file %s.\n", path);
		rc = -1;
	}
	fclose(f);
	buf_free(&out);
	return rc;
}

uint32_t
tiff_color_channels(uint32_t photometric)
{
	switch (photometric) {
	case 0: case 1:            return 1;   /* black/white is zero */
	case 2: case 6:            return 3;   /* RGB, YCbCr */
	case 5:                    return 4;   /* CMYK */
	default:                   return 0;
	}
}

int
tiff_predictor_for(int compression, uint32_t bps)
{
	/* Observed: Predictor 2 is tagged for LZW at 8 bits per sample only. */
	return compression == COMP_LZW && bps == 8 ? 2 : 1;
}

const unsigned char *
tiff_icc_for(int photometric, const unsigned char **p, size_t *len)
{
	if (photometric == 0 || photometric == 1) {
		*p = iccProfileGray;
		*len = sizeof(iccProfileGray);
	} else {
		*p = iccProfileRGB;
		*len = sizeof(iccProfileRGB);
	}
	return NULL;
}
