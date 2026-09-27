/*
 * -cat
 *
 * Concatenating images is not a re-encode.  Every input file is copied
 * byte for byte, the eight byte header of each file after the first is
 * dropped, and the only edits are to the offsets that point at data
 * inside the file.  Compression, predictor, bit depth and the rest are
 * inherited from the source untouched, so a concatenated file holds
 * whatever each input happened to contain.
 *
 * Everything after the dropped header moves by one fixed amount per
 * input, so a single running delta fixes a whole file at once.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "tiffutil.h"

static void
put32(unsigned char *p, uint32_t v, int be)
{
	if (be) {
		p[0] = (unsigned char)(v >> 24);
		p[1] = (unsigned char)(v >> 16);
		p[2] = (unsigned char)(v >> 8);
		p[3] = (unsigned char)v;
	} else {
		p[0] = (unsigned char)v;
		p[1] = (unsigned char)(v >> 8);
		p[2] = (unsigned char)(v >> 16);
		p[3] = (unsigned char)(v >> 24);
	}
}

/* Tags whose value is a file offset rather than a datum.  Guessing
 * wrongly in the permissive direction is what corrupts a file, so
 * anything not known to be an offset is left alone.  StripByteCounts is
 * deliberately absent: it is a length sitting beside an offset and must
 * not move. */
static int
is_offset_tag(uint16_t tag)
{
	switch (tag) {
	case 273:			/* StripOffsets */
	case 282:			/* XResolution */
	case 283:			/* YResolution */
	case 320:			/* ColorMap */
	case 324:			/* TileOffsets */
	case 330:			/* SubIFDs */
	case 34665:			/* ExifIFD */
	case 34675:			/* InterColorProfile */
	case 34735:			/* GeoKeyDirectory */
	case 34736:			/* GeoDoubleParams */
	case 34737:			/* GeoAsciiParams */
	case 42112:			/* GDAL_METADATA */
	case 42113:			/* GDAL_NODATA */
		return 1;
	default:
		return 0;
	}
}

/* Size in bytes of one value of the given field type. */
static uint32_t
type_size(uint16_t type)
{
	switch (type) {
	case 1: case 2: case 6: case 7:		return 1;
	case 3: case 8:				return 2;
	case 4: case 9: case 11: case 13:	return 4;
	case 5: case 10: case 12: case 16:
	case 17:				return 8;
	default:				return 0;
	}
}

/* Shift every offset in the file by delta, in place. */
static void
cat_shift(unsigned char *d, int be, uint32_t delta)
{
	uint32_t off = rd_be32(d + 4, be);
	uint32_t guard = 0;

	while (off != 0 && guard++ < 4096) {
		uint32_t n, k;
		if (off + 2 > 0 && off < 0xfffffff0u) {
			n = rd_be16(d + off, be);
			for (k = 0; k < n; k++) {
				unsigned char *e = d + off + 2 + 12 * k;
				uint16_t tag = rd_be16(e, be);
				uint16_t type = rd_be16(e + 2, be);
				uint32_t cnt = rd_be32(e + 4, be);
				uint32_t sz = type_size(type);
				if (!is_offset_tag(tag) || sz == 0)
					continue;
				/* A value that does not fit in the four byte
				 * slot is stored out of line and the slot holds
				 * the offset to it.  Testing the size rather
				 * than the count matters: an ICC profile has
				 * count 4508 but still keeps a single offset
				 * in the slot. */
				if (sz * cnt <= 4)
					continue;
				put32(e + 8, rd_be32(e + 8, be) + delta, be);
			}
			off = rd_be32(d + off + 2 + 12 * n, be);
		} else {
			break;
		}
	}
}

int
tiff_cat(const char *const *paths, int npaths, const char *outpath)
{
	FILE *out;
	long written = 0;
	long prev_next = -1;
	int total = 0, i;

	out = fopen(outpath, "wb");
	if (out == NULL)
		return 1;
	for (i = 0; i < npaths; i++) {
		tiff_t t;
		uint32_t first, last, drop = i == 0 ? 0 : 8;

		if (tiff_open_file(&t, paths[i]) < 0) {
			fclose(out);
			return 1;
		}
		if (t.ndir < 1 || t.len <= drop) {
			tiff_close(&t);
			fclose(out);
			return 1;
		}
		cat_shift(t.data, t.be, (uint32_t)(written - (long)drop));
		first = t.ifdoff[0] - drop;
		last = t.ifdoff[t.ndir - 1] - drop;

		if (prev_next >= 0) {
			unsigned char p[4];
			put32(p, (uint32_t)(written + (long)first), t.be);
			if (fseek(out, prev_next, SEEK_SET) != 0 ||
			    fwrite(p, 1, 4, out) != 4) {
				tiff_close(&t);
				fclose(out);
				return 1;
			}
		}
		if (fseek(out, written, SEEK_SET) != 0 ||
		    fwrite(t.data + drop, 1, t.len - drop, out) !=
		    t.len - drop) {
			tiff_close(&t);
			fclose(out);
			return 1;
		}
		prev_next = written + (long)last + 2 + 12 * (long)t.ndirs[t.ndir - 1];
		written += (long)t.len - (long)drop;
		total += t.ndir;
		tiff_close(&t);
	}
	if (fclose(out) != 0)
		return 1;
	printf("%d image%s written to %s.\n", total, total == 1 ? "" : "s",
	    outpath);
	return 0;
}
