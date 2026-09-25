/*
 * open - open files, directories, applications, and URLs.
 *
 * Clean-room implementation of Apple's /usr/bin/open. Behavior recovered
 * from the arm64e binary by disassembly plus black-box probing:
 *
 *   - getopt "etfFb:a:s:WRnghHvji:o:E:u:", long options --arch, --new,
 *     --background, --hide, --reveal, --wait-apps, --no-recents, --header,
 *     --fresh, --stdin, --stdout, --stderr, --env, --url;
 *   - "--args" divides option arguments from the argv delivered to the
 *     opened application;
 *   - -f writes stdin to a /tmp/open_XXXXXX.txt template, then opens it
 *     with the default text editor (TextEdit when none is known);
 *   - -h/-H search SDK and framework locations for headers matching the
 *     given file names, prompting interactively when more than one header
 *     matches;
 *   - URLs and existing files are opened through LaunchServices
 *     (__LSOpenURLsWithCompletionHandler); an argument that is both an
 *     existing file and a URL asks "Which did you mean?";
 *   - -R reveals files in the Finder; -W waits for launched applications
 *     to exit.
 *
 * Copyright (C) 2026, LibreDarwin
 * SPDX-License-Identifier: BSD-3-Clause
 */

#import <AppKit/AppKit.h>
#import <Foundation/Foundation.h>

#include <ApplicationServices/ApplicationServices.h>
#include <CoreServices/CoreServices.h>
#include <getopt.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <unistd.h>
#include <sys/event.h>



typedef struct __LSASN *LSASN;

extern void _LSASNExtractHighAndLowParts(LSASN inASN, unsigned int *outHigh,
    unsigned int *outLow);

extern int _LSOpenURLsWithCompletionHandler(CFArrayRef inURLs,
    CFURLRef inApplication, CFDictionaryRef inOptions,
    void (^completionHandler)(LSASN theApp, Boolean didOpen,
    CFErrorRef error));

extern CFStringRef _kLSOpenOptionSynchronousKey;

static const char o_usage[] =
    "Usage: %s [-e] [-t] [-f] [-W] [-R] [-n] [-g] [-h] [-s <partial SDK name>][-b <bundle identifier>] [-a <application>] [-u URL] [filenames] [--args arguments]\n"
    "Help: Open opens files from a shell.\n"
    "      By default, opens each file using the default application for that file.  \n"
    "      If the file is in the form of a URL, the file will be opened as a URL.\n"
    "Options: \n"
    "      -a                    Opens with the specified application.\n"
    "      --arch ARCH           Open with the given cpu architecture type and subtype.\n"
    "      -b                    Opens with the specified application bundle identifier.\n"
    "      -e                    Opens with TextEdit.\n"
    "      -t                    Opens with default text editor.\n"
    "      -f                    Reads input from standard input and opens with TextEdit.\n"
    "      -F  --fresh           Launches the app fresh, that is, without restoring windows. Saved persistent state is lost, excluding Untitled documents.\n"
    "      -R, --reveal          Selects in the Finder instead of opening.\n"
    "      -W, --wait-apps       Blocks until the used applications are closed (even if they were already running).\n"
    "          --args            All remaining arguments are passed in argv to the application's main() function instead of opened.\n"
    "      -n, --new             Open a new instance of the application even if one is already running.\n"
    "      -j, --hide            Launches the app hidden.\n"
    "      -g, --background      Does not bring the application to the foreground.\n"
    "      -h, --header          Searches header file locations for headers matching the given filenames, and opens them.\n"
    "      -s                    For -h, the SDK to use; if supplied, only SDKs whose names contain the argument value are searched.\n"
    "                            Otherwise the highest versioned SDK in each platform is used.\n"
    "      -u, --url URL         Open this URL, even if it matches exactly a filepath\n"
    "      -i, --stdin  PATH     Launches the application with stdin connected to PATH; defaults to /dev/null\n"
    "      -o, --stdout PATH     Launches the application with /dev/stdout connected to PATH; \n"
    "          --stderr PATH     Launches the application with /dev/stderr connected to PATH to\n"
    "          --env    VAR      Add an enviroment variable to the launched process, where VAR is formatted AAA=foo or just AAA for a null string value.\n";

static const char *o_archs[] = {
    "i386", "x86_64", "x86_64h", "arm64", "arm64e", "arm64_32", "any", NULL
};

static NSString *o_selectorPath;	/* selector name for name extraction */

static int o_byte550;			/* 0x550 */
static int o_byte551;			/* 0x551 (cleared by -h) */
static NSString *o_sdkName;		/* 0x558 (-s) */
static int o_bg, o_wait, o_fresh;	/* 0x560/564/568 */
static int o_textEdit, o_textFlag;	/* 0x56c (-t) / 0x570 (-e) */
static int o_hide, o_new;		/* 0x574/578 */
static int o_noRecents;			/* 0x57c */
static int o_fileFlag;			/* 0x580 (-f) */
static int o_reveal;			/* 0x588 */
static int o_header;			/* 0x58c */
static int o_cancel;			/* 0x590 (header chooser "cancel") */
static NSURL *o_stdinURL, *o_stdoutURL, *o_stderrURL;	/* 0x598/5a0/5a8 */
static NSMutableDictionary *o_env;	/* 0x5b0 */

static struct option o_longopts[] = {
    { "arch",       required_argument, NULL,      'X' },
    { "new",        no_argument,       &o_new,    1 },
    { "background", no_argument,       &o_bg,     1 },
    { "hide",       no_argument,       &o_hide,   1 },
    { "reveal",     no_argument,       &o_reveal, 1 },
    { "wait-apps",  no_argument,       &o_wait,   1 },
    { "no-recents", no_argument,       &o_noRecents, 1 },
    { "header",     no_argument,       &o_header, 1 },
    { "fresh",      no_argument,       &o_fresh,  1 },
    { "stdin",      required_argument, NULL,      'i' },
    { "stdout",     required_argument, NULL,      'o' },
    { "stderr",     required_argument, NULL,      'E' },
    { "env",        required_argument, NULL,      'V' },
    { "url",        required_argument, NULL,      'u' },
    { NULL, 0, NULL, 0 }
};

static dispatch_group_t o_launchGroup;

static void
die(NSString *msg)
{
	fputs([msg UTF8String], stderr);
	fputc('\n', stderr);
	exit(1);
}

static void
usage(void)
{
	fprintf(stderr, o_usage, "open");
	exit(1);
}

/*
 * Tolerate strings that are not valid UTF-8 (a raw argv element): fall
 * back to the default C string encoding.
 */
static NSString *
o_argString(const char *arg)
{
	NSString *s = [NSString stringWithUTF8String:arg];
	if (s != nil)
		return s;
	s = [[NSString alloc] initWithBytes:arg length:strlen(arg)
	    encoding:[NSString defaultCStringEncoding]];
	return [s autorelease];
}

static void
o_cancelled(void)
{
	die([NSString stringWithFormat:@"Cancelled."]);
}

static BOOL
o_hasURLScheme(NSString *arg)
{
	NSUInteger i, n = [arg length];
	unichar c;

	if (n == 0)
		return NO;
	c = [arg characterAtIndex:0];
	if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z')))
		return NO;
	for (i = 1; i < n; i++) {
		c = [arg characterAtIndex:i];
		if (c == ':')
			return YES;
		if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
		    (c >= '0' && c <= '9') || c == '+' || c == '-' ||
		    c == '.')
			continue;
		return NO;
	}
	return NO;
}

/*
 * Join a list of strings with the given conjunction ("and"/"or"):
 * "A", "A and B", "A, B, C and D".
 */
static NSString *
o_join(NSArray *items, NSString *conj)
{
	if (items == nil)
		return nil;
	NSUInteger n = [items count];
	if (n == 0)
		return @"";
	switch (n) {
	case 1:
		return [NSString stringWithFormat:@"%@", [items objectAtIndex:0]];
	case 2:
		return [NSString stringWithFormat:@"%@ %@ %@",
		    [items objectAtIndex:0], conj, [items objectAtIndex:1]];
	default:
		;
	}
	NSMutableString *s = [NSMutableString string];
	for (NSUInteger i = 0; i < n; i++) {
		if (i != 0)
			[s appendString:@", "];
		if (i == n - 1)
			[s appendFormat:@"%@ ", conj];
		[s appendFormat:@"%@", [items objectAtIndex:i]];
	}
	return s;
}

/*
 * Apply a selector to each item ("path" for NSURL) to build a display-name
 * array, falling back to the item itself.
 */
static NSArray *
o_names(NSArray *items, SEL sel)
{
	if (items == nil)
		return nil;
	NSMutableArray *out = [NSMutableArray arrayWithCapacity:[items count]];
	for (id item in items) {
		id r = [item performSelector:sel];
		if (r != nil)
			[out addObject:r];
		else
			[out addObject:item];
	}
	return out;
}

/* "\nPerhaps you meant '%@'?" */
static NSString *
o_file_suggestion(NSArray *possibles)
{
	if ([possibles count] == 1) {
		NSString *p = [[[possibles objectAtIndex:0] lowercaseString]
		    stringByAppendingString:@""];
		NSString *u = [@"http://" stringByAppendingString:p];
		if ([NSURL URLWithString:u] != nil &&
		    ([p hasSuffix:@".com"] || [p hasSuffix:@".org"] ||
		    [p hasSuffix:@".net"]))
			return [NSString stringWithFormat:@"\nPerhaps you meant '%@'?",
			    [NSString stringWithString:p]];
	}
	return @"";
}

/* "The URL%s %@ do%s not refer to a file.%" */
static void
o_dieNoFileURL(NSArray *args, NSArray *others)
{
	NSUInteger n = [args count];
	if (n == 0)
		return;
	BOOL one = (n == 1);
	NSString *suffix = @"";
	NSString *first = [args objectAtIndex:0];
	if (one && [others count] == 1 && [first hasPrefix:@"file://"])
		suffix = [NSString stringWithFormat:
		    @"\nPerhaps you meant 'file:///%@'?",
		    [first substringFromIndex:7]];
	die([NSString stringWithFormat:@"The URL%s %@ do%s not refer to a file.%@",
	    one ? "" : "s",
	    o_join(args, @"and"),
	    one ? "es" : "",
	    suffix]);
}

/* "The file%s %@ do%s not exist.%" */
static void
o_dieFileMissing(NSArray *urls, NSArray *others)
{
	NSMutableArray *missing = [NSMutableArray array];
	NSFileManager *fm = [NSFileManager defaultManager];
	for (NSURL *u in urls) {
		if ([u isFileURL] &&
		    ![fm fileExistsAtPath:[u path]])
			[missing addObject:[u path]];
	}
	if ([missing count] == 0)
		return;
	NSString *suffix = @"";
	if ([others count] == 1) {
		NSString *lower = [[[others objectAtIndex:0] lowercaseString]
		    stringByAppendingString:@""];
		NSString *cand = [@"http://" stringByAppendingString:lower];
		if ([NSURL URLWithString:cand] != nil &&
		    ([lower hasSuffix:@".com"] || [lower hasSuffix:@".org"] ||
		    [lower hasSuffix:@".net"]))
			suffix = [NSString stringWithFormat:
			    @"\nPerhaps you meant '%@'?", cand];
	}
BOOL one = ([missing count] == 1);
	NSString *msg = [NSString stringWithFormat:@
	    "The file%s %@ do%s not exist.%@",
	    one ? "" : "s",
	    o_join(missing, @"and"),
	    one ? "es" : "",
	    suffix];
	die(msg);
}

/*
 * "LSCopyApplicationURLsForBundleIdentifier() failed while trying to
 * determine the application with bundle identifier %@."
 */
static void
o_dieBundleLookup(NSString *bid)
{
	die([NSString stringWithFormat:@
	    "LSCopyApplicationURLsForBundleIdentifier() failed while trying to determine the application with bundle identifier %@.",
	    bid]);
}

/* "Unable to find application named '%@'" */
static void
o_dieNoApplication(NSString *name)
{
	die([NSString stringWithFormat:@
	    "Unable to find application named '%@'", name]);
}

/* "Unable to reveal file%s %@." */
static void
o_dieReveal(NSArray *urls)
{
	NSArray *names = o_names(urls, @selector(path));
	die([NSString stringWithFormat:@
	    "Unable to reveal file%s %@.",
	    [names count] == 1 ? "" : "s",
	    o_join(names, @"and")]);
}

/* "Unable to read path '%s'" */
static void
o_dieReadPath(const char *path)
{
	NSString *s = [NSString stringWithFormat:@"Unable to read path '%s'", path];
	die(s);
}

/* "Unable to interpret '%s' as a URL" */
static void
o_dieBadURL(const char *arg)
{
	die([NSString stringWithFormat:@"Unable to interpret '%s' as a URL", arg]);
}

/* "Unable to interpret \"%s\"" */
static void
o_dieBadQuote(const char *arg)
{
	die([NSString stringWithFormat:@"Unable to interpret \"%s\"", arg]);
}

/* "Unable to interpret '%@' as a path or URL" */
static void
o_dieBadPathURL(NSString *arg)
{
	die([NSString stringWithFormat:@
	    "Unable to interpret '%@' as a path or URL", arg]);
}

/* "Unable to open temporary file.  The error was %d: %s" */
static void
o_dieTempOpen(const char *path)
{
	die([NSString stringWithFormat:@
	    "Unable to open temporary file.  The error was %d: %s",
	    errno, strerror(errno)]);
}

/* "Unable to write to temporary file %s.  The error was %d: %s" */
static void
o_dieTempWrite(const char *path)
{
	die([NSString stringWithFormat:@
	    "Unable to write to temporary file %s.  The error was %d: %s",
	    path, errno, strerror(errno)]);
}

/* "Error reading from stdin, while writing to temporary file %s.  The error was %d: %s" */
static void
o_dieTempRead(const char *path)
{
	die([NSString stringWithFormat:@
	    "Error reading from stdin, while writing to temporary file %s.  The error was %d: %s",
	    path, ferror(stdin), strerror(errno)]);
}

/* "Unable to find header file%s matching %@" */
static void
o_dieHeadersMiss(NSArray *names)
{
	BOOL one = ([names count] == 1);
	die([NSString stringWithFormat:@
	    "Unable to find header file%s matching %@",
	    one ? "" : "s",
	    o_join(names, @"or")]);
}

/* LS-style dies */
static NSString *
o_appSpan(NSString *name)
{
	return [NSString stringWithFormat:@
	    " for the application %@", name];
}

static void
o_dieAppTrash(NSString *span)
{
	die([NSString stringWithFormat:@
	    "The application%@ cannot be opened because it is in the Trash.", span]);
}

static void
o_dieAppFormat(NSString *span)
{
	die([NSString stringWithFormat:@
	    "The application%@ cannot be opened because it has an incorrect executable format.", span]);
}

static void
o_dieAppMissing(NSString *span)
{
	die([NSString stringWithFormat:@
	    "The application%@ cannot be opened because its executable is missing.", span]);
}

static void
o_dieAppError(NSString *span, id error)
{
	die([NSString stringWithFormat:@
	    "The application%@ cannot be opened, error=%@", span, error]);
}

static void
o_dieAppUnexpected(NSString *span, id error)
{
	die([NSString stringWithFormat:@
	    "The application%@ cannot be opened for an unexpected reason, error=%@", span, error]);
}

/* "Some files were not found%@." */
static void
o_dieSomeFiles(NSArray *files, NSArray *apps)
{
	NSMutableArray *missing = [NSMutableArray array];
	NSFileManager *fm = [NSFileManager defaultManager];
	for (NSURL *u in files) {
		if ([u isFileURL] && ![fm fileExistsAtPath:[u path]])
			[missing addObject:[u path]];
	}
	if ([missing count] == 0)
		return;
	NSString *sugg = @"";
	if ([apps count] == 1) {
		NSString *lower = [[[apps objectAtIndex:0] lowercaseString]
		    stringByAppendingString:@""];
		NSString *cand = [@"http://" stringByAppendingString:lower];
		if ([NSURL URLWithString:cand] != nil &&
		    ([lower hasSuffix:@".com"] || [lower hasSuffix:@".org"] ||
		    [lower hasSuffix:@".net"]))
			sugg = [NSString stringWithFormat:
			    @"\nPerhaps you meant '%@'?", cand];
	}
	if ([missing count] == 0)
		return;
	die([NSString stringWithFormat:@"Some files were not found%@.",
	    sugg]);
}

/* "The volume does not exist%@.%" */
static void
o_dieVolume(NSArray *files)
{
	NSString *note = @"";
	if ([files count] == 1) {
		NSString *path = [[[files objectAtIndex:0] path] stringByAppendingString:@""];
		if (![path hasPrefix:@"/dev"])
			note = @" Perhaps it is a stale NFS handle.";
	}
	die([NSString stringWithFormat:@"The volume does not exist%@.%@",
	    @"", note]);
}

/* "<fn>() failed%@ with error %ld%@." */
static void
o_dieFailed(NSString *fn, NSString *span, long err, NSString *tail)
{
	die([NSString stringWithFormat:@"%@() failed%@ with error %ld%@.",
	    fn, span, err, tail]);
}

static void
o_dieAlias(NSString *url, NSString *err)
{
	die([NSString stringWithFormat:@"Unable to resolve alias file %@ (%@).",
	    url, err]);
}

static void
o_dieURLFile(NSString *url)
{
	die([NSString stringWithFormat:@
	    "The URL %@ does not refer to a file which exists.", url]);
}

static void
o_dieNoApp(NSString *url, NSString *err)
{
	die([NSString stringWithFormat:@
	    "No application knows how to open URL %@ (%@).", url, err]);
}

static void
o_dieNoVol(NSArray *files)
{
	o_dieVolume(files);
}

static void
o_warnWaitPid(OSStatus rc)
{
	fprintf(stderr,
	    "Unable to block on application (GetProcessPID() returned %lu)\n",
	    (unsigned long)rc);
}

static void
o_dieKqueue(void)
{
	die([NSString stringWithFormat:@
	    "Unable to block on applications (kqueue() failed: %s)",
	    strerror(errno)]);
}

static void
o_dieKevInitial(void)
{
	die([NSString stringWithFormat:@
	    "Unable to block on applications (initial call to kevent() failed: %s)",
	    strerror(errno)]);
}

static void
o_dieKevWait(void)
{
	die([NSString stringWithFormat:@
	    "Unable to block on applications (call to kevent() failed: %s)",
	    strerror(errno)]);
}

static void
o_dieAlloc(size_t size)
{
	die([NSString stringWithFormat:@"Unable to allocate %lu bytes",
	    (unsigned long)size]);
}

/* ---------------------------------- -W wait ---------------------------------- */

static void
o_waitApps(NSMutableArray *waitApps)
{
	if (o_wait == 0 || waitApps == nil || [waitApps count] == 0)
		return;

	NSUInteger count = [waitApps count];
	ProcessSerialNumber *psns = calloc(count, sizeof(ProcessSerialNumber));
	if (psns == NULL)
		o_dieAlloc(count * sizeof(ProcessSerialNumber));

	NSUInteger i = 0;
	for (id asn in waitApps) {
		ProcessSerialNumber *sn = &psns[i];
		_LSASNExtractHighAndLowParts((LSASN)asn, &sn->highLongOfPSN,
		    &sn->lowLongOfPSN);
		i++;
	}

	pid_t *pids = calloc(count, sizeof(pid_t));
	if (pids == NULL)
		o_dieAlloc(count * sizeof(pid_t));

	NSMutableSet *seen = [[NSMutableSet alloc] init];
	int found = 0;
	for (i = 0; i < count; i++) {
		ProcessSerialNumber *sn = &psns[i];
		uint64_t key = ((uint64_t)sn->highLongOfPSN << 32) |
		    sn->lowLongOfPSN;
		NSNumber *num = [[NSNumber alloc] initWithUnsignedLongLong:key];
		if (![seen containsObject:num]) {
			OSStatus rc = GetProcessPID(sn, &pids[found]);
			if (rc != 0)
				o_warnWaitPid(rc);
			else
				found++;
			[seen addObject:num];
		}
		[num release];
	}
	[seen release];
	free(psns);

	if (found < 1) {
		free(pids);
		return;
	}

	struct kevent *kev = calloc(found, sizeof(struct kevent));
	if (kev == NULL)
		o_dieAlloc(found * sizeof(struct kevent));

	int kq = kqueue();
	if (kq == 0)
		o_dieKqueue();

	for (i = 0; i < (NSUInteger)found; i++)
		EV_SET(&kev[i], pids[i], EVFILT_PROC,
		    EV_ADD | EV_ENABLE | EV_ONESHOT, NOTE_EXIT, 0, 0);

	if (kevent(kq, kev, found, NULL, 0, NULL) == -1)
		o_dieKevInitial();

	int remaining = found;
	do {
		int rv = kevent(kq, NULL, 0, kev, found, NULL);
		if (rv == -1)
			o_dieKevWait();
		remaining -= rv;
	} while (remaining > 0);

	free(kev);
	close(kq);
	free(pids);
}

/* ---------------------------------- header search ---------------------------------- */

@interface HeaderOpenState : NSObject
{
	NSMutableArray *_remaining;
	NSMutableDictionary *_headerToPaths;
	BOOL _finished;
}
- (id)initWithRemainingHeaders:(NSArray *)headers;
- (BOOL)finished;
- (void)visitHeader:(NSString *)name atPath:(NSString *)headerPath;
- (void)visitPath:(NSString *)path;
- (void)performFastPathSearch;
- (NSDictionary *)headersToHeaderPaths;
@end

static NSArray *o_headers_dirs;	/* fallback search roots */

static NSArray *
o_platformSDKDirs(NSURL *platformURL)
{
	NSFileManager *fm = [NSFileManager defaultManager];
	NSURL *sdksURL = [platformURL URLByAppendingPathComponent:@"SDKs"
	    isDirectory:YES];
	NSArray *list = [fm contentsOfDirectoryAtURL:sdksURL
	    includingPropertiesForKeys:nil options:0 error:NULL];
	NSMutableArray *d = [NSMutableArray array];
	for (NSURL *u in list) {
		if ([[u pathExtension] isEqualToString:@"sdk"])
			[d addObject:u];
	}
	return d;
}

@implementation HeaderOpenState

- (id)initWithRemainingHeaders:(NSArray *)headers
{
	if ((self = [super init]) != nil) {
		_remaining = [[NSMutableArray alloc]
		    initWithArray:headers copyItems:NO];
		_headerToPaths = [[NSMutableDictionary alloc] init];
		_finished = NO;
	}
	return self;
}

- (void)dealloc
{
	[_remaining release];
	[_headerToPaths release];
	[super dealloc];
}

- (BOOL)finished
{
	return _finished;
}

- (void)visitHeader:(NSString *)name atPath:(NSString *)headerPath
{
	NSString *path = [NSString stringWithFormat:@"%@%@", headerPath, name];
	NSMutableArray *list = [_headerToPaths objectForKey:name];
	if (list == nil) {
		list = [NSMutableArray array];
		[_headerToPaths setObject:list forKey:name];
	}
	[list addObject:path];
	if ([_remaining containsObject:name]) {
		[_remaining removeObject:name];
		if ([_remaining count] == 0)
			_finished = YES;
	}
}

- (void)visitPath:(NSString *)path
{
	NSFileManager *fm = [NSFileManager defaultManager];
	BOOL isDir = NO;
	if (![fm fileExistsAtPath:path isDirectory:&isDir])
		return;
	NSArray *sub = [fm contentsOfDirectoryAtPath:path error:NULL];
	for (NSString *name in sub) {
		if (isDir) {
			if ([_remaining containsObject:name])
				[self visitHeader:name atPath:path];
			NSString *next = [path stringByAppendingPathComponent:name];
			[self visitPath:next];
		} else if ([_remaining containsObject:name]) {
			[self visitHeader:name atPath:path];
		}
	}
}

- (void)performFastPathSearch
{
	for (NSString *dir in o_headers_dirs) {
		[self visitPath:dir];
		if (_finished)
			return;
	}
}

- (NSDictionary *)headersToHeaderPaths
{
	return _headerToPaths;
}

@end

static NSString *
o_developerDir(void)
{
	char *dev = getenv("DEVELOPER_DIR");
	if (dev != NULL)
		return [NSString stringWithUTF8String:dev];
	NSTask *task = [[NSTask alloc] init];
	[task setLaunchPath:@"/usr/bin/xcode-select"];
	[task setArguments:[NSArray arrayWithObject:@"-print-path"]];
	NSPipe *pipe = [NSPipe pipe];
	[task setStandardOutput:pipe];
	[task launch];
	[task waitUntilExit];
	NSData *data = [[pipe fileHandleForReading] readDataToEndOfFile];
	[task release];
	NSString *dir = [[[NSString alloc] initWithData:data
	    encoding:NSUTF8StringEncoding] autorelease];
	dir = [dir stringByTrimmingCharactersInSet:
	    [NSCharacterSet whitespaceAndNewlineCharacterSet]];
	if ([dir length] == 0)
		dir = @"/Applications/Xcode.app";
	return dir;
}

static NSArray *
o_discoverSDKs(void)
{
	NSMutableArray *sdks = [NSMutableArray array];
	NSString *dev = o_developerDir();
	if ([[dev pathExtension] isEqualToString:@"app"])
		dev = [dev stringByAppendingPathComponent:@"Contents/Developer"];
	NSString *platforms = [dev stringByAppendingPathComponent:@"Platforms"];
	NSURL *platformsURL = [NSURL fileURLWithPath:platforms];
	NSFileManager *fm = [NSFileManager defaultManager];
	if (![fm fileExistsAtPath:platforms]) {
		fprintf(stderr, "Warning: Xcode platforms folder not found at \"%s\". You may have invalid DEVELOPER_DIR or stale xcode-select setting.\n",
		    [platforms UTF8String]);
		return sdks;
	}
	NSArray *entries = [fm contentsOfDirectoryAtURL:platformsURL
	    includingPropertiesForKeys:nil options:0 error:NULL];
	for (NSURL *u in entries) {
		if ([[u pathExtension] isEqualToString:@"platform"])
			[sdks addObjectsFromArray:o_platformSDKDirs(u)];
	}
	return sdks;
}

static NSArray *
o_sdkPaths(void)
{
	NSMutableArray *paths = [NSMutableArray array];
	for (NSURL *u in o_discoverSDKs()) {
		NSString *name = [u lastPathComponent];
		if (o_sdkName != nil &&
		    [name rangeOfString:o_sdkName].location == NSNotFound)
			continue;
		if ([name rangeOfString:@".Internal"
		    ].location != NSNotFound)
			continue;
		[paths addObject:[u path]];
	}
	return [paths sortedArrayUsingSelector:@selector(compare:)];
}

/* ---------------------------------- main ---------------------------------- */

int
main(int argc, char **argv)
{
	NSAutoreleasePool *pool = [[NSAutoreleasePool alloc] init];
	NSMutableArray *files = [NSMutableArray array];	/* SP+128 */
	NSMutableArray *badFileURLs = [NSMutableArray array];
	NSMutableArray *urls = [NSMutableArray array];	/* SP+112 */
	NSMutableArray *args = nil;			/* SP+48 */
	NSString *appName = nil;			/* SP+56 */
	NSString *appString = nil;			/* SP+32 */
	CFURLRef defaultEditor = NULL;			/* SP+40 */
	NSString *arch = nil;				/* SP+64 */
	NSMutableArray *postOptFiles = nil;		/* SP+88 */

	o_byte551 = 0;
	o_env = nil;

	/*
	 * Pre-scan for "--args": collect the trailing arguments into args
	 * (SP+48) and blank them so getopt never sees them.
	 */
	{
		int i;
		for (i = 1; i < argc; i++) {
			if (strcmp(argv[i], "--args") == 0) {
				int j;
				for (j = i + 1; j < argc; j++) {
					NSString *a = o_argString(argv[j]);
					if (a != nil) {
						if (args == nil)
							args = [NSMutableArray array];
						[args addObject:a];
					}
					argv[j][0] = '\0';
				}
				argc = i;
				break;
			}
		}
	}

	optreset = 1;
	while (1) {
		int c = getopt_long(argc, argv,
		    "etfFb:a:s:WRnghHvji:o:E:u:", o_longopts, NULL);
		if (c == -1)
			break;
		if (c == '?')
			usage();
		switch (c) {
		case 'E':
			o_stderrURL = [[NSURL
			    fileURLWithFileSystemRepresentation:optarg
			    isDirectory:NO relativeToURL:nil] retain];
			break;
		case 'F':
			o_fresh = 1;
			break;
		case 'H':
			o_header = 1;
			break;
		case 'R':
			o_reveal = 1;
			break;
		case 'W':
			o_wait = 1;
			break;
		case 'X':	/* --arch */
			[arch release];
			arch = [o_argString(optarg) retain];
			break;
		case 'a':
			appName = [o_argString(optarg) retain];
			break;
		case 'b': {
			NSString *bid = o_argString(optarg);
			[appString release];
			appString = [bid retain];
			break;
		}
		case 'e':
			o_textFlag = 1;
			break;
		case 'f':
			o_fileFlag = 1;
			break;
		case 'g':
			o_bg = 1;
			break;
		case 'h':
			o_header = 1;
			o_byte551 = 0;
			break;
		case 'i':
			o_stdinURL = [[NSURL
			    fileURLWithFileSystemRepresentation:optarg
			    isDirectory:NO relativeToURL:nil] retain];
			break;
		case 'j':
			o_hide = 1;
			break;
		case 'n':
			o_new = 1;
			break;
		case 'o':
			o_stdoutURL = [[NSURL
			    fileURLWithFileSystemRepresentation:optarg
			    isDirectory:NO relativeToURL:nil] retain];
			break;
		case 's':
			o_sdkName = [o_argString(optarg) retain];
			break;
		case 't':
			o_textEdit = 1;
			break;
		case 'u': {
			NSString *u = o_argString(optarg);
			if (u != nil && [u rangeOfString:@":"].location !=
			    NSNotFound) {
				NSURL *url = [NSURL URLWithString:u];
				if (url != nil) {
					[urls addObject:url];
					break;
				}
			}
			o_dieBadURL(optarg);
			break;
		}
		case 'V': {	/* --env */
			const char *e = optarg;
			if (e == NULL || *e == '\0')
				break;
			const char *eq = strchr(e, '=');
			if (eq != NULL) {
				if (eq - e >= 2) {
					NSString *name = [[NSString alloc]
					    initWithBytes:e length:eq - e
					    encoding:NSUTF8StringEncoding];
					NSString *value = [[NSString alloc]
					    initWithCString:eq + 1
					    encoding:NSUTF8StringEncoding];
					if (o_env == nil)
						o_env = [[NSMutableDictionary
						    dictionary] retain];
					[o_env setObject:value forKey:name];
					[name release];
					[value release];
				} else {
					fprintf(stderr, "Ignoring incorrectly formatted enviroment variable %s\n", e);
				}
			} else {
				NSString *name = o_argString(e);
				if (o_env == nil)
					o_env = [[NSMutableDictionary
					    dictionary] retain];
				[o_env setObject:@"" forKey:name];
			}
			break;
		}
		case 'v':
			break;
		default:
			break;
		}
	}

	if (o_fileFlag) {
		/* -f: drain stdin into a temp file, then open with TextEdit */
		char path[256];
		int fd = -1;
		strncpy(path, "/tmp/open_XXXXXXXX.txt", sizeof(path));
		fd = mkstemps(path, 4);
		if (fd < 0)
			o_dieTempOpen(path);
		NSString *p = [NSString stringWithUTF8String:path];
		[files addObject:[NSURL fileURLWithPath:p]];
		char buf[4096];
		ssize_t n;
		while ((n = read(STDIN_FILENO, buf, sizeof(buf))) > 0) {
			if (write(fd, buf, n) < 0) {
				o_dieTempWrite(path);
			}
		}
		if (ferror(stdin))
			o_dieTempRead(path);
		close(fd);
		o_textFlag = 1;
		appString = @"com.apple.TextEdit";
	}

	/* -t: default text editor */
	if (o_textEdit) {
		defaultEditor = LSCopyDefaultApplicationURLForContentType(
		    CFSTR("txt"), kLSRolesEditor, NULL);
		if (defaultEditor == NULL) {
			appString = @"com.apple.TextEdit";
			defaultEditor = NULL;
		}
	}

	if (!o_fileFlag && (appString == nil)) {
		if (o_textFlag && appString == nil)
			appString = @"com.apple.TextEdit";
	}

	/* Remaining arguments are file names (SP+88) */
	postOptFiles = [NSMutableArray arrayWithCapacity:(argc - optind)];
	while (optind < argc) {
		NSString *a = o_argString(argv[optind++]);
		if (a == nil)
			o_dieBadPathURL([NSString stringWithFormat:@"<nil>"]);
		if ([a length] == 0)
			a = @".";
		[postOptFiles addObject:a];
	}

	if (o_header) {
		/* header mode: search SDK + fallback roots for headers */
		o_headers_dirs = [NSArray arrayWithObjects:
		    @"/usr/include", @"/usr/local/include",
		    @"/System/Library/Frameworks",
		    @"/System/Library/PrivateFrameworks",
		    @"/Library/Frameworks",
		    @"~/Library/Frameworks",
		    @"/Library/PrivateFrameworks",
		    @"~/Library/PrivateFrameworks", nil];
		NSMutableArray *roots = [NSMutableArray
		    arrayWithArray:o_headers_dirs];
		[roots addObjectsFromArray:o_sdkPaths()];

		HeaderOpenState *state = [[HeaderOpenState alloc]
		    initWithRemainingHeaders:postOptFiles];
		[state performFastPathSearch];
		/* search framework roots for Frameworks specifically */
		for (NSString *root in o_headers_dirs) {
			if ([root hasSuffix:@"Frameworks"])
				[state visitPath:root];
			if ([state finished])
				break;
		}
		NSDictionary *table = [state headersToHeaderPaths];

		/* any file names with no match at all? */
		NSMutableArray *noMatch = [NSMutableArray array];
		for (NSString *name in postOptFiles) {
			NSArray *hits = [table objectForKey:name];
			if ((hits == nil) || ([hits count] == 0))
				[noMatch addObject:name];
		}
		if ([noMatch count] != 0)
			o_dieHeadersMiss(noMatch);

		/* pick a header per requested name */
		NSMutableArray *answers = [NSMutableArray array];
		for (NSString *name in postOptFiles) {
			NSArray *hits = [table objectForKey:name];
			if ([hits count] == 1) {
				[answers addObject:[hits objectAtIndex:0]];
				continue;
			}
			/* interactive chooser */
			printf("%s?\n", [name UTF8String]);
			puts("[0]\tcancel");
			puts("[1]\tall");
			putchar('\n');
			int idx = 2;
			for (NSString *h in hits) {
				printf("[%d]\t%s\n", idx++, [h UTF8String]);
			}
			printf("\nWhich header(s) for \"%s\"? ", [name UTF8String]);
			fflush(stdout);
			char buf[1024];
			int selected = -1;
			do {
				if (fgets(buf, sizeof(buf), stdin) == NULL)
					break;
				if (strchr(buf, '\n') == NULL)
					continue;
				NSString *reply = o_argString(buf);
				NSScanner *scanner = [NSScanner
				    scannerWithString:reply];
				[scanner setCharactersToBeSkipped:[NSCharacterSet
				    whitespaceAndNewlineCharacterSet]];
				BOOL cur = NO;
				int n = 0;
				int v;
				while ([scanner scanInt:&v]) {
					n++;
					if (v == 0) {
						o_cancel = 1;
					} else if (v == 1) {
						[answers addObjectsFromArray:hits];
						cur = YES;
					} else if (v == 2 && (long)[hits count] + 2 > v) {
						[answers addObject:[hits
						    objectAtIndex:(v - 2)]];
						cur = YES;
					} else if (v > 2 && (long)[hits count] + 2 >= v) {
						[answers addObject:[hits
						    objectAtIndex:(v - 2)]];
						cur = YES;
					} else {
						fprintf(stderr, "Please enter values in the range 0 through %u\n",
						    (unsigned)([hits count] + 1));
						[scanner setScanLocation:[reply length]];
					}
					if ([scanner isAtEnd] && n > 0)
						break;
				}
				selected = n;
			} while (selected < 1);
			if (o_cancel) {
				o_cancelled();
				/* NOTREACHED */
			}
		}

		/* deduplicate answers, preserving order */
		NSMutableSet *seen = [NSMutableSet setWithCapacity:[answers count]];
		NSMutableArray *out = [NSMutableArray arrayWithCapacity:[answers count]];
		for (NSString *a in answers) {
			if ([seen member:a] == nil) {
				[seen addObject:a];
				[out addObject:a];
			}
		}
		[state release];
		[files addObjectsFromArray:out];
	} else {
		/*
		 * Normal mode: classify each remaining argument as a file
		 * path, a URL, or an ambiguous both.
		 */
		for (NSString *arg in postOptFiles) {
			if (!o_hasURLScheme(arg)) {
				/* plain file path */
				[files addObject:[NSURL fileURLWithPath:arg]];
				continue;
			}
			if ([[arg lowercaseString] hasPrefix:@"file:"]) {
				NSURL *fu = [NSURL URLWithString:arg];
				if (fu == nil || [[fu path] length] == 0)
					[badFileURLs addObject:arg];
			}
			NSURL *fileURL = [NSURL fileURLWithPath:arg];
			if (![[NSFileManager defaultManager] fileExistsAtPath:arg]) {
				/* not a file; hand to LS as a URL */
				[files addObject:[NSURL URLWithString:arg] ?: (id)[NSURL fileURLWithPath:arg]];
				continue;
			}
			if (o_textFlag || o_textEdit || o_reveal) {
				[files addObject:fileURL];
				continue;
			}
			/* ambiguous: ask */
			printf("%s?\n", [arg UTF8String]);
			puts("[0]\tcancel");
			printf("[1]\tOpen the file %s\n", [arg fileSystemRepresentation]);
			printf("[2]\tOpen the URL  %s\n", [[ [NSURL URLWithString:arg] absoluteString] UTF8String]);
			printf("\nWhich did you mean? ");
			fflush(stdout);
			int choice = 0;
			if (scanf(" %d", &choice) != 1)
				o_cancelled();
			char buf[1024];
			if (fgets(buf, sizeof(buf), stdin) == NULL) {
				if (ferror(stdin))
					;
			}
			while (strchr(buf, '\n') == NULL &&
			    fgets(buf, sizeof(buf), stdin) != NULL)
				;
			if (choice <= 0)
				o_cancelled();
			if (choice == 1)
				[files addObject:fileURL];
			else
				[files addObject:[NSURL URLWithString:arg] ?: (id)[NSURL fileURLWithPath:arg]];
		}
	}

	o_dieNoFileURL(badFileURLs, postOptFiles);

	/* missing-file die; must precede -a/-b resolution */
	o_dieFileMissing(files, postOptFiles);

	NSArray *opened = [files arrayByAddingObjectsFromArray:urls];

	if (appName != nil) {
		/* -a: resolve application name, filter URLs to that app */
		NSString *path = [[NSWorkspace sharedWorkspace]
		    fullPathForApplication:appName];
		if (path == nil)
			o_dieNoApplication(appName);
		appString = path;
	} else if (appString != nil) {
		/* -b: resolve the bundle identifier to the application */
		CFArrayRef matches = LSCopyApplicationURLsForBundleIdentifier(
		    (CFStringRef)appString, NULL);
		NSURL *resolved = nil;
		if (matches != NULL) {
			NSArray *appURLs = (NSArray *)matches;
			if ([appURLs count] > 0)
				resolved = [appURLs objectAtIndex:0];
			CFRelease(matches);
		}
		if (resolved == nil)
			o_dieBundleLookup(appString);
		[appString release];
		appString = [[resolved path] retain];
	}

	if ([opened count] == 0 && appString == nil && defaultEditor == NULL)
		usage();

	if (arch != nil) {
		BOOL ok = NO;
		NSString *lower = [arch lowercaseString];
		for (int i = 0; o_archs[i] != NULL; i++)
			if ([lower isEqualToString:
			    [NSString stringWithUTF8String:o_archs[i]]]) {
				ok = YES;
				break;
			}
		if (!ok) {
			die([NSString stringWithFormat:
			    @"Unable to decode architecture option %@", arch]);
		}
	}

	if (o_reveal) {
		NSMutableArray *fail = [NSMutableArray array];
		NSFileManager *fm = [NSFileManager defaultManager];
		for (NSURL *u in files) {
			NSString *path = [u path];
			BOOL isDir;
			if ([fm fileExistsAtPath:path isDirectory:&isDir] &&
			    [[NSWorkspace sharedWorkspace] selectFile:path
				inFileViewerRootedAtPath:@"/"])
				continue;
			[fail addObject:u];
		}
		if ([fail count] == 0)
			_exit(0);
		o_dieReveal(fail);
	}

	o_selectorPath = @"path";

	if (opened) {
		NSMutableDictionary *lsOpts = [NSMutableDictionary dictionary];
		[lsOpts setObject:[NSNumber numberWithBool:YES]
		    forKey:(id)_kLSOpenOptionSynchronousKey];
		NSMutableArray *waitApps = nil;
		if (o_wait)
			waitApps = [[NSMutableArray alloc] init];
		dispatch_group_t group = dispatch_group_create();
		o_launchGroup = group;
		typedef void (^o_CompBlock)(LSASN theApp, Boolean didOpen,
		    CFErrorRef error);
		o_CompBlock comp = ^(LSASN theApp, Boolean didOpen,
		    CFErrorRef error) {
			if (error != NULL) {
				OSStatus code = (OSStatus)CFErrorGetCode(error);
				NSString *dom = [(NSError *)error domain];
				NSString *span = @"";
				if (appString != nil)
					span = o_appSpan(appString);
				if (dom != nil &&
				    [dom isEqualToString:(NSString *)kCFErrorDomainOSStatus]) {
					switch (code) {
					case -10827:
						o_dieAppMissing(span);
						break;
					case -10661:
						o_dieAppFormat(span);
						break;
					case -10660:
						o_dieAppTrash(span);
						break;
					case -43:
						o_dieSomeFiles(opened, [NSArray array]);
						break;
					case -35:
						o_dieNoVol(opened);
						break;
					default:
						(void)dom;
						o_dieFailed(@"_LSOpenURLsWithCompletionHandler", span,
						    (long)code, @"");
						break;
					}
				} else if (dom != nil &&
				    [dom isEqualToString:(NSString *)kCFErrorDomainPOSIX]) {
					o_dieAppError(span, (id)error);
				} else {
					o_dieAppUnexpected(span, (id)error);
				}
			}
			if (theApp != NULL && waitApps != nil) {
				@synchronized (waitApps) {
					[waitApps addObject:(id)theApp];
				}
			}
			if (didOpen == 0) {
				if ([o_env count] != 0) {
					NSString *wmsg;
					if (o_stdinURL != nil || o_stdoutURL != nil ||
					    o_stderrURL != nil)
						wmsg = [NSString stringWithFormat:@
						    "Application %@ was already running and so the additional environment variables and redirected stdin/stdout/stderr provided could not be set.\n",
						    appString];
					else
						wmsg = [NSString stringWithFormat:@
						    "Application %@ was already running and so the additional environment variables could not be set.\n",
						    appString];
					fputs([wmsg UTF8String], stderr);
				} else if (o_stdinURL != nil || o_stdoutURL != nil ||
				    o_stderrURL != nil) {
					NSString *wmsg = [NSString stringWithFormat:@
					    "Application %@ was already running and so the redirected stdin/stdout/stderr provided could not be set\n",
					    appString];
					fputs([wmsg UTF8String], stderr);
				}
			}
			dispatch_group_leave(group);
		};
		NSMutableDictionary *groups = [NSMutableDictionary dictionary];
		NSMutableDictionary *appFor = [NSMutableDictionary dictionary];
		NSURL *fixedApp = nil;
		NSString *fixedKey = nil;
		if (appString != nil) {
			if ([appString hasPrefix:@"/"]) {
				fixedApp = [NSURL fileURLWithPath:appString];
			} else {
				CFArrayRef matches =
				    LSCopyApplicationURLsForBundleIdentifier(
				    (CFStringRef)appString, NULL);
				if (matches != NULL) {
					if (CFArrayGetCount(matches) > 0)
						fixedApp = [[(NSURL *)
						    CFArrayGetValueAtIndex(matches, 0)
						    retain] autorelease];
					CFRelease(matches);
				}
				if (fixedApp == nil)
					o_dieBundleLookup(appString);
			}
			fixedKey = [fixedApp path];
		}
		for (NSURL *u in opened) {
			NSURL *appURL = fixedApp;
			NSString *key = fixedKey;
			if (appURL == nil) {
				CFErrorRef lerr = NULL;
				CFURLRef def = LSCopyDefaultApplicationURLForURL(
				    (CFURLRef)u, kLSRolesAll, &lerr);
				if (def == NULL) {
					NSString *desc = nil;
					if (lerr != NULL &&
					    ![[u scheme] isEqualToString:@"file"])
						desc = [(NSError *)lerr description];
					o_dieNoApp([u absoluteString], desc);
				}
				appURL = [(NSURL *)def autorelease];
				key = [appURL path];
			}
			NSMutableArray *arr = [groups objectForKey:key];
			if (arr == nil) {
				arr = [NSMutableArray array];
				[groups setObject:arr forKey:key];
				[appFor setObject:appURL forKey:key];
			}
			[arr addObject:u];
		}
		for (NSString *key in groups) {
			NSURL *appURL = [appFor objectForKey:key];
			NSArray *urlArray = [groups objectForKey:key];
			dispatch_group_enter(group);
			(void)_LSOpenURLsWithCompletionHandler(
			    (CFArrayRef)urlArray, (CFURLRef)appURL,
			    (CFDictionaryRef)lsOpts, comp);
		}
		dispatch_group_wait(group, DISPATCH_TIME_FOREVER);
		dispatch_release(group);

		if (o_wait) {
			/* kqueue-wait for app processes */
			o_waitApps(waitApps);
			[waitApps release];
		}
	}

	[pool drain];
	_exit(0);
}