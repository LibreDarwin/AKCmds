/*
 * tiffutil - shared declarations.
 *
 * Copyright (C) 2026, LibreDarwin
 * SPDX-License-Identifier: BSD-3-Clause
 */

#ifndef TIFFUTIL_H
#define TIFFUTIL_H

#include <stdarg.h>
#include <stdint.h>
#include <stddef.h>
#include <time.h>

#include "buf.h"

typedef struct {
	uint16_t tag;
	uint16_t type;
	uint32_t count;
	unsigned char inl[4];
} tiff_ent_t;

typedef struct {
	int openerc;                  /* TUFF_* from the last open attempt */
	int be;                       /* big-endian when set */
	unsigned char *data;
	size_t len;
	tiff_ent_t **ents;            /* per directory */
	uint32_t *ndirs;              /* entry count per directory */
	uint32_t *ifdoff;             /* file offset of each directory */
	int ndir;
	uint32_t *nstrips;            /* strip count per directory */
	uint32_t **strips;            /* strip offsets per directory */
	uint32_t **stripbc;           /* strip byte counts per directory */
} tiff_t;

/* Endian-aware reads against a raw buffer. */
uint16_t rd_be16(const unsigned char *p, int be);
uint32_t rd_be32(const unsigned char *p, int be);

int tiff_cat(const char *const *paths, int npaths, const char *outpath);
int tiff_open_mem(tiff_t *t, const unsigned char *data, size_t len);
extern int tu_chatter;
void tu_set_chatter(int on);

#define TYPE_UNDEFINED 7

#define TUFF_ENOENT (-2)
#define TUFF_EOPEN  (-1)

int tiff_open_file(tiff_t *t, const char *path);
void tiff_close(tiff_t *t);
void tiff_sort_entries(tiff_t *t);

int tu_get_uint(tiff_t *t, int dir, uint16_t tag, uint32_t *out);
int tu_get_rational(tiff_t *t, int dir, uint16_t tag, uint32_t *num,
    uint32_t *den);
int tu_get_bytes(tiff_t *t, int dir, uint16_t tag, unsigned char **out, uint32_t *n);
int tu_has_tag(tiff_t *t, int dir, uint16_t tag);
unsigned char *tu_tag_raw(tiff_t *t, int dir, uint16_t tag, int *type, uint32_t *count);
const char *tu_type_name(int type);
const char *tu_compression_name(uint32_t c);
size_t tu_type_size(int type);

void tu_warn(const char *fmt, ...);

/* Decompression.  Both return a malloc'd buffer the caller frees. */
int lzw_decode(const unsigned char *in, size_t inlen, unsigned char **out,
    size_t *outlen);
int lzw_encode(const unsigned char *in, size_t inlen, unsigned char **out,
    size_t *outlen);
int packbits_encode(const unsigned char *in, size_t inlen, size_t rowbytes,
    unsigned char **out,
    size_t *outlen);
int packbits_decode(const unsigned char *in, size_t inlen, unsigned char **out,
    size_t *outlen);

/* Group 4.  Takes one byte per pixel (00 or ff), not packed bits, and writes
 * the encoded strip including its two end-of-block codes.  invert is for a
 * source photometric of 0, whose samples mean the opposite. */
int g4_encode(const unsigned char *px, uint32_t width, uint32_t height,
    int invert, unsigned char **out, size_t *outlen);

/* The other direction, for a strip of `rows` lines.  The result is packed one
 * bit per sample, most significant bit first, each row padded to a byte, which
 * is the shape load_image reads; outlen is the row stride times the row count. */
int g4_decode(const unsigned char *in, size_t inlen, uint32_t width,
    uint32_t rows, int invert, unsigned char **out, size_t *outlen);

/* An image reduced to what a rewrite needs. */
typedef struct {
	uint32_t width, height;
	uint32_t bps, spp;
	uint32_t photometric;
	uint32_t outphoto;            /* the photometric the rewrite carries */
	uint32_t outbps;              /* depth the rewrite carries when it is
	                               * not the widened one in bps, else 0 */
	uint32_t rows_per_strip;
	uint32_t xres, yres;          /* reduced to whole units per inch */
	unsigned char *px;            /* decoded, native endian, predictor undone */
	size_t pxlen;
	unsigned char *cmap;          /* palette, big-endian, or NULL */
	uint32_t cmapcount;
	uint16_t xalpha;              /* ExtraSamples value for the one alpha
	                               * channel the output keeps */
	uint32_t srcspp;             /* samples the file itself carried, before
	                               * surplus channels were dropped */
	uint32_t srcbps;             /* depth the file itself carried, before
	                               * narrow samples were widened to a byte */
	int unusable;                /* the reference tool cannot turn this into
	                               * an image at all, and writes nothing */
	int unopenable;              /* the photometric means nothing to it, so
	                               * the file is refused outright rather than
	                               * written empty */
} tuimg_t;

int tiff_build_strip(const unsigned char *src, size_t srclen, uint32_t bps,
    uint32_t spp, uint32_t width, int predictor, int swap,
    unsigned char **out, size_t *outlen);

/* One directory's worth of output: the decoded image plus the strip already
 * encoded for it, which is what lets a multi-directory rewrite keep each
 * directory's own compression. */
typedef struct {
	tuimg_t im;
	const unsigned char *strip;
	size_t striplen;
	int compression;
	int predictor;
	const char *desc;
	const char *software;
} tuwrite_t;

int tiff_write_images(const char *path, const tuwrite_t *items, int n);
int tiff_write_image(const char *path, const tuimg_t *im,
    const unsigned char *strip, size_t striplen, int compression,
    int predictor);
int tiff_predictor_for(int compression, uint32_t bps);
int tiff_color_channels(uint32_t photometric);
uint32_t tiff_out_photometric(uint32_t photo, uint32_t spp, uint32_t bps);
const unsigned char *tiff_icc_for(int photometric, unsigned char *scratch,
    size_t scratchlen, const unsigned char **p, size_t *len);

/* Text modes. */
int tu_cmd_info(const char *path, int verbose);
int tu_cmd_dump(const char *path);

/* Tags used by the tool. */
#define TAG_NEWSUBFILETYPE   254
#define TAG_IMAGEWIDTH      256
#define TAG_IMAGELENGTH     257
#define TAG_BITSPERSAMPLE   258
#define TAG_COMPRESSION     259
#define TAG_PHOTOMETRIC     262
#define TAG_FILLORDER       266
#define TAG_IMAGEDESCRIPTION 270
#define TAG_STRIPOFFSETS    273
#define TAG_ORIENTATION     274
#define TAG_SAMPLESPERPIXEL 277
#define TAG_ROWSPERSTRIP    278
#define TAG_STRIPBYTECOUNTS 279
#define TAG_XRESOLUTION     282
#define TAG_YRESOLUTION     283
#define TAG_PLANARCONFIG    284
#define TAG_COLORMAP        320
#define TAG_PREDICTOR       317
#define TAG_YCBCRCOEFFICIENT 529
#define TAG_YCBCRSUBSAMPLING 530
#define TAG_REFERENCEBLACKWHITE 532
#define TAG_DATATYPE        32996
#define TAG_TILEDEPTH       32998
#define TAG_RESOLUTIONUNIT  296
#define TAG_SOFTWARE        305
#define TAG_SAMPLEFORMAT    339
#define TAG_EXTRASAMPLES    338
#define TAG_ICCPROFILE      34675

/* Compression schemes tiffutil writes. */
#define COMP_NONE      1
#define COMP_G4        4
#define COMP_LZW       5
#define COMP_PACKBITS  32773

#endif
