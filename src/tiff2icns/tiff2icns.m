/*
 * tiff2icns - convert an image file into a .icns icon family file.
 *
 * Clean-room implementation of Apple's /usr/bin/tiff2icns.  Behavior was
 * recovered from the arm64e binary by disassembly plus black-box probing:
 *
 *   - usage: tiff2icns [-noLarge] infile [outfile]; the source is opened
 *     lazily by path (NSImage initByReferencingFile:) and must exist;
 *   - the source's bitmap representations are scanned for exact matches
 *     at 48, 32, 16, 128, 256, 512 and 1024 pixels; among equal-size
 *     matches the representation with the most bits per sample wins;
 *   - by default, when no 32x32 representation exists, a 64x64
 *     representation is synthesized from a copy of the image, so the
 *     result always contains a small icon; -noLarge disables that;
 *   - the chosen representations are packed into an icon family with
 *     CGImageDestinationCreateWithData + kUTTypeAppleICNS and written
 *     back with writeToFile:atomically:YES; when no representation
 *     matched, an empty .icns file is still written;
 *   - without an outfile argument the output path is the source path with
 *     the extension replaced by "icns".
 *
 * Copyright (C) 2026, LibreDarwin
 * SPDX-License-Identifier: BSD-3-Clause
 */

#import <AppKit/AppKit.h>
#import <Foundation/Foundation.h>

#include <CoreServices/CoreServices.h>
#include <ImageIO/ImageIO.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

__attribute__((used))
static const char kNTVersion[] = "@(#)PROGRAM:tiff2icns  PROJECT:AKCmds-350";

static void usage(const char *prog)
{
    fprintf(stderr, "Usage: %s [%s] infile [outfile]\n", prog, "-noLarge");
    exit(1);
}

/*
 * Pick the source representation that fits the icon slot of the given
 * size.  Candidates must be bitmap representations whose pixel size is
 * exactly size x size; when several fit, the one with the most bits
 * per sample is used.
 */
static NSBitmapImageRep *repForSize(NSImage *image, NSInteger size)
{
    NSBitmapImageRep *best = nil;
    NSArray *reps = [image representations];
    NSUInteger count = [reps count];

    if (count < 1)
        return nil;

    for (NSUInteger i = 0; i < count; i++) {
        NSBitmapImageRep *rep = [reps objectAtIndex:i];
        NSInteger wide, high, bps;

        if (![rep isKindOfClass:[NSBitmapImageRep class]])
            continue;
        wide = [rep pixelsWide];
        if (wide != size)
            continue;
        high = [rep pixelsHigh];
        if (high != size)
            continue;
        bps = [rep bitsPerSample];
        if (best != nil && [best bitsPerSample] >= bps)
            continue;
        best = rep;
    }

    return best;
}

static int convert(const char *prog, const char *src, const char *out,
    int addSmallIcon)
{
    NSFileManager *fm = [NSFileManager defaultManager];
    NSString *srcPath, *outPath;
    NSImage *image;
    NSBitmapImageRep *rep, *rep32;
    NSMutableArray *icons;
    NSData *data;
    int rc = 0;

    srcPath = [fm stringWithFileSystemRepresentation:src length:strlen(src)];
    fm = [NSFileManager defaultManager];

    if (![fm fileExistsAtPath:srcPath]) {
        fprintf(stderr, "%s: source file '%s' does not exist.\n",
            prog, src);
        rc = 1;
        goto out;
    }

    image = [[[NSImage alloc] initByReferencingFile:srcPath] autorelease];
    if (image == nil || [[image representations] count] < 1) {
        fprintf(stderr, "%s: cannot load source image '%s'\n",
            prog, src);
        rc = 1;
        goto out;
    }

    icons = [[NSMutableArray alloc] init];
    rep = repForSize(image, 48);
    if (rep != nil)
        [icons addObject:rep];
    rep32 = repForSize(image, 32);
    if (rep32 != nil)
        [icons addObject:rep32];
    rep = repForSize(image, 16);
    if (rep != nil)
        [icons addObject:rep];
    rep = repForSize(image, 128);
    if (rep != nil)
        [icons addObject:rep];
    rep = repForSize(image, 256);
    if (rep != nil)
        [icons addObject:rep];
    rep = repForSize(image, 512);
    if (rep != nil)
        [icons addObject:rep];
    rep = repForSize(image, 1024);
    if (rep != nil)
        [icons addObject:rep];

    if (addSmallIcon && rep32 == nil) {
        NSImage *scaled = [image copy];
        [scaled setSize:NSMakeSize(64.0, 64.0)];
        [scaled lockFocus];
        rep = [[NSBitmapImageRep alloc]
            initWithFocusedViewRect:NSMakeRect(0.0, 0.0, 64.0, 64.0)];
        [scaled unlockFocus];
        [scaled release];
        [icons addObject:rep];
        [rep release];
    }

    if (out != nil) {
        outPath = [fm stringWithFileSystemRepresentation:out length:strlen(out)];
    } else {
        outPath = [[srcPath stringByDeletingPathExtension]
            stringByAppendingPathExtension:@"icns"];
    }

    if ([icons count] > 0) {
        CGImageDestinationRef dest;
        NSMutableData *mdata = [[NSMutableData alloc] init];
        dest = CGImageDestinationCreateWithData((CFMutableDataRef)mdata,
            kUTTypeAppleICNS, [icons count], NULL);
        for (NSBitmapImageRep *icon in icons) {
            CGImageDestinationAddImage(dest, [icon CGImage], NULL);
        }
        CGImageDestinationFinalize(dest);
        CFRelease(dest);
        data = mdata;
    } else {
        data = [[NSData alloc] init];
        fprintf(stderr,
            "%s: no appropriate images found. writing empty file '%s'\n",
            prog, [outPath fileSystemRepresentation]);
    }

    if ([data writeToFile:outPath atomically:YES]) {
        [data release];
        [icons release];
        rc = 0;
        goto out;
    }

    fprintf(stderr, "%s: cannot load source image %s\n",
        prog, [outPath fileSystemRepresentation]);
    rc = 1;
    goto out;

out:
    return rc;
}

int main(int argc, char **argv)
{
    NSAutoreleasePool *pool;
    const char *src, *out;
    int addSmallIcon, rc;

    pool = [[NSAutoreleasePool alloc] init];

    if (argc < 2)
        usage(argv[0]);

    addSmallIcon = (strcmp(argv[1], "-noLarge") != 0);
    src = (addSmallIcon ? argv[1] : argv[2]);
    if (src == NULL)
        usage(argv[0]);
    out = (addSmallIcon ? argv[2] : argv[3]);

    rc = convert(argv[0], src, out, addSmallIcon);

    [pool release];
    exit(rc);
}