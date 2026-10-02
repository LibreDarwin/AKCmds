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
	uint32_t outphoto;           /* the photometric the rewrite carries */
	uint32_t rows_per_strip;
	uint32_t xres, yres;
	const unsigned char *strip;
	size_t striplen;
	const unsigned char *cmap;
	uint32_t cmapcount;
	const char *desc;
	const char *software;
	uint16_t xalpha;
	int floatout;                /* samples are IEEE float, not integers */
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
 * big-endian, so nothing here transforms samples.  Offsets are absolute, so
 * the caller can append a chain of directories.  first says whether this is
 * the head of the file, in which case the 8-byte header is emitted here.  The
 * directory's own offset and the file position of its next-pointer field come
 * back so the caller can link this directory to the next one.
 */
static int
write_dir(buf_t *out, const wimg_t *im, int compression, int predictor,
    const unsigned char *icc, size_t icclen, int first, uint32_t *ifd_off_out,
    size_t *ifd_pos_out)
{
	unsigned char bps_ext[MAX_SPP * 2];
	unsigned char fmt_ext[MAX_SPP * 2];
	unsigned char xr[8], yr[8], xs_ext[MAX_SPP * 2];
	wfield_t f[32];
	int placed[32], extseq[32], next_;
	unsigned char extpad[32];
	int nf = 0, n = 0;
	size_t pad;
	uint32_t ifd_off, ncolor = 0;
	size_t ext_base, cur;
	uint32_t strip_abs;

	/* Where this directory's strip is about to land.  The header is only
	 * half written at this point, so account for its second word. */
	if (first && buf_u16(out, 0x4d4d) < 0)
		return -1;
	if (first && buf_u16(out, 42) < 0)
		return -1;
	strip_abs = (uint32_t)out->len + (first ? 4u : 0u);

	/* The directory's own resolution is carried into the output, written as
	 * a whole number over one.  A zero here is a real value, not a missing
	 * one: load_image has already substituted the default where needed. */
	be32(xr, im->xres); be32(xr + 4, 1);
	be32(yr, im->yres); be32(yr + 4, 1);
	ncolor = tiff_color_channels(im->photometric);

	for (uint32_t i = 0; i < im->spp && i < MAX_SPP; i++)
		be16(bps_ext + 2 * i, im->bps);
	/* SampleFormat 3 is IEEE float, and the only output that carries it is
	 * the LogLuv one: every other colour space is handed back as integers. */
	for (uint32_t i = 0; i < im->spp && i < MAX_SPP; i++)
		be16(fmt_ext + 2 * i, im->floatout ? 3 : 1);

	/* Fields are appended in ascending tag order. */
	f[nf++] = (wfield_t){TAG_IMAGEWIDTH, 3, 1, im->width, NULL, 0};
	f[nf++] = (wfield_t){TAG_IMAGELENGTH, 3, 1, im->height, NULL, 0};
	f[nf++] = (wfield_t){TAG_BITSPERSAMPLE, 3, im->spp, 0,
	    bps_ext, (size_t)im->spp * 2};
	f[nf++] = (wfield_t){TAG_COMPRESSION, 3, 1, (uint32_t)compression, NULL, 0};
	f[nf++] = (wfield_t){TAG_PHOTOMETRIC, 3, 1, im->outphoto, NULL, 0};
	f[nf++] = (wfield_t){TAG_FILLORDER, 3, 1, 1, NULL, 0};
	if (im->desc != NULL)
		f[nf++] = (wfield_t){TAG_IMAGEDESCRIPTION, 2,
		    (uint32_t)strlen(im->desc) + 1, 0,
		    (const unsigned char *)im->desc, strlen(im->desc) + 1};
	f[nf++] = (wfield_t){TAG_STRIPOFFSETS, 4, 1, strip_abs, NULL, 0};
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
	if (im->software != NULL)
		f[nf++] = (wfield_t){TAG_SOFTWARE, 2,
		    (uint32_t)strlen(im->software) + 1, 0,
		    (const unsigned char *)im->software, strlen(im->software) + 1};
	/* 317 sorts before 338, so the predictor is queued ahead of the
	 * extra samples. It is emitted for every predicted file, not only
	 * the ones that also carry extra samples. */
	if (predictor == 2)
		f[nf++] = (wfield_t){TAG_PREDICTOR, 3, 1, 2, NULL, 0};
	/* A palette is carried through untouched: the reference tool copies
	 * the source's colour map rather than synthesising one. */
	if (im->cmap != NULL && im->cmapcount > 0)
		f[nf++] = (wfield_t){TAG_COLORMAP, 3, im->cmapcount, 0,
		    im->cmap, (size_t)im->cmapcount * 2};
	/* One extra channel, carrying the source's own association value or
	 * unassociated alpha when the input did not say.  320 sorts ahead of
	 * 338, so a palette entry is queued before this one. */
	if (ncolor && im->spp > ncolor) {
		be16(xs_ext, im->xalpha);
		f[nf++] = (wfield_t){TAG_EXTRASAMPLES, 3, 1, 0,
		    xs_ext, 2};
	}
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
			TAG_XRESOLUTION, TAG_YRESOLUTION,
			TAG_BITSPERSAMPLE, TAG_SAMPLEFORMAT,
			TAG_COLORMAP, TAG_EXTRASAMPLES,
			TAG_IMAGEDESCRIPTION, TAG_SOFTWARE,
			TAG_ICCPROFILE
		};

		/* The directory starts on an even offset, so an odd-length strip
		 * is followed by a pad byte.  The head's strip sits straight after
		 * the 8-byte header, a later one wherever the caller has got to. */
		pad = (strip_abs + im->striplen) & 1;
		ifd_off = (uint32_t)(strip_abs + im->striplen + pad);
		ext_base = strip_abs + im->striplen + pad + 2 + (size_t)12 * nf + 4;
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
				/* Every out-of-line block begins on an even
				 * offset, so an odd-length one is followed
				 * by a pad byte. */
				if (cur & 1) {
					cur++;
					extpad[next_] = 1;
				} else {
					extpad[next_] = 0;
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

	if (first && buf_u32(out, ifd_off) < 0)
		return -1;
	if (buf_put(out, im->strip, im->striplen) < 0)
		return -1;
	if (pad && buf_u8(out, 0) < 0)
		return -1;
	*ifd_off_out = ifd_off;
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
	*ifd_pos_out = out->len;      /* where the next-pointer field begins */
	if (buf_u32(out, 0) < 0)
		return -1;
	for (int k = 0; k < next_; k++) {
		int i = extseq[k];
		if (extpad[k] && buf_u8(out, 0) < 0)
			return -1;
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
    uint32_t spp, uint32_t width, int predictor,
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
tiff_write_images(const char *path, const tuwrite_t *items, int n)
{
	buf_t out = {NULL, 0, 0};
	FILE *f;
	int rc = 0, nwritten = 0;
	size_t prev_next_pos = 0;

	for (int i = 0; i < n; i++) {
		wimg_t m;
		unsigned char labscratch[496];
		const unsigned char *icc = NULL;
		size_t icclen = 0, ifd_pos = 0;
		uint32_t ifd_off = 0;

		/* A directory the reference tool could not make an image out of
		 * is dropped, leaving an empty file when it was the only one,
		 * and the run is still called a success. */
		if (items[i].im.unusable)
			continue;

		/* The float output gets its own profile rather than the RGB
		 * one, even though both are photometric 2. */
		if (items[i].im.floatout) {
			icc = iccProfileFloat;
			icclen = sizeof(iccProfileFloat);
		} else {
			tiff_icc_for((int)items[i].im.outphoto, labscratch,
			    sizeof(labscratch), &icc, &icclen);
		}
		m.width = items[i].im.width;
		m.height = items[i].im.height;
		m.bps = items[i].im.outbps != 0 ? items[i].im.outbps :
		    items[i].im.bps;
		m.spp = items[i].im.spp;
		m.photometric = items[i].im.outphoto;
		m.outphoto = items[i].im.outphoto;
		m.rows_per_strip = items[i].im.rows_per_strip;
		m.xres = items[i].im.xres;
		m.yres = items[i].im.yres;
		m.cmap = items[i].im.cmap;
		m.cmapcount = items[i].im.cmapcount;
		m.strip = items[i].strip;
		m.striplen = items[i].striplen;
		m.desc = items[i].desc;
		m.software = items[i].software;
		m.xalpha = items[i].im.xalpha;
		m.floatout = items[i].im.floatout;
		if (write_dir(&out, &m, items[i].compression, items[i].predictor,
		    icc, icclen, nwritten == 0, &ifd_off, &ifd_pos) < 0) {
			buf_free(&out);
			return -1;
		}
		/* Point the previous directory at this one, now that this one's
		 * offset is known.  The last directory keeps the zero that
		 * write_dir already emitted for it. */
		if (nwritten > 0 && prev_next_pos != 0)
			be32(out.p + prev_next_pos, ifd_off);
		prev_next_pos = ifd_pos;
		nwritten++;
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

int
tiff_write_image(const char *path, const tuimg_t *im, const unsigned char *strip,
    size_t striplen, int compression, int predictor)
{
	tuwrite_t one;

	one.im = *im;
	one.strip = strip;
	one.striplen = striplen;
	one.compression = compression;
	one.predictor = predictor;
	one.desc = NULL;
	one.software = NULL;
	return tiff_write_images(path, &one, 1);
}

uint32_t
tiff_out_photometric(uint32_t photo, uint32_t spp, uint32_t bps)
{
	/* A single bit of gray is the one depth that keeps its own name on the
	 * way out, because the reference repacks it and hands it to the bilevel
	 * compressor rather than widening it like everything else. */
	if (bps == 1 && (photo == 0 || photo == 1))
		return 1;
	/* Two of the rest turn on the pixel rather than the space.  WhiteIsZero
	 * survives only for a single sample: with more than one the reference
	 * calls the result BlackIsZero, and LogL is gray for one sample and RGB
	 * for three or four. */
	if (photo == 0)
		return spp == 1 ? 0 : 1;
	if (photo == 6 || photo == 32845)
		return 2;
	/* All three CIE Lab encodings are laid down as plain Lab. */
	if (photo == 9 || photo == 10)
		return 8;
	if (photo == 32844)
		return spp == 1 ? 1 : 2;
	/* A palette keeps its own name as well as its colour map, because the
	 * samples are indices and stay that way. */
	return photo;
}

int
tiff_color_channels(uint32_t photometric)
{
	switch (photometric) {
	case 0: case 1:            return 1;   /* black/white is zero */
	case 2: case 6:            return 3;   /* RGB, YCbCr */
	case 3:                   return 1;   /* palette: the samples are indices */
	case 5:                    return 4;   /* CMYK */
	case 8: case 9: case 10:   return 3;   /* the three CIE Lab encodings */
	case 32844:               return 3;   /* LogL: one sample is gray, three are
	                                        * read as colour, as many as four
	                                        * keep a spare channel */
	case 32845:               return 3;   /* LogLuv */
	default:                   return 0;   /* a mask, or nothing recognised */
	}
}

int
tiff_predictor_for(int compression, uint32_t bps)
{
	/* Observed: Predictor 2 is tagged for LZW at 8 bits per sample only. */
	return compression == COMP_LZW && bps == 8 ? 2 : 1;
}

/* The Lab profile is rebuilt per image and carries the moment it was made, so
 * the template's zeroed date field is filled in here.  The ICC header spells
 * the date out as six 16-bit big-endian numbers: year, month, day, hour,
 * minute, second.  The profile is written as UTC, as the specification says,
 * which cannot be checked against the reference on a host whose local time
 * happens to be UTC; see NOTES.md. */
static void
tiff_stamp_lab(unsigned char *p)
{
	time_t now = time(NULL);
	struct tm tm;
	uint16_t v[6];
	int i;

	if (gmtime_r(&now, &tm) == NULL)
		return;
	v[0] = (uint16_t)tm.tm_year + 1900;
	v[1] = (uint16_t)tm.tm_mon + 1;
	v[2] = (uint16_t)tm.tm_mday;
	v[3] = (uint16_t)tm.tm_hour;
	v[4] = (uint16_t)tm.tm_min;
	v[5] = (uint16_t)tm.tm_sec;
	for (i = 0; i < 6; i++) {
		p[24 + i * 2] = (unsigned char)(v[i] >> 8);
		p[24 + i * 2 + 1] = (unsigned char)(v[i] & 0xff);
	}
}

const unsigned char *
tiff_icc_for(int photometric, unsigned char *scratch, size_t scratchlen,
             const unsigned char **p, size_t *len)
{
	if (photometric == 0 || photometric == 1) {
		*p = iccProfileGray;
		*len = sizeof(iccProfileGray);
	} else if (photometric == 5) {
		*p = iccProfileCMYK;
		*len = sizeof(iccProfileCMYK);
	} else if (photometric == 8) {
		if (scratch != NULL && scratchlen == sizeof(iccProfileLab)) {
			memcpy(scratch, iccProfileLab, sizeof(iccProfileLab));
			tiff_stamp_lab(scratch);
			*p = scratch;
		} else {
			*p = iccProfileLab;
		}
		*len = sizeof(iccProfileLab);
	} else {
		*p = iccProfileRGB;
		*len = sizeof(iccProfileRGB);
	}
	return NULL;
}
