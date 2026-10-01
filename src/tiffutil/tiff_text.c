/*
 * tiffutil - the reporting modes.
 *
 * -info and -verboseinfo summarise a directory; -verboseinfo adds the strip
 * table that -info omits.  -dump prints the raw IFD with the same field
 * names, types and count notation the reference tool uses.
 *
 * Copyright (C) 2026, LibreDarwin
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include <stdio.h>
#include <stdlib.h>
#include <errno.h>
#include <string.h>

#include "tiffutil.h"

size_t
tu_type_size(int type)
{
	switch (type) {
	case 1: case 2: case 6: case 7: return 1;
	case 3: case 8:                 return 2;
	case 4: case 9: case 11:        return 4;
	case 5: case 10: case 12:       return 8;
	default:                        return 0;
	}
}

/* The tags whose presence the reference tool does not draw as an unknown
 * field, observed by walking a directory that holds one candidate tag at a
 * time.  A tag outside this set draws a warning while the directory is read,
 * which is a property of reading rather than of the file: -dump walks the
 * raw IFD itself and says nothing, and neither do the converting modes.
 * The set is not the same as the one -dump names tags from -- 347 is named
 * JPEGTables there and reported unknown here. */
static const uint16_t known_tags[] = {
	254, 255, 256, 257, 258, 259, 262, 263, 264, 265, 266, 269, 270, 271,
	272, 273, 274, 277, 278, 279, 280, 281, 282, 283, 284, 285, 286, 287,
	288, 289, 290, 291, 296, 297, 300, 301, 305, 306, 315, 316, 318, 319,
	320, 321, 322, 323, 324, 325, 330, 332, 333, 334, 336, 337, 338, 339,
	340, 341, 343, 344, 345, 346, 400, 401, 402, 403, 404, 405, 433, 434,
	435, 529, 530, 531, 532, 559, 700, 32995, 32996, 32997, 32998, 33300,
	33301, 33302, 33303, 33304, 33305, 33306, 33421, 33422, 33432, 33723,
	34377, 34665, 34675, 34732, 34853, 34908, 34909, 34910, 34911, 37439,
	37724, 40965, 50706, 50707, 50708, 50709, 50710, 50711, 50712, 50713,
	50714, 50715, 50716, 50717, 50718, 50719, 50720, 50721, 50722, 50723,
	50724, 50725, 50726, 50727, 50728, 50729, 50730, 50731, 50732, 50733,
	50734, 50735, 50736, 50737, 50738, 50739, 50740, 50741, 50778, 50779,
	50780, 50781, 50827, 50828, 50829, 50830, 50831, 50832, 50833, 50834,
};

static int
tag_is_known(uint16_t tag, uint32_t compression)
{
	size_t lo = 0, hi = sizeof(known_tags) / sizeof(known_tags[0]);

	while (lo < hi) {
		size_t mid = lo + (hi - lo) / 2;

		if (known_tags[mid] == tag)
			return 1;
		if (known_tags[mid] < tag)
			lo = mid + 1;
		else
			hi = mid;
	}
	/* One tag is named only for the codecs that predict, which covers LZW,
	 * both Deflate spellings and PixarFilm.  Under any other compression
	 * it is left unnamed and draws the warning with the rest. */
	return tag == TAG_PREDICTOR &&
	    (compression == 5 || compression == 8 ||
	     compression == 32909 || compression == 32946);
}

/* The tag vocabulary, spelled as the reference tool spells it. */
static const char *
dump_tag_name(uint16_t tag)
{
	switch (tag) {
	case TAG_IMAGEWIDTH:      return "ImageWidth";
	case TAG_IMAGELENGTH:     return "ImageLength";
	case TAG_BITSPERSAMPLE:   return "BitsPerSample";
	case TAG_COMPRESSION:     return "Compression";
	case TAG_PHOTOMETRIC:     return "Photometric";
	case TAG_FILLORDER:       return "FillOrder";
	case TAG_STRIPOFFSETS:    return "StripOffsets";
	case TAG_ORIENTATION:     return "Orientation";
	case TAG_SAMPLESPERPIXEL: return "SamplesPerPixel";
	case TAG_ROWSPERSTRIP:    return "RowsPerStrip";
	case TAG_STRIPBYTECOUNTS: return "StripByteCounts";
	case TAG_XRESOLUTION:     return "XResolution";
	case TAG_YRESOLUTION:     return "YResolution";
	case TAG_PLANARCONFIG:    return "PlanarConfig";
	case TAG_RESOLUTIONUNIT:  return "ResolutionUnit";
	case TAG_PREDICTOR:       return "Predictor";
	case TAG_EXTRASAMPLES:    return "ExtraSamples";
	case TAG_SAMPLEFORMAT:    return "SampleFormat";
	case TAG_ICCPROFILE:      return "ICC Profile";
	case 254:                 return "NewSubfileType";
	case 270:                 return "Description";
	case 293:                 return "PageNumber";
	case 297:                 return "PageName";
	case 301:                 return "TransferFunction";
	case 305:                 return "Software";
	case 320:                 return "Colormap";
	case 33628:               return "CFAPattern";
	/* JPEGTables is named here and yet still drawn as an unknown field by
	 * -info: the two go through different vocabularies. */
	case 347:                 return "JPEGTables";
	default:                  return NULL;
	}
}

/* Photometric is quoted for the two grayscale cases and left bare for the
 * colour ones, which is a quirk of the reference tool worth keeping. */
static int
photo_channels(uint32_t p)
{
	switch (p) {
	case 0: return 1;		/* WhiteIsZero */
	case 1: return 1;		/* BlackIsZero */
	case 2: return 3;		/* RGB */
	case 3: return 1;		/* RGBPalette */
	case 4: return 4;		/* TransparencyMask, as counted here */
	case 5: return 4;		/* CMYK */
	case 6: return 3;		/* YCbCr */
	case 8: return 3;		/* CIELab */
	case 9: return 3;		/* ICCLab */
	case 10: return 3;		/* ITULab */
	case 32845: return 3;		/* LogLuv */
	default:  return 0;		/* unaccounted for: no warning at all */
	}
}

/* The reference tool names only the five colour spaces it has words for, and
 * even then separated keeps its number in front of the name.  Everything else
 * falls back to the raw value in hex, so the long tail of registered
 * photometrics is not spelled out at all. */
static void
photometric_phrase(uint32_t p, uint32_t spp, char *buf, size_t buflen)
{
	switch (p) {
	case 0: snprintf(buf, buflen, "\"min-is-white\""); break;
	case 1: snprintf(buf, buflen, "\"min-is-black\""); break;
	case 2: snprintf(buf, buflen, "RGB color"); break;
	case 3: snprintf(buf, buflen, "palette color (RGB from colormap)"); break;
	case 4: snprintf(buf, buflen, "transparency mask"); break;
	case 5:
		if (spp == 2)
			snprintf(buf, buflen, "%u (NeXT alpha, used in 1.0)", p);
		else
			snprintf(buf, buflen, "%u (CMYK color)", p);
		break;
	default: snprintf(buf, buflen, "%u (0x%x)", p, p); break;
	}
}

static const char *
orientation_phrase(uint32_t o)
{
	switch (o) {
	case 1: return "row 0 top, col 0 lhs";
	case 2: return "row 0 top, col 0 rhs";
	case 3: return "row 0 bottom, col 0 rhs";
	case 4: return "row 0 bottom, col 0 lhs";
	case 5: return "row 0 left, col 0 top";
	case 6: return "row 0 right, col 0 top";
	case 7: return "row 0 right, col 0 bottom";
	case 8: return "row 0 left, col 0 bottom";
	default: return "unknown";
	}
}

static const char *
sampleformat_phrase(uint32_t s)
{
	switch (s) {
	case 1: return "unsigned integer";
	case 2: return "signed integer";
	case 3: return "floating point";
	case 4: return "undefined";
	default: return "unknown";
	}
}

/*
 * Pull the description out of an ICC profile, which is what the reference
 * tool reports as "Profile Name".  ICC v2 profiles carry a 'desc' tag with an
 * ASCII string, while v4 profiles use a 'mluc' with UTF-16 text; both appear
 * in the wild so both are handled.
 */
static void
icc_profile_name(tiff_t *t, int dir, char *out, size_t outsz)
{
	unsigned char *icc = NULL;
	uint32_t icclen = 0;
	int be = 1;                   /* an ICC profile is always big-endian */

	out[0] = '\0';
	if (tu_get_bytes(t, dir, TAG_ICCPROFILE, &icc, &icclen) < 0 || icc == NULL)
		return;
	if (icclen < 132) {
		free(icc);
		return;
	}
	uint32_t ntags = rd_be32(icc + 128, be);
	for (uint32_t i = 0; i < ntags; i++) {
		const unsigned char *e = icc + 132 + 12 * i;
		if (e + 12 > icc + icclen)
			break;
		if (rd_be32(e, be) != 0x64657363u)         /* 'desc' */
			continue;
		uint32_t off = rd_be32(e + 4, be);
		if (off + 8 > icclen)
			break;
		uint32_t sig = rd_be32(icc + off, be);
		if (sig == 0x64657363u && off + 12 <= icclen) {
			/* ICC v2: a 4-byte count then the ASCII text. */
			uint32_t n = rd_be32(icc + off + 8, be);
			if (off + 12 + n > icclen)
				n = icclen - off - 12;
			if (n > outsz - 1)
				n = (uint32_t)outsz - 1;
			memcpy(out, icc + off + 12, n);
			out[n] = '\0';
		} else if (sig == 0x6d6c7563u && off + 28 <= icclen) {
			/* ICC v4: take the first localised record's UTF-16BE. */
			uint32_t rlen = rd_be32(icc + off + 8, be);
			uint32_t roff = rd_be32(icc + off + 20, be);
			uint32_t absoff = off + roff;
			if (absoff + 12 > icclen)
				break;
			uint32_t n = rd_be32(icc + absoff + 4, be);
			if (absoff + 8 + n * 2 > icclen)
				n = (icclen - absoff - 8) / 2;
			if (n > (uint32_t)((outsz - 1) / 2))
				n = (uint32_t)((outsz - 1) / 2);
			(void)rlen;
			for (uint32_t k = 0; k < n; k++)
				out[k] = (char)icc[absoff + 8 + 2 * k + 1];
			out[n] = '\0';
		}
		break;
	}
	free(icc);
}

int
tu_cmd_info(const char *path, int verbose)
{
	tiff_t t;
	uint32_t v;

	if (tiff_open_file(&t, path) < 0) {
		tiff_close(&t);
		return -1;
	}
	for (int d = 0; d < t.ndir; d++) {
		uint32_t w = 0, h = 0, bps = 0, spp = 1, rps = 0, photo = 0;
		uint32_t shown_rps = 0;
		int resplit = 0;

		/* Unnamed fields are named on stderr as the directory is read,
		 * in the order they sit in it rather than in tag order, and
		 * before any of the other per-directory complaints. */
		{
			uint32_t comp = 0;

			tu_get_uint(&t, d, TAG_COMPRESSION, &comp);
			for (uint32_t i = 0; i < t.ndirs[d]; i++) {
				uint16_t tag = t.ents[d][i].tag;

				if (!tag_is_known(tag, comp))
					tu_warn("TIFFReadDirectory: Warning,"
					    " Unknown field with tag %u (0x%x)"
					    " encountered.\n", tag, tag);
			}
		}
		{
			uint32_t cspp = 0, ctype = 0;
			int cxtype = 0;
			uint32_t ccount = 0, nchan;
			tu_get_uint(&t, d, TAG_SAMPLESPERPIXEL, &cspp);
			tu_get_uint(&t, d, TAG_PHOTOMETRIC, &ctype);
			if (tu_tag_raw(&t, d, TAG_EXTRASAMPLES, &cxtype,
			    &ccount) == NULL)
				ccount = 0;
			/* The sum is only ever short, never long, and a
			 * photometric with no channel count is not checked.  This
			 * one is a warning rather than part of the report, so it
			 * goes to stderr and not into the listing. */
			nchan = photo_channels(ctype);
			if (nchan != 0 && nchan + ccount < cspp)
				tu_warn("TIFFReadDirectory: Warning, Sum of"
				    " Photometric type-related color channels"
				    " and ExtraSamples doesn't match"
				    " SamplesPerPixel. Defining non-color"
				    " channels as ExtraSamples..\n");
		}
		printf("Directory at 0x%x\n", t.ifdoff[d]);
		tu_get_uint(&t, d, TAG_IMAGEWIDTH, &w);
		tu_get_uint(&t, d, TAG_IMAGELENGTH, &h);
		printf("  Image Width: %u Image Length: %u\n", w, h);
		if (tu_get_uint(&t, d, TAG_XRESOLUTION, &v) == 0) {
			uint32_t y = 0;
			tu_get_uint(&t, d, TAG_YRESOLUTION, &y);
			printf("  Resolution: %u, %u\n", v, y);
		}
		if (tu_has_tag(&t, d, TAG_RESOLUTIONUNIT)) {
			tu_get_uint(&t, d, TAG_RESOLUTIONUNIT, &v);
			printf("  Resolution Unit: %s\n", v == 2 ? "pixels/inch"
			    : v == 3 ? "centimeters/inch" : "unknown");
		}
		if (tu_get_uint(&t, d, TAG_BITSPERSAMPLE, &bps) == 0)
			printf("  Bits/Sample: %u\n", bps);
		if (tu_has_tag(&t, d, TAG_SAMPLEFORMAT)) {
			tu_get_uint(&t, d, TAG_SAMPLEFORMAT, &v);
			printf("  Sample Format: %s\n", sampleformat_phrase(v));
		}
		if (tu_get_uint(&t, d, TAG_COMPRESSION, &v) == 0)
			printf("  Compression Scheme: %s\n", tu_compression_name(v));
		tu_get_uint(&t, d, TAG_SAMPLESPERPIXEL, &spp);
		if (tu_get_uint(&t, d, TAG_PHOTOMETRIC, &photo) == 0) {
			char ph[64];

			/* Separated at two samples is not four inks, it is the
			 * grayscale and alpha pair NeXT shipped in 1.0. */
			photometric_phrase(photo, spp, ph, sizeof(ph));
			printf("  Photometric Interpretation: %s\n", ph);
		}
		if (tu_has_tag(&t, d, TAG_EXTRASAMPLES))
			printf("  Alpha: Present\n");
		if (tu_has_tag(&t, d, TAG_FILLORDER)) {
			tu_get_uint(&t, d, TAG_FILLORDER, &v);
			printf("  FillOrder: %s\n", v == 2 ? "lsb-to-msb" : "msb-to-lsb");
		}
		if (tu_get_uint(&t, d, TAG_PREDICTOR, &v) == 0 && v != 1)
			printf("  Predictor: %s\n",
			    v == 2 ? "horizontal differencing" : "floating point");
		if (tu_has_tag(&t, d, TAG_ORIENTATION)) {
			tu_get_uint(&t, d, TAG_ORIENTATION, &v);
			printf("  Orientation: %s\n", orientation_phrase(v));
		}
		printf("  Samples/Pixel: %u\n", spp);		tu_get_uint(&t, d, TAG_ROWSPERSTRIP, &rps);
		{
			/* A file stored as one strip is reported as the strips it
			 * would be split into, at roughly 8K per strip.  A file
			 * that already has several strips is reported as stored,
			 * and a written file keeps the stored value. */
			uint32_t shown = rps;

			if (shown == 0 || shown >= h) {
				uint32_t rb = w * ((bps + 7) / 8) * spp;

				shown = rb == 0 ? 1 : 8192 / rb;
				if (shown == 0)
					shown = 1;
				if (shown > h)
					shown = h;
			}
			/* A genuine split only happens when the recomputed
			 * value came out below the image height. */
			resplit = (rps == 0 || rps >= h) && shown < h;
			shown_rps = shown;
			printf("  Rows/Strip: %u\n", shown);
			if (shown)
				printf("  Number of Strips: %u\n",
				    (h + shown - 1) / shown);
		}
		if (verbose) {
			uint32_t rowbytes = w * ((bps + 7) / 8) * spp;
			uint32_t n = t.nstrips[d];

			if (resplit && shown_rps > 0 && rowbytes > 0) {
				/* Stored as one strip, so list the strips the
				 * reported layout implies. */
				uint32_t k = (h + shown_rps - 1) / shown_rps;

				printf("  Strips (Offset, ByteCount):\n");
				for (uint32_t i = 0; i < k; i++) {
					uint32_t rows = h - i * shown_rps;

					if (rows > shown_rps)
						rows = shown_rps;
					printf("     %u, %u\n",
					    8 + i * shown_rps * rowbytes,
					    rows * rowbytes);
				}
			} else if (n > 0) {
				printf("  Strips (Offset, ByteCount):\n");
				for (uint32_t i = 0; i < n; i++)
					printf("     %u, %u\n", t.strips[d][i],
					    t.stripbc[d][i]);
			}
		}
		if (tu_get_uint(&t, d, TAG_PLANARCONFIG, &v) == 0)
			printf("  Planar Configuration: %s\n",
			    v == 2 ? "planar" : "Not planar");
		if (tu_has_tag(&t, d, TAG_COLORMAP)) {
			unsigned char *cm = NULL;
			uint32_t cn = 0;

			if (tu_get_bytes(&t, d, TAG_COLORMAP, &cm, &cn) == 0 &&
			    cm != NULL) {
				if (verbose) {
					printf("  Color Map: \n");
					for (uint32_t i = 0; i + 2 < cn; i += 3)
						printf("    %u: %u %u %u\n", i / 3,
						    rd_be16(cm + 2 * i, t.be),
						    rd_be16(cm + 2 * i + 2, t.be),
						    rd_be16(cm + 2 * i + 4, t.be));
				} else {
					printf("  Color Map: (present)\n");
				}
				free(cm);
			}
		}
		if (tu_has_tag(&t, d, TAG_ICCPROFILE)) {
			char name[256];
			icc_profile_name(&t, d, name, sizeof(name));
			if (name[0] != '\0')
				printf("  Profile Name: %s\n", name);
		}
	}
	tiff_close(&t);
	return 0;
}

int
tu_cmd_dump(const char *path)
{
	tiff_t t;
	uint32_t off;

	if (tiff_open_file(&t, path) < 0) {
		int rc, openerc = t.openerc;

		tiff_close(&t);
		/* A file that could not be read is reported by name and still
		 * exits 0. A file that was read but is not a TIFF is reported
		 * against a null name -- the reference tool formats a path it
		 * never got -- and does exit non-zero. */
		if (openerc == TUFF_ENOENT) {
			fprintf(stderr, "%s: %s\n", path, strerror(ENOENT));
			return 0;
		}
		fprintf(stderr, "(null): Error while reading TIFF header.\n");
		rc = 1;
		return rc;
	}
	printf("Magic: 0x%02x%02x <%s-endian> Version: 0x%x <ClassicTIFF>\n",
	    t.data[0], t.data[1], t.be ? "big" : "little",
	    (unsigned)rd_be16(t.data + 2, t.be));

	off = rd_be32(t.data + 4, t.be);
	for (int d = 0; d < t.ndir && off; d++) {
		uint16_t n = rd_be16(t.data + off, t.be);
		uint32_t next = rd_be32(t.data + off + 2 + (size_t)n * 12, t.be);

		/* Directories are separated by a blank line; the last one has
		 * no trailing blank. */
		if (d > 0)
			printf("\n");
		printf("Directory %d: offset %u (%#x) next %u (%#x)\n", d,
		    off, off, next, next);
		for (uint16_t i = 0; i < n; i++) {
			const unsigned char *e = t.data + off + 2 + (size_t)i * 12;
			uint16_t tag = rd_be16(e, t.be);
			uint16_t type = rd_be16(e + 2, t.be);
			uint32_t cnt = rd_be32(e + 4, t.be);
			size_t sz = tu_type_size(type);
			const char *nm = dump_tag_name(tag);
			int rtype;
			uint32_t rcount;
			unsigned char *val = tu_tag_raw(&t, d, tag, &rtype, &rcount);
			size_t esz = tu_type_size(rtype);
			uint32_t shown = 0, k;

			printf("%s (%u) %s (%u) %u<", nm ? nm : "Unknown",
			    tag, tu_type_name(type), type, cnt);
			if (val != NULL && esz > 0) {
				/* A RATIONAL reports its numerator, matching
				 * the way the reference tool shows values. */
				shown = esz == 1 ? val[0]
				    : esz == 2 ? rd_be16(val, t.be)
				    : rd_be32(val, t.be);
			} else {
				/* Nothing resolvable: read the inline slot. */
				unsigned char raw[4];
				memcpy(raw, e + 8, 4);
				shown = sz == 1 ? raw[0] : sz == 2
				    ? rd_be16(raw, t.be) : rd_be32(raw, t.be);
			}
			if (type == TYPE_UNDEFINED) {
				/* Undefined bytes are shown in hex. The reference
				 * tool prints a zero as "00" but every other
				 * value with an 0x prefix and no padding, so 2
				 * comes out "0x2" and 12 comes out "0xc".
				 * Both paths preview 24 values. */
				for (k = 0; k < cnt && k < 24; k++) {
					unsigned int x = val ? val[k] : 0;
					if (x == 0)
						printf(k ? " %02x" : "%02x", x);
					else
						printf(k ? " 0x%x" : "0x%x", x);
				}
				if (cnt > 20)
					printf(" ...");
			} else if (cnt == 1) {
				printf("%u", shown);
			} else {
				for (k = 0; k < cnt && k < 24; k++) {
					const unsigned char *p = val ? val + k * esz : NULL;
					uint32_t x = p == NULL ? 0
					    : esz == 1 ? p[0]
					    : esz == 2 ? rd_be16(p, t.be)
					    : rd_be32(p, t.be);
					printf("%s%u", k ? " " : "", x);
				}
				if (cnt > 24)
					printf(" ...");
			}
			printf(">");
			free(val);
			printf("\n");
		}
		off = next;
	}
	tiff_close(&t);
	return 0;
}
