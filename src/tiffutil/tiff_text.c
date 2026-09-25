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
	case 320:                 return "ColorMap";
	case 33628:               return "CFAPattern";
	default:                  return NULL;
	}
}

/* Photometric is quoted for the two grayscale cases and left bare for the
 * colour ones, which is a quirk of the reference tool worth keeping. */
static const char *
photometric_phrase(uint32_t p)
{
	switch (p) {
	case 0: return "\"min-is-white\"";
	case 1: return "\"min-is-black\"";
	case 2: return "RGB color";
	case 3: return "palette color";
	case 4: return "Transparency Mask";
	case 5: return "CMYK color";
	case 6: return "YCbCr color";
	case 8: return "CIELab color";
	case 9: return "ICCLab color";
	case 10: return "ITULab color";
	default: return tu_photometric_name(p);
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
			if (n > (outsz - 1) / 2)
				n = (outsz - 1) / 2;
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
		if (tu_get_uint(&t, d, TAG_PHOTOMETRIC, &photo) == 0)
			printf("  Photometric Interpretation: %s\n",
			    photometric_phrase(photo));
		if (tu_has_tag(&t, d, TAG_FILLORDER)) {
			tu_get_uint(&t, d, TAG_FILLORDER, &v);
			printf("  FillOrder: %s\n", v == 2 ? "lsb-to-msb" : "msb-to-lsb");
		}
		if (tu_has_tag(&t, d, TAG_ORIENTATION)) {
			tu_get_uint(&t, d, TAG_ORIENTATION, &v);
			printf("  Orientation: %s\n", orientation_phrase(v));
		}
		tu_get_uint(&t, d, TAG_SAMPLESPERPIXEL, &spp);
		printf("  Samples/Pixel: %u\n", spp);
		tu_get_uint(&t, d, TAG_ROWSPERSTRIP, &rps);
		printf("  Rows/Strip: %u\n", rps);
		if (rps)
			printf("  Number of Strips: %u\n", (h + rps - 1) / rps);
		if (verbose) {
			uint32_t n = t.nstrips;
			if (n > 0) {
				printf("  Strips (Offset, ByteCount):\n");
				for (uint32_t i = 0; i < n; i++)
					printf("     %u, %u\n", t.strips[i],
					    t.stripbc[i]);
			}
		}
		if (tu_get_uint(&t, d, TAG_PLANARCONFIG, &v) == 0)
			printf("  Planar Configuration: %s\n",
			    v == 2 ? "planar" : "Not planar");
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
		tiff_close(&t);
		return -1;
	}
	printf("Magic: 0x%02x%02x <%s-endian> Version: 0x%x <ClassicTIFF>\n",
	    t.data[0], t.data[1], t.be ? "big" : "little",
	    (unsigned)rd_be16(t.data + 2, t.be));

	off = rd_be32(t.data + 4, t.be);
	for (int d = 0; d < t.ndir && off; d++) {
		uint16_t n = rd_be16(t.data + off, t.be);
		uint32_t next = rd_be32(t.data + off + 2 + (size_t)n * 12, t.be);

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
			if (cnt == 1) {
				printf("%u", shown);
			} else {
				for (k = 0; k < cnt && k < 20; k++) {
					const unsigned char *p = val ? val + k * esz : NULL;
					uint32_t x = p == NULL ? 0
					    : esz == 1 ? p[0]
					    : esz == 2 ? rd_be16(p, t.be)
					    : rd_be32(p, t.be);
					printf("%s%u", k ? " " : "", x);
				}
				if (cnt > 20)
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
