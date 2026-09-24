/*
 * pbcopy/pbpaste - copy and paste between the NSPasteboard and stdio.
 *
 * Single-source, argv[0]-dispatched clean-room implementation of Apple's
 * /usr/bin/pbcopy and /usr/bin/pbpaste (Apple ships the same object code
 * under both names).
 *
 * Behavior recovered from /usr/bin/pbpaste (arm64e) via disassembly plus
 * black-box probing:
 *
 *   - encoding is derived from setlocale(LC_ALL, "") / nl_langinfo(CODESET),
 *     with a kCFStringEncodingASCII / invalid-id fallback to
 *     [NSString defaultCStringEncoding];
 *   - "Prefer" (then "prefer") and "pboard" are read from NSUserDefaults,
 *     i.e. from "-key value" arguments on the command line;
 *   - pbcopy sniffs the input for PostScript / RTF headers and declares the
 *     matching type with a nil owner, then sets the string for it;
 *   - pbpaste asks availableTypeFromArray: for
 *     [prefer?, NSStringPboardType, NSPostScriptPboardType,
 *      NSRTFPboardType] and writes the matching string re-encoded with
 *     lossy conversion on;
 *   - no matching type: clean exit 0 with no output; pasteboardWithName:
 *     failure exits 1.
 *
 * Copyright (C) 2026, LibreDarwin
 * SPDX-License-Identifier: BSD-3-Clause
 */

#import <AppKit/AppKit.h>
#import <Foundation/Foundation.h>

#include <locale.h>
#include <langinfo.h>
#include <stdlib.h>

static NSStringEncoding
encodingFromLocale(void)
{
	CFStringEncoding cfEnc;
	NSStringEncoding nsEnc;
	CFStringRef codeset;

	setlocale(LC_ALL, "");
	codeset = CFStringCreateWithCString(NULL, nl_langinfo(CODESET),
	    kCFStringEncodingUTF8);
	cfEnc = CFStringConvertIANACharSetNameToEncoding(codeset);
	if (codeset != NULL)
		CFRelease(codeset);

	if (cfEnc == kCFStringEncodingASCII || cfEnc == (CFStringEncoding)-1)
		nsEnc = [NSString defaultCStringEncoding];
	else
		nsEnc = CFStringConvertEncodingToNSStringEncoding(cfEnc);

	return nsEnc;
}

int
main(int argc __unused, char **argv)
{
	NSAutoreleasePool *pool = [[NSAutoreleasePool alloc] init];
	NSStringEncoding nsEnc = encodingFromLocale();
	NSString *programName = [[NSString stringWithCString:argv[0]
	    encoding:nsEnc] lastPathComponent];
	NSUserDefaults *defaults = [NSUserDefaults standardUserDefaults];
	NSArray *arguments = [[NSProcessInfo processInfo] arguments];
	BOOL isPbcopy = [programName isEqualToString:@"pbcopy"];

	if ([arguments containsObject:@"-help"] ||
	    [arguments containsObject:@"-h"] ||
	    [arguments containsObject:@"-H"]) {
		if (isPbcopy)
			NSLog(@"Usage: %@ [%@]", programName, @"-help");
		else
			NSLog(@"Usage: %@ [%@] [-%@ %@|%@|%@]",
			    programName, @"-help", @"Prefer", @"rtf", @"ps", @"txt");
		_exit(0);
	}

	NSString *pboardName = [defaults objectForKey:@"pboard"];
	NSPasteboardName pboard;
	if ([pboardName isEqualToString:@"ruler"])
		pboard = NSRulerPboard;
	else if ([pboardName isEqualToString:@"find"])
		pboard = NSFindPboard;
	else if ([pboardName isEqualToString:@"font"])
		pboard = NSFontPboard;
	else
		pboard = NSGeneralPboard;

	NSPasteboard *pb = [NSPasteboard pasteboardWithName:pboard];
	if (pb == nil)
		_exit(1);

	if (isPbcopy) {
		NSData *data = [[NSFileHandle fileHandleWithStandardInput]
		    readDataToEndOfFile];
		NSString *str = [[NSString alloc] initWithData:data
		    encoding:nsEnc];
		if (str == nil)
			str = [[NSString alloc] initWithData:data
			    encoding:[NSString defaultCStringEncoding]];

		NSPasteboardType type;
		if ([str hasPrefix:@"%!PS-Adobe-2.0 EPSF-"])
			type = NSPostScriptPboardType;
		else if ([str hasPrefix:@"{\\rtf"])
			type = NSRTFPboardType;
		else
			type = NSStringPboardType;

		[pb declareTypes:[NSArray arrayWithObject:type] owner:nil];
		[pb setString:str forType:type];
		[str release];
	} else {
		NSString *prefer = [defaults objectForKey:@"Prefer"];
		if (prefer == nil || [prefer length] == 0)
			prefer = [defaults objectForKey:@"prefer"];

		NSArray *types;
		if (prefer != nil && [prefer length] != 0)
			types = [NSArray arrayWithObjects:prefer,
			    NSStringPboardType, NSPostScriptPboardType,
			    NSRTFPboardType, nil];
		else
			types = [NSArray arrayWithObjects:NSStringPboardType,
			    NSPostScriptPboardType, NSRTFPboardType, nil];

		NSPasteboardType type = [pb availableTypeFromArray:types];
		if (type != nil) {
			NSString *str = [pb stringForType:type];
			NSData *out = [str dataUsingEncoding:nsEnc
			    allowLossyConversion:YES];
			[[NSFileHandle fileHandleWithStandardOutput]
			    writeData:out];
		}
	}

	[pool drain];
	_exit(0);
}