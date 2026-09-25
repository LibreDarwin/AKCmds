/*
 * tops - in-place textual substitution tool for source files.
 *
 * Clean-room implementation of Apple's /usr/bin/tops.  Behavior was
 * recovered from the binary by disassembly plus black-box probing:
 *
 *   - rules come from a -scriptfile or are reconstructed from the
 *     command line into the same textual form a script file contains,
 *     then parsed identically.  A rule keyword may be given with or
 *     without a leading dash; every keyword that names an operand
 *     consumes the next argument and quotes it, so operand tokens may
 *     be passed bare.
 *
 *   - the parse string is scanned into find, replace and replacemethod
 *     rules.  A replacement of "same" leaves the matched text unchanged;
 *     where- clauses restrict a match to the values captured by named
 *     tokens, within- clauses run sub-rules over a named token, and
 *     error/warning attach a message.
 *
 *   - patterns are matched at the token level: runs of characters, the
 *     parentheses that group argument lists and the angle-bracket names
 *     inside them, and named <foo> tokens acting as wildcards.
 *
 *   - diagnostics go to stdout except for file read/write errors, which
 *     go to stderr.  Progress is a two-row bar whose bottom row fills as
 *     bytes are consumed; every dot is flushed immediately.
 *
 * Copyright (C) 2026, LibreDarwin
 * SPDX-License-Identifier: BSD-3-Clause
 */

#import <Foundation/Foundation.h>

#include <ctype.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

__attribute__((used))
static const char kNTVersion[] = "@(#)PROGRAM:tops  PROJECT:AKCmds-350";

/* ------------------------------------------------------------------ */
/* Exact usage text printed on -help, on missing rules and on errors.  */
/* ------------------------------------------------------------------ */

static const char *kUsage[] = {
    "tops [-help] [-dont] [-semiverbose] [-verbose] [-nocontext] [-nofileinfo]",
    "     (-scriptfile script_name)                                      |    ",
    "     (find <search_pattern>                                              ",
    "         [where (<symbol>...) isOneOf {(<match>...)...}] ...)       |    ",
    "     (replace <search_pattern> with <replacement_pattern> | same         ",
    "         [where (<symbol>...) isOneOf {(<match>...)...}]...              ",
    "         [within (<symbol>) {...}]...                                    ",
    "         [error <message>]                                               ",
    "         [warning <message>])                                       |    ",
    "     (replacemethod <selector> with <new_selector>                       ",
    "         { [replace <symbol> with <symbol_replacement>]... }             ",
    "         [where (<symbol>...) isOneOf {(<match> ...)...}]...             ",
    "         [within (<symbol>) {...}]...                                    ",
    "         [error <message>]                                               ",
    "         [warning <message>])                                            ",
    "     [-classfile classfile]                                              ",
    "     [filename ...]                                                      ",
    NULL};

static void printUsage(void)
{
    int i;
    for (i = 0; kUsage[i] != NULL; i++)
        puts(kUsage[i]);
}

/* ------------------------------------------------------------------ */
/* Token model shared by the parser, the matcher and the replacer.     */
/* ------------------------------------------------------------------ */

enum {
    TPTokenWS,
    TPTokenWord,
    TPTokenParen,
    TPTokenBrace,
    TPTokenComma,
    TPTokenNamed,
    TPTokenOther
};

@interface TPToken : NSObject {
@public
    int type;
    NSRange range;
    int line;
}
@property (retain) NSString *text;
@property (retain) NSString *key;
@end

@implementation TPToken
@synthesize text;
@synthesize key;
@end

static TPToken *makeToken(NSString *src, NSRange r, int type, int line)
{
    TPToken *t = [[TPToken alloc] init];
    t->type = type;
    t->range = r;
    t->line = line;
    t.text = [src substringWithRange:r];
    t.key = t.text;
    return [t autorelease];
}

static BOOL isDelim(unichar c)
{
    if (isalnum(c) || c == '_' || c == '$')
        return NO;
    return YES;
}

static BOOL skipRegionAt(NSString *src, NSUInteger i, NSUInteger *endPast)
{
    NSUInteger len = [src length];
    unichar c = [src characterAtIndex:i];

    if (c == '/' && i + 1 < len) {
        unichar d = [src characterAtIndex:i + 1];
        if (d == '/') {
            NSUInteger j = i + 2;
            while (j < len && [src characterAtIndex:j] != '\n')
                j++;
            *endPast = j;
            return YES;
        }
        if (d == '*') {
            NSUInteger j = i + 2;
            while (j + 1 < len &&
                   !([src characterAtIndex:j] == '*' &&
                     [src characterAtIndex:j + 1] == '/'))
                j++;
            *endPast = (j + 1 < len) ? j + 2 : len;
            return YES;
        }
        return NO;
    }

    if (c == '"' || c == '\'') {
        unichar quote = c;
        NSUInteger j = i + 1;
        while (j < len) {
            unichar d = [src characterAtIndex:j];
            if (d == '\\') {
                j += 2;
                continue;
            }
            if (d == quote) {
                j++;
                break;
            }
            if (d == '\n')
                break;
            j++;
        }
        if (j > len)
            j = len;
        *endPast = j;
        return YES;
    }

    return NO;
}

/* Split src into tokens.  <name> becomes one named token (no brackets). */
static NSArray *tokenizeString(NSString *src)
{
    NSMutableArray *out = [NSMutableArray array];
    NSUInteger len = [src length];
    NSUInteger i = 0;
    int line = 1;

    while (i < len) {
        unichar c = [src characterAtIndex:i];
        NSUInteger start = i;
        int type = TPTokenOther;
        NSUInteger regionEnd = 0;

        if (skipRegionAt(src, i, &regionEnd)) {
            NSUInteger k;
            for (k = i; k < regionEnd; k++)
                if ([src characterAtIndex:k] == '\n')
                    line++;
            [out addObject:makeToken(src, NSMakeRange(i, regionEnd - i),
                                    TPTokenOther, line)];
            i = regionEnd;
            continue;
        }

        if (isspace(c)) {
            while (i < len && isspace([src characterAtIndex:i])) {
                if ([src characterAtIndex:i] == '\n')
                    line++;
                i++;
            }
            type = TPTokenWS;
        } else if (c == '(' || c == ')') {
            type = TPTokenParen;
            i++;
        } else if (c == '{' || c == '}') {
            type = TPTokenBrace;
            i++;
        } else if (c == ',') {
            type = TPTokenComma;
            i++;
        } else if (c == '<') {
            NSUInteger j = i + 1;
            while (j < len && [src characterAtIndex:j] != '>')
                j++;
            NSUInteger end = j < len ? j + 1 : j;
            TPToken *t = makeToken(src, NSMakeRange(start, end - start),
                                  TPTokenNamed, line);
            t.key = [src substringWithRange:NSMakeRange(start + 1,
                                                        end - start - 2)];
            [out addObject:t];
            i = end;
            continue;
        } else if (!isDelim(c)) {
            while (i < len) {
                c = [src characterAtIndex:i];
                if (isspace(c) || isDelim(c))
                    break;
                i++;
            }
            type = TPTokenWord;
        } else {
            i++;
            type = TPTokenOther;
        }
        [out addObject:makeToken(src, NSMakeRange(start, i - start), type, line)];
    }
    return out;
}

/* ------------------------------------------------------------------ */
/* Progress bar state (per-run globals, as the oracle keeps them).     */
/* ------------------------------------------------------------------ */

static BOOL gBarRow1Drawn = NO;
static long gBarDots = 0;

static void updateStatusBar(unsigned long processed, unsigned long total)
{
    /* First call for a file: draw the top row in full and open the
     * bottom row with an opening bracket. */
    if (!gBarRow1Drawn) {
        printf("[0%%................25%%................50%%................75%%..............100%%]\n");
        putchar('[');
        fflush(stdout);
        gBarRow1Drawn = YES;
        gBarDots = 0;
    }

    if (processed < total) {
        long target = total ? (long)(processed * 77.0 / (double)total) : 77;
        if (target < 0)
            target = 0;
        while (gBarDots < target) {
            putchar('.');
            fflush(stdout);
            gBarDots++;
        }
    } else {
        while (gBarDots < 77) {
            putchar('.');
            fflush(stdout);
            gBarDots++;
        }
        printf("]\n");
        gBarRow1Drawn = NO;
    }
}

/* The replacemethod bar is a fixed two-row widget: the top row is the
 * usual percentage scale and the lower row is 77 cells wide.  With n rules
 * the report for rule k (zero based) is printed at cell floor(77*k/n), the
 * payload at 77-ceil(20/n), and the closing run is ceil(20/n) cells.  The
 * first rule's report therefore lands before the bar opens. */
#define RM_BAR_CELLS 77
#define RM_TAIL_CELLS 20

static int rmRuleCount = 1;
static int rmBarCell;
static BOOL rmIsTTY;

static int rmCellForRule(int k)
{
    return (RM_BAR_CELLS * k) / rmRuleCount;
}

static int rmTailCells(void)
{
    return (RM_TAIL_CELLS + rmRuleCount - 1) / rmRuleCount;
}

/* The top row is the plain percentage scale. */
static void rmDrawTopRow(void)
{
    printf("[0%%................25%%................50%%................75%%..............100%%]\n");
    fflush(stdout);
}

/* The lower row starts here; on a terminal the first rule's report is
 * printed between the two rows, so the bracket is opened separately. */
static void rmOpenLowerRow(void)
{
    putchar('[');
    rmBarCell = 0;
    fflush(stdout);
}

static void openReplaceMethodBar(void)
{
    rmDrawTopRow();
    rmOpenLowerRow();
}

/* Extend the lower row with dots up to the given cell. */
static void fillReplaceMethodBar(int cell)
{
    while (rmBarCell < cell) {
        putchar('.');
        rmBarCell++;
    }
    fflush(stdout);
}

static void closeReplaceMethodBar(void)
{
    fillReplaceMethodBar(RM_BAR_CELLS);
    printf("]\n");
    fflush(stdout);
}

/* ------------------------------------------------------------------ */
/* A single parsed rule.                                               */
/* ------------------------------------------------------------------ */

enum {
    RRFind,
    RRReplace,
    RRReplaceMethod
};

@interface TPRule : NSObject {
@public
    int type;
    int matches;                 /* reset per file */
    BOOL isSame;
}
@property (retain) NSString *pattern;
@property (retain) NSString *replacement;
@property (retain) NSString *errorMsg;
@property (retain) NSString *warningMsg;
@property (retain) NSArray *whereClauses;
@property (retain) NSArray *withinSymbols;  /* within: "<x>" (one per clause)      */
@property (retain) NSArray *withinRules;    /* within: array of NSArray of TPRule  */
@property (retain) NSArray *subRules;       /* replacemethod: array of TPRule      */
@end

@implementation TPRule
@synthesize pattern, replacement, errorMsg, warningMsg;
@synthesize whereClauses, withinSymbols, withinRules, subRules;
@end

/* ------------------------------------------------------------------ */
/* Parse error report.                                                 */
/* ------------------------------------------------------------------ */

@interface TopsParser : NSObject {
@public
    NSString *src;
    NSUInteger pos;
    BOOL failed;
    int lastError;
}
- (id) initWithString:(NSString *)s;
- (NSArray *) parse;
@end

@implementation TopsParser

- (id) initWithString:(NSString *)s
{
    self = [super init];
    if (self) {
        src = [s retain];
        pos = 0;
        failed = NO;
        lastError = 0;
    }
    return self;
}

- (void) dealloc
{
    [src release];
    [super dealloc];
}

- (void) skipWS
{
    NSUInteger len = [src length];
    while (pos < len && isspace([src characterAtIndex:pos]))
        pos++;
}

- (unsigned long) lineAt:(NSUInteger)p
{
    unsigned long line = 1, i = 0;
    NSUInteger len = [src length];
    while (i < len && i < p) {
        if ([src characterAtIndex:i] == '\n')
            line++;
        i++;
    }
    return line;
}

/* Read a "..." string growing from pos (pos must sit on the first ").
 * Returns the string or nil; on failure records the exact position. */
- (NSString *) parseQuoted
{
    NSUInteger len = [src length];
    NSUInteger i = pos;

    if (i >= len || [src characterAtIndex:i] != '"') {
        failed = YES;
        lastError = 1;
        return nil;
    }
    NSMutableString *out = [NSMutableString string];
    i++;
    while (i < len) {
        unichar c = [src characterAtIndex:i];
        if (c == '"') {
            i++;
            pos = i;
            return out;
        }
        if (c == '\\' && i + 1 < len) {
            /* Keep escaped pairs verbatim, matching the oracle's reader. */
            [out appendString:@"\\"];
            [out appendString:[NSString stringWithFormat:@"%C", [src characterAtIndex:i + 1]]];
            i += 2;
            continue;
        }
        if (c == '\n') {
            failed = YES;
            lastError = 3;
            return nil;
        }
        [out appendString:[NSString stringWithFormat:@"%C", c]];
        i++;
    }
    failed = YES;
    lastError = 3;
    return nil;
}

/* Consume the keyword at pos if present. */
- (BOOL) keyword:(NSString *)kw
{
    NSUInteger len = [src length];
    NSUInteger n = [kw length];
    NSRange r = NSMakeRange(pos, n);
    if (r.location + r.length <= len &&
        [src compare:kw options:NSLiteralSearch range:r] == NSOrderedSame) {
        pos += n;
        return YES;
    }
    return NO;
}

/* <symbol> list within (...) */
- (NSArray *) parseSymbolList
{
    NSMutableArray *syms = [NSMutableArray array];
    [self skipWS];
    if (![self keyword:@"("])
        return nil;
    for (;;) {
        [self skipWS];
        if ([self keyword:@")"])
            break;
        if (pos >= [src length] || [src characterAtIndex:pos] != '"') {
            failed = YES;
            lastError = 1;
            return nil;
        }
        NSString *s = [self parseQuoted];
        if (!s)
            return nil;
        [syms addObject:s];
        [self skipWS];
        if ([self keyword:@","])
            continue;
    }
    return syms;
}

/* (match...) tuple list inside { ... } */
- (NSArray *) parseMatchList:(NSUInteger)arity
{
    NSMutableArray *tuples = [NSMutableArray array];
    [self skipWS];
    if (![self keyword:@"{"])
        return nil;
    for (;;) {
        [self skipWS];
        if ([self keyword:@"}"])
            break;
        if (![self keyword:@"("])
            return nil;
        NSMutableArray *tuple = [NSMutableArray array];
        NSUInteger k;
        for (k = 0; k < arity; k++) {
            [self skipWS];
            if (pos >= [src length] || [src characterAtIndex:pos] != '"') {
                failed = YES;
                lastError = 1;
                return nil;
            }
            NSString *s = [self parseQuoted];
            if (!s)
                return nil;
            [tuple addObject:s];
            [self skipWS];
            if (k + 1 < arity && [self keyword:@","]) {
                [self skipWS];
            }
        }
        if (![self keyword:@")"]) {
            failed = YES;
            lastError = 4;
            return nil;
        }
        [tuples addObject:tuple];
        [self skipWS];
        if ([self keyword:@","])
            continue;
    }
    return tuples;
}

/* A whole replace/replace method body except its lead keyword. */
- (TPRule *) parseReplaceRule:(int)rtype
{
    TPRule *rule = [[[TPRule alloc] init] autorelease];
    rule->type = rtype;

    [self skipWS];
    if (pos >= [src length] || [src characterAtIndex:pos] != '"') {
        failed = YES;
        lastError = 1;
        return rule;
    }
    rule.pattern = [self parseQuoted];
    if (!rule.pattern)
        return rule;

    if (rtype != RRFind) {
        [self skipWS];
        [self keyword:@"with"];
        [self skipWS];
        if ([self keyword:@"same"]) {
            rule->isSame = YES;
            rule.replacement = @"same";
        } else if (pos < [src length] && [src characterAtIndex:pos] == '"') {
            rule.replacement = [self parseQuoted];
            if (!rule.replacement)
                return rule;
        } else if (pos < [src length]) {
            failed = YES;
            lastError = 1;
            return rule;
        } else {
            failed = YES;
            return rule;
        }
    }

    for (;;) {
        [self skipWS];
        if (pos >= [src length])
            break;
        if ([self keyword:@"where"]) {
            [self skipWS];
            NSArray *syms = [self parseSymbolList];
            if (!syms)
                return rule;
            [self skipWS];
            if (![self keyword:@"isOneOf"])
                return rule;
            NSArray *mt = [self parseMatchList:[syms count]];
            if (!mt)
                return rule;
            if (rule.whereClauses == nil)
                rule.whereClauses = [NSMutableArray array];
            [(NSMutableArray *)rule.whereClauses addObject:
                [NSDictionary dictionaryWithObjectsAndKeys:
                    syms, @"symbols", mt, @"matches", nil]];
        } else if ([self keyword:@"within"]) {
            [self skipWS];
            NSArray *syms = [self parseSymbolList];
            if (!syms || [syms count] < 1)
                return rule;
            [self skipWS];
            if (![self keyword:@"{"])
                return rule;
            NSMutableArray *subs = [NSMutableArray array];
            for (;;) {
                [self skipWS];
                if ([self keyword:@"}"])
                    break;
                if ([self keyword:@"replace"]) {
                    TPRule *sub = [self parseReplaceRule:RRReplace];
                    if (!sub.pattern)
                        return rule;
                    [subs addObject:sub];
                } else {
                    return rule;
                }
            }
            rule.withinSymbols = syms;
            rule.withinRules = [NSArray arrayWithObject:subs];
        } else if ([self keyword:@"error"]) {
            [self skipWS];
            rule.errorMsg = [self parseQuoted];
        } else if ([self keyword:@"warning"]) {
            [self skipWS];
            rule.warningMsg = [self parseQuoted];
        } else {
            break;
        }
    }
    return rule;
}

- (NSArray *) parse
{
    NSMutableArray *rules = [NSMutableArray array];
    unsigned long len = [src length];

    for (;;) {
        [self skipWS];
        if (pos >= len)
            break;
        if ([self keyword:@"find"]) {
            TPRule *r = [self parseReplaceRule:RRFind];
            if (r.pattern)
                [rules addObject:r];
            else
                failed = YES;
        } else if ([self keyword:@"replacemethod"]) {
            TPRule *r = [self parseReplaceRule:RRReplaceMethod];
            if (r.pattern)
                [rules addObject:r];
            else
                failed = YES;
        } else if ([self keyword:@"replace"]) {
            TPRule *r = [self parseReplaceRule:RRReplace];
            if (r.pattern)
                [rules addObject:r];
            else
                failed = YES;
        } else {
            failed = YES;
            lastError = 2;
        }
        if (failed)
            break;
    }
    if (failed)
        return nil;
    return rules;
}

@end

/* ------------------------------------------------------------------ */
/* The Tops object.                                                    */
/* ------------------------------------------------------------------ */

@interface Tops : NSObject {
@public
    NSMutableString *parseString;
    NSString *scriptFile;
    NSString *classFile;
    NSMutableArray *fileNames;
    BOOL dontWriteFiles;
    int verbose;
    int hideContext;
    BOOL useStdin;
}
@property (retain) NSMutableString *parseString;
@property (retain) NSString *scriptFile;
@property (retain) NSString *classFile;
@property (retain) NSMutableArray *fileNames;
@end

@implementation Tops

@synthesize parseString, scriptFile, classFile, fileNames;

- (id) init
{
    self = [super init];
    if (self) {
        parseString = [[NSMutableString alloc] initWithCapacity:0];
        fileNames = [[NSMutableArray alloc] init];
        dontWriteFiles = NO;
        verbose = 0;
        hideContext = NO;
        useStdin = NO;
    }
    return self;
}

- (void) dealloc
{
    [parseString release];
    [scriptFile release];
    [classFile release];
    [fileNames release];
    [super dealloc];
}

/* ------------------------------------------------------------------ */
/* Command-line reconstruction of the rules.                           */
/* ------------------------------------------------------------------ */

/* Rules may be chained in a single command line; the comma that separates
 * them is not part of the token it follows.  A bare word such as "same,"
 * carries no colon, so its comma belongs to the word. */
static NSString *stripRuleSeparator(NSString *s)
{
    if ([s hasSuffix:@","] && [s rangeOfString:@":"].location != NSNotFound)
        return [s substringToIndex:[s length] - 1];
    return s;
}

- (void) buildFromArgv
{
    NSArray *args = [[NSProcessInfo processInfo] arguments];
    NSUInteger i, count = [args count];

    for (i = 1; i < count; i++) {
        NSString *arg = [args objectAtIndex:i];

        if ([arg isEqualToString:@"-help"]) {
            printUsage();
            exit(0);
        } else if ([arg isEqualToString:@"-dont"]) {
            dontWriteFiles = YES;
        } else if ([arg isEqualToString:@"-semiverbose"]) {
            if (verbose < 1)
                verbose = 1;
        } else if ([arg isEqualToString:@"-verbose"]) {
            verbose = 2;
        } else if ([arg isEqualToString:@"-nocontext"]) {
            hideContext = YES;
        } else if ([arg isEqualToString:@"-nofileinfo"]) {
            verbose = 0;
        } else if ([arg isEqualToString:@"-scriptfile"] ||
                   [arg isEqualToString:@"scriptfile"]) {
            if (i + 1 < count)
                self.scriptFile = [args objectAtIndex:++i];
        } else if ([arg isEqualToString:@"-classfile"] ||
                   [arg isEqualToString:@"classfile"]) {
            if (i + 1 < count)
                self.classFile = [args objectAtIndex:++i];
        } else if ([arg isEqualToString:@"find"] ||
                   [arg isEqualToString:@"-find"] ||
                   [arg isEqualToString:@"replace"] ||
                   [arg isEqualToString:@"-replace"] ||
                   [arg isEqualToString:@"replacemethod"] ||
                   [arg isEqualToString:@"-replacemethod"]) {
            if (i + 1 < count)
                [parseString appendFormat:@"%@ \"%@\"", arg,
                    stripRuleSeparator([args objectAtIndex:++i])];
        } else if ([arg isEqualToString:@"with"] || [arg isEqualToString:@"-with"]) {
            if (i + 1 < count)
                [parseString appendFormat:@"with \"%@\"",
                    stripRuleSeparator([args objectAtIndex:++i])];
        } else if ([arg isEqualToString:@"same"]) {
            [parseString appendString:@"same"];
        } else if ([arg isEqualToString:@"where"]) {
            if (i + 1 < count) {
                NSString *next = [args objectAtIndex:++i];
                if ([next hasPrefix:@"\""])
                    [parseString appendFormat:@"where %@", next];
                else
                    [parseString appendFormat:@"where \"%@\"", next];
            }
        } else if ([arg isEqualToString:@"isOneOf"] ||
                   [arg isEqualToString:@"within"] ||
                   [arg isEqualToString:@"error"] ||
                   [arg isEqualToString:@"warning"] ||
                   [arg isEqualToString:@"message"]) {
            if (i + 1 < count)
                [parseString appendFormat:@"%@ %@", arg, [args objectAtIndex:++i]];
        } else if ([arg isEqualToString:@"}"]) {
            [parseString appendString:@"}"];
        } else if ([arg isEqualToString:@"-"]) {
            useStdin = YES;
        } else {
            [fileNames addObject:arg];
        }
    }

    if (scriptFile != nil) {
        NSString *rules = [NSString stringWithContentsOfFile:scriptFile
                                                    encoding:NSUTF8StringEncoding
                                                       error:NULL];
        if (rules != nil)
            [parseString insertString:rules atIndex:0];
    }
}

/* Print "Expected ..." + rest-of-string + the failure trailer.  Two
 * variants are known: quote failures and rule-specifier failures, the
 * latter printed with a blank line after the offending rest of line. */
- (void) reportParseError:(NSUInteger)atPos in:(NSString *)s kind:(int)kind
{
    NSUInteger len = [s length];
    NSString *rest = @"";
    NSString *head;

    if (atPos < len) {
        rest = [s substringFromIndex:atPos];
        while ([rest length] > 0 &&
               [rest characterAtIndex:[rest length] - 1] == '\n')
            rest = [rest substringToIndex:[rest length] - 1];
    }

    if (kind == 1)
        head = [NSString stringWithFormat:
            @"Expected '\"'..., character position = %lu\n%s\n\n",
            (unsigned long)(atPos + 1), [rest UTF8String]];
    else if (kind == 3)
        head = [NSString stringWithFormat:
            @"Unterminated quoted argument..., character position = %lu\n%s\n\n",
            (unsigned long)(atPos + 1), [rest UTF8String]];
    else if (kind == 4)
        head = [NSString stringWithFormat:
            @"Expected ')', character position = %lu\n%s\n\n",
            (unsigned long)(atPos + 1), [rest UTF8String]];
    else
        head = [NSString stringWithFormat:
            @"Expected rule type specifier or flag..., character position = %lu\n%s\n\n",
            (unsigned long)(atPos + 1), [rest UTF8String]];

    fputs([head UTF8String], stdout);
    fputs("An error occured parsing rules\n", stdout);
    printUsage();
}

/* ------------------------------------------------------------------ */
/* Apply one rule to a stretch of text; return the changed text and    */
/* append report strings for every substitution made.                  */
/* ------------------------------------------------------------------ */

/* replacemethod: locate Obj-C message expressions [receiver part:arg part:arg]
 * whose selector matches the pattern and rebuild them with the new
 * selector, binding the argument slots positionally.  Missing argument
 * slots for a longer replacement selector get the placeholder
 * <_tops_argN>, numbering the slots 2,4,6,... after the last real one. */
- (NSString *) applyReplaceMethodRule:(TPRule *)rule to:(NSString *)text
                             withName:(NSString *)name
                               reports:(NSMutableArray *)reports
                                  count:(int *)count
                                 changes:(int *)changes
{
    /* A pattern with no colon in it constrains nothing the oracle can match. */
    if ([rule.pattern rangeOfString:@":"].location == NSNotFound)
        return text;

    NSArray *pColon = [rule.pattern componentsSeparatedByString:@":"];
    NSMutableArray *patNames = [NSMutableArray array];
    NSUInteger pCount = [pColon count];
    for (NSUInteger k = 0; k < pCount; k++) {
        NSString *p = [pColon objectAtIndex:k];
        if ([p length] == 0)
            continue;
        /* A fragment sitting after the final colon is a suffix hint, not a
         * part of its own: "pq:rs" constrains only "pq", while "pq:rs:"
         * demands a following part too. */
        if (k == pCount - 1 && ![rule.pattern hasSuffix:@":"])
            continue;
        [patNames addObject:p];
    }

    /* The replacement selector is used up to its last colon; anything after
     * that is discarded.  With no colon at all it stands alone as a word. */
    NSString *replSel = rule.replacement;
    BOOL replTakesArgs = [replSel rangeOfString:@":"].location != NSNotFound;
    if (replTakesArgs) {
        NSRange lastColon = [replSel rangeOfString:@":"
                                         options:NSBackwardsSearch];
        replSel = [replSel substringToIndex:NSMaxRange(lastColon)];
    }
    NSMutableArray *replNames = [NSMutableArray array];
    NSUInteger rStart = 0;
    for (NSUInteger k = 0; k < [replSel length]; k++) {
        if ([replSel characterAtIndex:k] == ':') {
            [replNames addObject:[replSel substringWithRange:
                                  NSMakeRange(rStart, k - rStart)]];
            rStart = k + 1;
        }
    }
    NSUInteger replCount = [replNames count];

    NSUInteger len = [text length];
    NSMutableString *out = [NSMutableString string];
    NSUInteger i = 0;
    __block int line = 1;
    NSUInteger copyStart = 0;

    while (i < len) {
        unichar c = [text characterAtIndex:i];
        NSUInteger regionEnd = 0;
        if (skipRegionAt(text, i, &regionEnd)) {
            NSUInteger k;
            for (k = i; k < regionEnd; k++)
                if ([text characterAtIndex:k] == '\n')
                    line++;
            i = regionEnd;
            continue;
        }
        if (c != '[') {
            if (c == '\n')
                line++;
            i++;
            continue;
        }

        NSUInteger msgStart = i;
        __block NSUInteger j = i + 1;
        NSMutableArray *args = [NSMutableArray array];
        NSMutableArray *names = [NSMutableArray array];
        NSString *receiver = nil;
        BOOL ok = YES;

        void (^skipWS)(void) = ^{
            while (j < len && isspace([text characterAtIndex:j])) {
                if ([text characterAtIndex:j] == '\n')
                    line++;
                j++;
            }
        };
        /* consume the run of non-space chars up to ']' and return it. */
        NSString *(^readRun)(void) = ^{
            NSUInteger s = j;
            while (j < len && !isspace([text characterAtIndex:j]) &&
                   [text characterAtIndex:j] != ']')
                j++;
            return [text substringWithRange:NSMakeRange(s, j - s)];
        };
        /* consume chars up to ':' and return them; ':' stays in place. */
        NSString *(^readNameUpToColon)(void) = ^{
            NSUInteger s = j;
            while (j < len && !isspace([text characterAtIndex:j]) &&
                   [text characterAtIndex:j] != ':' &&
                   [text characterAtIndex:j] != ']')
                j++;
            return [text substringWithRange:NSMakeRange(s, j - s)];
        };

        skipWS();
        if (j >= len || [text characterAtIndex:j] == ']') {
            ok = NO;
        } else {
            /* first run is always the receiver; a colon disqualifies it. */
            NSString *first = readRun();
            if ([first rangeOfString:@":"].location != NSNotFound) {
                ok = NO;
            } else {
                receiver = first;
            }
        }

        if (ok) {
            for (;;) {
                skipWS();
                if (j < len && [text characterAtIndex:j] == ']')
                    break;
                if (j >= len) { ok = NO; break; }
                if ([text characterAtIndex:j] == ':') {
                    ok = NO;
                    break;
                }
                NSString *nm = readNameUpToColon();
                if (j >= len || [text characterAtIndex:j] != ':') {
                    ok = NO;
                    break;
                }
                j++;
                [names addObject:nm];
                NSUInteger as = j;
                while (j < len && !isspace([text characterAtIndex:j]) &&
                       [text characterAtIndex:j] != ']')
                    j++;
                NSString *arg = [text substringWithRange:
                    NSMakeRange(as, j - as)];
                if ([arg length] == 0) {
                    ok = NO;
                    break;
                }
                [args addObject:arg];
            }
            if (j >= len || [text characterAtIndex:j] != ']')
                ok = NO;
        }

        /* Selector comparison: the pattern's parts constrain only the start
         * of the selector, so a message with extra trailing parts matches. */
        if (ok && [names count] >= [patNames count]) {
            for (NSUInteger k = 0; k < [patNames count]; k++)
                if (![[names objectAtIndex:k] isEqualToString:
                        [patNames objectAtIndex:k]])
                    ok = NO;
        } else {
            ok = NO;
        }

        if (!ok) {
            i++;
            continue;
        }

        /* rebuild with the replacement selector. */
        NSRange msgRange = NSMakeRange(msgStart, j + 1 - msgStart);
        NSString *matchedText = [text substringWithRange:msgRange];
        NSMutableString *build = [NSMutableString string];
        [build appendString:@"["];
        if (receiver)
            [build appendString:receiver];
        NSUInteger realArgs = [args count];
        if (!replTakesArgs) {
            [build appendFormat:@" %@", replSel];
        } else {
            for (NSUInteger k = 0; k < replCount; k++) {
                NSString *nm = [replNames objectAtIndex:k];
                [build appendString:@" "];
                /* A one-character replacement part is dropped by the oracle,
                 * leaving the bare "name:" slot. */
                if ([nm length] >= 2)
                    [build appendString:nm];
                [build appendString:@":"];
                if (k < realArgs)
                    [build appendString:[args objectAtIndex:k]];
                else
                    [build appendFormat:@"<_tops_arg%lu>", (unsigned long)(2 * k)];
            }
        }
        [build appendString:@"]"];
        NSString *repl = build;

        [out appendString:[text substringWithRange:
                           NSMakeRange(copyStart, msgStart - copyStart)]];
        [out appendString:repl];
        /* -dont asks to see the change, so it reports even when quiet. */
        if (verbose >= 2 || dontWriteFiles)
            [reports addObject:[NSString stringWithFormat:@"%@:%d: '%@' -> '%@'\n",
                         name, line, matchedText, repl]];
        if (count)
            (*count)++;
        if (changes && ![repl isEqualToString:matchedText])
            (*changes)++;
        i = j + 1;
        copyStart = j + 1;
    }


    if (copyStart < len)
        [out appendString:[text substringFromIndex:copyStart]];
    return out;
}

- (NSString *) applyRule:(TPRule *)rule to:(NSString *)text
               withName:(NSString *)name
                 reports:(NSMutableArray *)reports
                   count:(int *)count
                  changes:(int *)changes
{
    if (rule->type == RRReplaceMethod)
        return [self applyReplaceMethodRule:rule to:text
                                    withName:name reports:reports
                                       count:count changes:changes];

    if (rule->isSame)
        return text;

    NSArray *ptoks = tokenizeString(rule.pattern);
    NSUInteger pt = [ptoks count];
    NSArray *stoks = tokenizeString(text);
    NSUInteger st = [stoks count];
    NSMutableString *out = [NSMutableString string];
    NSUInteger si = 0;

    if (pt == 0)
        return text;

    NSUInteger pbeg = 0, pend = pt;
    while (pbeg < pend &&
           ((TPToken *)[ptoks objectAtIndex:pbeg])->type == TPTokenWS)
        pbeg++;
    while (pend > pbeg &&
           ((TPToken *)[ptoks objectAtIndex:pend - 1])->type == TPTokenWS)
        pend--;
    if (pbeg >= pend)
        return text;

    while (si < st) {
        /* Try to match the pattern beginning at token si. */
        BOOL matched = YES;
        NSUInteger pi = pbeg;
        NSUInteger sj = si;
        NSMutableDictionary *caps = [NSMutableDictionary dictionary];
        NSRange full = NSMakeRange(NSNotFound, 0);
        int line = 0;

        while (pi < pend) {
            TPToken *p = [ptoks objectAtIndex:pi];
            if (p->type == TPTokenWS) {
                if (sj >= st ||
                    ((TPToken *)[stoks objectAtIndex:sj])->type != TPTokenWS) {
                    matched = NO;
                    break;
                }
                while (sj < st &&
                       ((TPToken *)[stoks objectAtIndex:sj])->type == TPTokenWS)
                    sj++;
                pi++;
                continue;
            }
            if (sj >= st) { matched = NO; break; }
            TPToken *s = [stoks objectAtIndex:sj];
            if (p->type == TPTokenNamed) {
                [caps setObject:s.text forKey:p.key];
            } else if (p->type != s->type || ![p.text isEqualToString:s.text]) {
                if (p->type == TPTokenWord && s->type == TPTokenWord &&
                    [p.text isEqualToString:s.text]) {
                    /* equal words */
                } else {
                    matched = NO;
                    break;
                }
            }
            if (full.location == NSNotFound) {
                full.location = s->range.location;
                line = s->line;
            }
            full.length = (s->range.location + s->range.length) -
                full.location;
            pi++;
            sj++;
        }
        if (!matched) {
            [out appendString:[[stoks objectAtIndex:si] text]];
            si++;
            continue;
        }

        BOOL whereOK = YES;
        if (rule.whereClauses != nil) {
            NSUInteger c, cn = [rule.whereClauses count];
            for (c = 0; c < cn && whereOK; c++) {
                NSDictionary *clause = [rule.whereClauses objectAtIndex:c];
                NSArray *syms = [clause objectForKey:@"symbols"];
                NSArray *tuples = [clause objectForKey:@"matches"];
                NSUInteger symCount = [syms count];
                BOOL allCaptured = YES;
                NSMutableArray *captured = [NSMutableArray arrayWithCapacity:symCount];
                NSUInteger k;
                for (k = 0; k < symCount; k++) {
                    NSString *cap = [caps objectForKey:
                        [[syms objectAtIndex:k]
                            stringByTrimmingCharactersInSet:
                                [NSCharacterSet characterSetWithCharactersInString:@"<>"]]];
                    if (cap == nil) {
                        allCaptured = NO;
                        break;
                    }
                    [captured addObject:cap];
                }
                if (!allCaptured)
                    continue;
                whereOK = NO;
                NSUInteger t, tn = [tuples count];
                for (t = 0; t < tn; t++) {
                    NSArray *tuple = [tuples objectAtIndex:t];
                    BOOL tupleOK = YES;
                    for (k = 0; k < symCount; k++) {
                        if (![[captured objectAtIndex:k] isEqualToString:
                                [tuple objectAtIndex:k]]) {
                            tupleOK = NO;
                            break;
                        }
                    }
                    if (tupleOK) {
                        whereOK = YES;
                        break;
                    }
                }
            }
        }
        if (!whereOK) {
            [out appendString:[[stoks objectAtIndex:si] text]];
            si++;
            continue;
        }

        NSRange matchRange = full;
        NSString *matchedText = [text substringWithRange:matchRange];

        if (rule.withinSymbols != nil) {
            /* Run sub rules inside each named token of the match; the
             * outer token keeps the modified text. */
            NSArray *subs = [rule.withinRules count] ?
                [rule.withinRules objectAtIndex:0] : nil;
            if (subs != nil) {
                NSDictionary *oldCaps = [[caps copy] autorelease];
                for (NSString *sym in rule.withinSymbols) {
                    NSString *symName = [sym stringByTrimmingCharactersInSet:
                        [NSCharacterSet characterSetWithCharactersInString:@"<>"]];
                    NSString *inner = [caps objectForKey:symName];
                    if (!inner)
                        continue;
                    NSString *changed = inner;
                    for (TPRule *sub in subs) {
                        int subCount = 0;
                        int subChanges = 0;
                        NSMutableArray *subRep = [NSMutableArray array];
                        changed = [self applyRule:sub to:changed
                                         withName:name reports:subRep
                                            count:&subCount
                                           changes:&subChanges];
                        for (NSString *r in subRep) {
                            [reports addObject:r];
                            if (count)
                                (*count)++;
                            if (changes)
                                (*changes)++;
                        }
                        (void)oldCaps;
                    }
                    if (changed)
                        [caps setObject:changed forKey:symName];
                }
            }
        }

        /* Build the replacement text. */
        if (rule->type == RRFind) {
            /* find reports the whole matching line as its context. */
            NSRange lineRange;
            NSRange b = [text rangeOfString:@"\n"
                                    options:NSBackwardsSearch
                                      range:NSMakeRange(0, full.location)];
            NSRange f = [text rangeOfString:@"\n"
                                    options:0
                                      range:NSMakeRange(full.location,
                                          [text length] - full.location)];
            lineRange.location = (b.location == NSNotFound) ? 0 : b.location + 1;
            lineRange.length = ((f.location == NSNotFound) ? [text length]
                                                           : f.location)
                - lineRange.location;
            if (verbose >= 2)
                [reports addObject:[NSString stringWithFormat:@"%@:%d:%@\n",
                             name, line, [text substringWithRange:lineRange]]];
            if (count)
                (*count)++;
            [out appendString:matchedText];
            si = sj;
            continue;
        }

        NSString *repl;
        if (rule->isSame) {
            repl = rule.replacement;
        } else {
            NSArray *rtoks = tokenizeString(rule.replacement);
            NSMutableString *build = [NSMutableString string];
            for (TPToken *rt in rtoks) {
                if (rt->type == TPTokenWS) {
                    [build appendString:rt.text];
                } else if (rt->type == TPTokenNamed) {
                    NSString *c = [caps objectForKey:rt.key];
                    [build appendString:c ? c : @""];
                } else {
                    [build appendString:rt.text];
                }
            }
            repl = build;
        }

        if (verbose >= 2)
            [reports addObject:[NSString stringWithFormat:@"%@:%d: '%@' -> '%@'\n",
                         name, line, matchedText, repl]];
        if (count)
            (*count)++;
        if (changes)
            (*changes)++;
        [out appendString:repl];
        si = sj;
    }
    return out;
}

- (void) printReports:(NSArray *)reports
{
    NSUInteger i, n = [reports count];
    for (i = 0; i < n; i++)
        fputs([[reports objectAtIndex:i] UTF8String], stdout);
}

/* ------------------------------------------------------------------ */
/* File / stdin processing.                                            */
/* ------------------------------------------------------------------ */

/* Apply every rule without producing any output, so the caller can
 * decide when the reports are shown.  Returns text, reports, count and
 * changes. */
- (NSDictionary *) evaluateFile:(NSString *)name data:(NSString *)data
                          rules:(NSArray *)rules
{
    NSMutableArray *reports = [NSMutableArray array];
    NSMutableArray *groups = [NSMutableArray array];
    int count = 0;
    int changes = 0;
    NSString *current = data;

    for (TPRule *rule in rules) {
        NSUInteger before = [reports count];
        current = [self applyRule:rule to:current
                         withName:name reports:reports
                            count:&count changes:&changes];
        [groups addObject:[reports subarrayWithRange:
                           NSMakeRange(before, [reports count] - before)]];
    }

    return [NSDictionary dictionaryWithObjectsAndKeys:
                current, @"text",
                reports, @"reports",
                groups, @"groups",
                [NSNumber numberWithInt:count], @"count",
                [NSNumber numberWithInt:changes], @"changes", nil];
}

/* Process one file: apply every rule, print the per-file diagnostics.
 * The replacemethod form is intercepted by the caller, which owns its bar
 * layout, so this handles the find/replace progress bar only. */
- (unsigned long) processFile:(NSString *)name data:(NSString *)data
                        rules:(NSArray *)rules
                   hasReplaceMethod:(BOOL)hasRM
                   single:(BOOL)single grandTotal:(unsigned long)grandTotal
{
    NSMutableArray *reports = [NSMutableArray array];
    int count = 0;
    int changes = 0;
    NSString *current = data;
    BOOL ui = (verbose >= 1);

    for (TPRule *rule in rules)
        current = [self applyRule:rule to:current
                         withName:name reports:reports
                            count:&count changes:&changes];


    if (verbose >= 2)
        [self printReports:reports];

    if (ui && count > 0)
        fprintf(stdout, "%s: %d occurrences found\n", [name UTF8String], count);

    if (ui && !dontWriteFiles && changes > 0) {
        fprintf(stdout, "%s written\n", [name UTF8String]);
        if (![name isEqualToString:@"StandardInput"]) {
            NSError *err = nil;
            if (![current writeToFile:name atomically:YES
                  encoding:NSUTF8StringEncoding error:&err]) {
                fprintf(stderr, "***Could not write %s\n", [name UTF8String]);
            }
        }
    }
    return [current length];
}

- (void) applyRules
{
    /* A class hierarchy the parser cannot make sense of stops the run
     * before any file is touched. */
    if (classFile != nil) {
        fprintf(stderr, "***Bad class hierarchy\n");
        printUsage();
        return;
    }

    if ([parseString length] == 0) {
        fprintf(stderr, "***No rules specified\n");
        return;
    }

    TopsParser *parser = [[[TopsParser alloc] initWithString:parseString] autorelease];
    NSArray *rules = [parser parse];

    rmIsTTY = isatty(fileno(stdout)) ? YES : NO;

    if (rules == nil || [rules count] == 0) {
        if (parser->failed && parser->lastError != 0)
            [self reportParseError:parser->pos in:parseString
                              kind:parser->lastError];
        else {
            fputs("An error occured parsing rules\n", stdout);
            printUsage();
        }
        return;
    }

    if ([fileNames count] == 0) {
        NSFileHandle *fh = [NSFileHandle fileHandleWithStandardInput];
        NSData *d = [fh readDataToEndOfFile];
        NSString *text = [[[NSString alloc] initWithData:d
                    encoding:NSUTF8StringEncoding] autorelease];
        if (text == nil)
            text = @"";

        BOOL hasRM = NO;
        for (TPRule *rule in rules)
            if (rule->type == RRReplaceMethod)
                hasRM = YES;

        /* replacemethod consumes standard input as a whole: the reports
         * come first, then the result is echoed inside a bar whose lower
         * row also carries the occurrence count. */
        if (hasRM) {
            NSDictionary *res = [self evaluateFile:@"StandardInput" data:text
                                            rules:rules];
            NSArray *groups = [res objectForKey:@"groups"];
            NSString *out = [res objectForKey:@"text"];
            int c = [[res objectForKey:@"count"] intValue];
            if (dontWriteFiles)
                out = text;

            if (rmIsTTY) {
                /* Standard input cannot be rewritten, so a terminal run just
                 * echoes the text and shows a completed bar. */
                fputs([text UTF8String], stdout);
                if (verbose >= 1) {
                    rmRuleCount = (int)[groups count];
                    rmDrawTopRow();
                    rmOpenLowerRow();
                    closeReplaceMethodBar();
                }
                return;
            }

            if (verbose >= 2 || dontWriteFiles) {
                if (verbose >= 1) {
                    if ([groups count] > 0)
                        [self printReports:[groups objectAtIndex:0]];
                } else {
                    [self printReports:[res objectForKey:@"reports"]];
                }
            }

            if (verbose >= 1) {
                rmRuleCount = (int)[groups count];
                openReplaceMethodBar();
                NSUInteger k, gn = [groups count];
                for (k = 1; k < gn; k++) {
                    NSArray *g = [groups objectAtIndex:k];
                    if ([g count] == 0)
                        continue;
                    fillReplaceMethodBar(rmCellForRule((int)k));
                    [self printReports:g];
                }
                fillReplaceMethodBar(RM_BAR_CELLS - rmTailCells());
                fputs([out UTF8String], stdout);
                if (c > 0)
                    fprintf(stdout, "%d occurrences\n", c);
                closeReplaceMethodBar();
            } else {
                fputs([out UTF8String], stdout);
            }
            return;
        }

        /* Standard input is processed line by line.  Each line is
         * echoed as read; matching lines also print their context. */
        NSArray *lines = [text componentsSeparatedByString:@"\n"];
        int count = 0;
        BOOL termByNewline = [text hasSuffix:@"\n"];
        NSUInteger nLines = [lines count];
        NSUInteger li;

        if (termByNewline && nLines > 0 &&
            [[lines lastObject] length] == 0)
            nLines--;

        for (li = 0; li < nLines; li++) {
            NSString *line = [lines objectAtIndex:li];
            NSMutableArray *lineReports = [NSMutableArray array];
            NSMutableString *changed = [NSMutableString stringWithString:line];
            for (TPRule *rule in rules) {
                int c = 0;
                int ch = 0;
                NSMutableArray *rep = [NSMutableArray array];
                NSString *next = [self applyRule:rule to:changed
                                        withName:@"StandardInput"
                                          reports:rep count:&c changes:&ch];
                for (NSString *r in rep)
                    [lineReports addObject:r];
                count += c;
                changed = [NSMutableString stringWithString:next];
            }
            if (verbose >= 1) {
                if ([lineReports count] && !hideContext)
                    [self printReports:lineReports];
                fputs([changed UTF8String], stdout);
                if (li < nLines - 1 || termByNewline)
                    fputc('\n', stdout);
            }
        }

        if (verbose >= 2 && count > 0)
            fprintf(stdout, "%d occurrences\n", count);
        if (verbose >= 1) {
            updateStatusBar(0, [lines count]);
            updateStatusBar([lines count], [lines count]);
        }
        return;
    }

    NSFileManager *fm = [NSFileManager defaultManager];

    BOOL hasRM = NO;
    for (TPRule *rule in rules)
        if (rule->type == RRReplaceMethod)
            hasRM = YES;

    /* Progress bar totals cover every existing file in the batch. */
    unsigned long grandTotal = 0;
    for (NSString *name in fileNames)
        if ([fm fileExistsAtPath:name]) {
            NSDictionary *attrs = [fm attributesOfItemAtPath:name error:NULL];
            if (attrs)
                grandTotal += [[attrs objectForKey:NSFileSize]
                                  unsignedLongValue];
        }

    NSUInteger index = 0, total = [fileNames count];

    /* replacemethod keeps its console diagnostics inside the bars.  Each
     * file is evaluated first, then its banner and reports are printed,
     * then the previous bar is closed and a new one opened; the last bar
     * is closed after the loop.  Quiet runs still rewrite the files. */
    if (hasRM) {
        BOOL ui = (verbose >= 1);
        BOOL barOpen = NO;
        NSArray *groups = nil;
        for (index = 0; index < total; index++) {
            NSString *name = [fileNames objectAtIndex:index];
            if (![fm fileExistsAtPath:name]) {
                fprintf(stdout, "File %s does not exist\n\n", [name UTF8String]);
                printUsage();
                return;
            }
            NSString *text = [NSString stringWithContentsOfFile:name
                                encoding:NSUTF8StringEncoding error:NULL];
            if (text == nil) {
                fprintf(stderr, "***Could not read %s\n", [name UTF8String]);
                continue;
            }
            NSDictionary *res = [self evaluateFile:name data:text rules:rules];
            groups = [res objectForKey:@"groups"];

            if (ui) {
                NSUInteger remaining = total - index - 1;
                if (remaining == 0)
                    fprintf(stdout, "Processing %s (last file)\n", [name UTF8String]);
                else
                    fprintf(stdout, "Processing %s (%lu file%s left)\n",
                            [name UTF8String], (unsigned long)remaining,
                            remaining == 1 ? "" : "s");
            }

            BOOL showReports = (verbose >= 2 || dontWriteFiles);
            if (showReports && verbose < 1)
                [self printReports:[res objectForKey:@"reports"]];

            if (ui) {
                rmRuleCount = (int)[groups count];
                NSUInteger k, gn = [groups count];

                if (rmIsTTY) {
                    /* On a terminal the first report lands between the two
                     * rows and the bar is completed before the summary. */
                    rmDrawTopRow();
                    if (showReports && gn > 0)
                        [self printReports:[groups objectAtIndex:0]];
                    rmOpenLowerRow();
                    for (k = 1; k < gn; k++) {
                        NSArray *g = [groups objectAtIndex:k];
                        if ([g count] == 0)
                            continue;
                        fillReplaceMethodBar(rmCellForRule((int)k));
                        [self printReports:g];
                    }
                    closeReplaceMethodBar();
                } else {
                    if (showReports && gn > 0)
                        [self printReports:[groups objectAtIndex:0]];
                    if (barOpen)
                        closeReplaceMethodBar();
                    openReplaceMethodBar();
                    barOpen = YES;

                    /* Every rule past the first reports from inside the bar. */
                    for (k = 1; k < gn; k++) {
                        NSArray *g = [groups objectAtIndex:k];
                        if ([g count] == 0)
                            continue;
                        fillReplaceMethodBar(rmCellForRule((int)k));
                        [self printReports:g];
                    }
                    fillReplaceMethodBar(RM_BAR_CELLS - rmTailCells());
                }

                int count = [[res objectForKey:@"count"] intValue];
                if (count > 0)
                    fprintf(stdout, "%s: %d occurrences found\n",
                            [name UTF8String], count);
            }

            if (!dontWriteFiles && [[res objectForKey:@"changes"] intValue] > 0) {
                if (ui)
                    fprintf(stdout, "%s written\n", [name UTF8String]);
                if (![name isEqualToString:@"StandardInput"]) {
                    NSError *err = nil;
                    if (![[res objectForKey:@"text"] writeToFile:name atomically:YES
                            encoding:NSUTF8StringEncoding error:&err])
                        fprintf(stderr, "***Could not write %s\n",
                                [name UTF8String]);
                }
            }
        }
        if (barOpen)
            closeReplaceMethodBar();
        return;
    }

    BOOL barDrawn = NO;
    unsigned long readCount = 0;
    for (NSString *name in fileNames) {
        if (![fm fileExistsAtPath:name]) {
            fprintf(stdout, "File %s does not exist\n\n", [name UTF8String]);
            printUsage();
            return;
        }
        if (verbose >= 1) {
            NSUInteger remaining = total - index - 1;
            if (remaining == 0)
                fprintf(stdout, "Processing %s (last file)\n", [name UTF8String]);
            else
                fprintf(stdout, "Processing %s (%lu file%s left)\n",
                        [name UTF8String], (unsigned long)remaining,
                        remaining == 1 ? "" : "s");
        }
        NSString *text = [NSString stringWithContentsOfFile:name
                            encoding:NSUTF8StringEncoding error:NULL];
        if (text == nil) {
            fprintf(stderr, "***Could not read %s\n", [name UTF8String]);
            index++;
            continue;
        }
        readCount++;
        [self processFile:name data:text rules:rules
                      hasReplaceMethod:hasRM
                      single:(total == 1) grandTotal:grandTotal];
        if (hasRM && total == 1)
            barDrawn = YES;
        index++;
    }

    /* Bars are drawn at the end of a batch: one complete two-row bar
     * per file that was successfully read (unless the replacemethod
     * layout already drew its own mid-run bar). */
    if (!barDrawn && verbose >= 1) {
        unsigned long i;
        for (i = 0; i < readCount; i++)
            updateStatusBar(0, 0);
    }
}

@end

/* ------------------------------------------------------------------ */

int main(int argc, char **argv)
{
    NSAutoreleasePool *pool = [[NSAutoreleasePool alloc] init];
    Tops *tops = [[Tops alloc] init];

    (void)argc;
    (void)argv;

    [tops buildFromArgv];

    if ([[tops parseString] length] == 0 && [tops scriptFile] == nil) {
        printUsage();
        [tops release];
        [pool drain];
        return 0;
    }

    [tops applyRules];

    [tops release];
    [pool drain];
    return 0;
}