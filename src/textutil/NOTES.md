# textutil reverse-engineering notes

Observations from `/usr/bin/textutil` on Darwin 25 (arm64e). All are black-box
probes; nothing was disassembled or read out of the binary. The companion
harness is `tests/textutil-parity.sh`, which runs both tools over the same
fixtures and compares exit status, both streams, and every byte produced.

## Scope

This port reads plain text and writes `txt`, `rtf`, `rtfd` and `html`. It
recognises the other five format names so that the option surface matches, but
does not write them, and it has no readers but plain text. The divergences that
follow from that are listed together at the end.

## -info counts UTF-16 code units, and previews the same 30 of them

`Length` is not a character count in the sense the word suggests, and the
preference is the tell. Everything downstream of the decode works in UTF-16
units, because that is what the reference tool hands to its own converters:

| input | Size | Length | why |
| --- | --- | --- | --- |
| `caf\xc3\xa9` | 5 bytes | 5 | e-acute is one UTF-16 unit |
| `\xe4\xb8\xad` | 3 bytes | 1 | CJK is one UTF-16 unit |
| `\xf0\x9f\x98\x80` | 4 bytes | 2 | above the BMP, so a surrogate pair |

The preview cut is in the same units. Thirty characters is thirty *units*, so
a document of 30 CJK characters is 90 bytes and previews in full, and a
document of 30 emoji is 120 bytes and previews as 15 emoji. `units29`,
`units30` and `units33` in the harness sit either side of the cut, and
`units34` puts a supplementary character at positions 1 and 30.

## The preview cut has a bug at the surrogate boundary, and it is reproduced

A supplementary character occupies two UTF-16 units. The reference tool's cut
tests only the first unit, so a character whose *second* unit is the 30th is
counted as fitting, and the preview ends halfway through it. It then formats
the truncated string through a path that cannot represent a lone surrogate,
and prints the literal four characters `(null)`:

    $ printf '\xf0\x9f\x98\x80' > x; for i in $(seq 28); do printf y >> x; done
    $ printf '\xf0\x9f\x98\x80' >> x
    $ /usr/bin/textutil -info x | tail -1
      Contents:  (null)...

Note that the 29 characters *before* it are discarded along with the broken
pair, so the whole preview is lost, not just its tail. `units34` in the harness
is exactly this case. Reproducing it is deliberate: it is what the tool does,
and a user diffing this port against the system tool should not see a
difference. `PREVIEW_UNITS` in `info.c` is where the boundary is decided, and
the check is written as a first-unit test so that the bug stays visible rather
than being quietly corrected later.

A document whose **first line is empty** has no preview at all, and the
`Contents:` field is not printed rather than printed empty. A newline, a
carriage return, or a line or paragraph separator at the very start is enough,
because all four end the first line, and a leading space is not:

    $ printf '\nplain text\n' > x
    $ /usr/bin/textutil -info x
    File:  x
      Type:  plain text
      Size:  12 bytes
      Length:  10 characters

This was a bug in this port, found by the empty-document cases in the harness
while detection was being worked on, and it is fixed rather than documented.

## A byte order mark is consumed, never counted, and never written back

`tu_decode_plain()` honours a leading mark and drops it, so the mark reaches
neither `Size` nor `Length` nor the preview:

| mark | bytes | Size | Length |
| --- | --- | --- | --- |
| `EF BB BF` | 3 | 6 | 6 |
| `FF FE` | 2 | 7 | 7 |
| `FE FF` | 2 | 7 | 7 |
| `FF FE 00 00` | 4 | 7 | 7 |

With no mark the text is read as UTF-8. Note that the UTF-32 marks are checked
before the UTF-16 ones, since `FF FE 00 00` also starts with `FF FE`; getting
that order wrong reads a UTF-32 file as UTF-16 and yields Chinese punctuation
rather than text. A lone surrogate in UTF-16 input is dropped, since it has no
UTF-8 form, and so is a value above U+10FFFF.

## -encoding is applied to txt and html, and to nothing else

The output encoding is a real option, not a decoration. Measured on a text file
containing e-acute and a CJK character:

| argument | bytes out |
| --- | --- |
| `utf8`, `UTF-8`, `utf-8`, `4` | unchanged |
| `utf-16` | `FF FE` then little endian |
| `utf-16le` | little endian, no mark |
| `utf-16be` | big endian, no mark |
| `utf-32` | `FF FE 00 00` then little endian |
| `utf-32le`, `utf-32be` | as named, no mark |

Names are matched without regard to case, and `utf16` and `utf32` are accepted
alongside the hyphenated spellings. A number is an `NSStringEncoding`, and is
read leniently: a leading run of digits is a number even with trailing rubbish,
so `4x` means `4`.

The two wide encodings and the byte-order-named ones differ in whether they
carry a mark, and both are checked over input holding a two byte and a four
byte character, so the surrogate pair that a supplementary character becomes in
UTF-16 is compared rather than assumed. RTF and RTFD output is 7-bit by
construction and the reference tool leaves it in ASCII whatever was asked for,
and `-info` writes nothing, so `-encoding` is inert for both.

The HTML writer's `charset` attribute names the encoding that was **used**, not
the spelling that was asked for, so `utf8` and the number `4` both produce
`charset=utf-8` while `utf-16le` produces `charset=utf-16le`. That is the one
place the encoding is visible in the bytes of an otherwise unchanged document,
and it is why `tu_encoding_name()` exists separately from the parser.

## -format overrides what the contents say, and is refused before -help

`-format` names the reader to use instead of sniffing. It is validated as the
option is read, ahead of any other work and ahead of `-help`:

    $ /usr/bin/textutil -format wibble -info file.txt
    Invalid input format.
    $ /usr/bin/textutil -format nope -help
    Invalid input format.            # and no usage

`textutil -encoding nope -help` behaves the same way, so both of these are
parse-time errors rather than work-time ones. Nine names are accepted, matched
without regard to case and over the whole string, so `rtfd ` with a trailing
space and `public.rtf` are both invalid. No UTI spellings are accepted.

Forcing plain text is the useful case and is implemented: `-format txt` on a
file that would otherwise be read as RTF reports it as plain text, with every
character counted. The other eight name readers this port does not have, and are
refused in the words the reference tool's own reader would use, which differ
between two families:

| forced | reference wording | reader |
| --- | --- | --- |
| `rtf`, `rtfd` | `The file "x" couldn't be opened.` | rich text |
| `doc`, `docx`, `odt`, `wordml` | `The file isn't in the correct format.` | office package |
| `html`, `webarchive` | *succeeds* | absent here |

A forced read failure is still not a failure of the command, so the status
stays 0, which is the same rule that governs an ordinary read failure under
`-info` and `-convert`. Only `-cat` turns one into status 1.

## A destination is checked before anything is written, in four different ways

`-output` names a path that may be unusable in ways that each have their own
wording, and the reference tool checks the *destination* before opening
anything, so none of these leave a partial file:

| situation | message |
| --- | --- |
| parent folder absent | `The folder doesn't exist.` |
| parent folder not writable | `You don't have permission.` |
| destination is a directory | `The file "base" couldn't be saved in the folder "folder".` |
| open or write failed | `Error writing PATH.` with nothing after it |

The third quotes two names: the destination's own name without its extension,
and the name of the folder holding it. That second name is why the harness
runs both tools in a subdirectory **of the same name** — otherwise a
diagnostic naming the working directory differs for reasons that have nothing
to do with the tool.

A bundle is a folder, so the rules are the reference tool's rather than this
port's convenience: an existing directory is used as it stands, a missing
parent is created, and a plain file in the way is **replaced** by the bundle.
A file destination is replaced rather than written in place, which is why the
file writers write directly where the reference tool writes to a temporary file
and renames. The two observable consequences are that a symlink to a directory
in the way is replaced by a regular file rather than followed, and that an
existing read-only regular file can be replaced where this port would fail to
open it. Both are divergences; neither is exercised by the harness.

`/dev/stdout` is exempt, because it is not a destination to be checked.

## Read failures have three wordings too

| situation | message |
| --- | --- |
| no such path | `The file doesn't exist.` |
| no permission | `You don't have permission.` |
| a directory, or unopenable | `The file "x" couldn't be opened.` |

A directory is rejected before the read rather than at it, because `fopen()`
succeeds on a directory and only the read then fails — which would otherwise
report an allocation failure. The quoted name is the last path component of the
path as given, and a symlink is followed to what it points at.

## -cat and -info have different exit statuses, and both are 0 on a read failure

This is the one place the reference tool's status handling looks wrong, and it
is reproduced:

| command | missing input |
| --- | --- |
| `-info` | 0 |
| `-convert` | 0 |
| `-cat` | 1 |

`-cat` is also the only command that writes nothing at all when an input is
missing: it reports the file, exits 1, and leaves no output behind even if
earlier inputs were fine.

## -stdin replaces the file list, and the conflict is diagnosed first

`-stdin` reads the redirected bytes and decodes them exactly as a file's would
be, so a byte order mark on the stream is honoured. It reports `File:  stdin`
and omits `Size`, because there is no file behind it to measure. `-stdin`
together with any file is `Input files and stdin both specified.`, and that is
diagnosed *before* the "no command" fallback, so `textutil -stdin somefile`
reports the conflict rather than the usage text.

## RTF: the font table is a fixed table, and the size is half-points

`-fontsize` is in points and the RTF control word is in half-points, so
`-fontsize 12` writes `\fs24`. The default is Helvetica-Light at 12pt, which is
`\fswiss\fcharset0 Helvetica-Light` and `\fs24`. The mapped families, with
their `\f<N>` numbers, are:

| -font | family | number |
| --- | --- | --- |
| default | Helvetica-Light | 0 |
| Courier | Courier | 1 |
| Arial | Helvetica | 2 |
| Times New Roman | Times | 3 |
| Monaco | Courier | 4 |

An arbitrary name cannot be mapped without a font database, so `-font` with a
name outside this table is a documented divergence rather than an error.

## HTML: the class numbers depend on first use, not on the kinds in play

The writer emits a fixed HTML 4.01 Strict envelope with an empty `<title>` (or
the one `-title` sets) and a Cocoa HTML Writer generator tag, then one paragraph
per line. Two paragraph kinds exist — a line with text, and a line with none —
and each is given a `p<N>` class, but **the numbers are handed out in the order
the paragraphs first introduce them**. So a document of nothing but blank lines
calls that first kind `p1`, not `p2`, and a document whose first line is blank
has its blank-line stylesheet entry appear before its text one. Getting this
from the kinds rather than the order produces a byte-identical document for
most inputs and a wrong one exactly when the first line is blank.

A line with nothing on it is emitted as `<br>`, or as no-break spaces when it
holds only blanks, and its class gets `min-height: 14.0px` so it still occupies
a line. Only the classes actually used appear in the stylesheet, which is
therefore assembled after the body is known. Within a line, leading whitespace
becomes no-break spaces inside an Apple-converted-space span, a tab becomes an
Apple-tab-span — and that stylesheet rule is emitted only if a tab was actually
seen — and `&`, `<` and `>` are escaped. Everything else, including text
outside ASCII, is written through as UTF-8.

`-excludedelements` selects a second HTML serialisation, an XHTML 1.0
Transitional one that wraps text in `<font>` elements. Its value is parsed and
validated; the serializer is not implemented. See below.

## A file with no format in its name is read as what its bytes say, and a guess that fails is plain text

A name picks the reader outright when it is one of `.txt`, `.rtf`, `.html`,
`.htm` and `.webarchive`, and those five are the only ones believed. A `.doc`,
`.docx`, `.odt`, `.wordml` or `.rtfd` **file**, a `.foo`, and a name with no dot
at all are all read by their bytes instead, which is why a `.doc` holding RTF
comes back as RTF text. A `.rtfd` **directory** is a bundle because of what it
is called; what it holds decides whether it can be read, not what it is.

A `.webarchive` is believed as a *name* but there is no reader for it here, so
its bytes are read as text, which is what the reference tool does to a file
with that name that is not an archive: the `Type` line is the one the name
gives and the `Length` is the count of the text. A real archive is a binary
property list, so what it holds is a divergence; see the list below.

The two byte gates are not alike, and neither is satisfied by the shortest
thing that could match it. Each wants bytes *past* the spelling, and they want
different numbers of them:

| | matches | bytes wanted | case | leading bytes |
| --- | --- | --- | --- | --- |
| RTF | `{\rtf` | 6 | sensitive | only `\n` and `\r` |
| HTML | `<html` | 7 | not | any whitespace |
| HTML | `<!doctype html` | 15 | not | any whitespace |

So `{\rtf` on its own is plain text and `{\rtf}` is RTF, and a six byte document
beginning `<html` is text whatever its sixth byte is — `5`, a space, or the
bracket that closes the tag — while every seven byte one is HTML. A version digit
is not wanted, which is what `{\rtf}` and `{\rtfa}` show, and `{\RTF}` is text
whatever follows it. A doctype is not enough on its own either: `<!doctype x>` and
`<!doctype html` are text and `<!doctype html>` is HTML.

Passing the gate is not the last word. The reference tool then **reads the file
with the reader the gate chose, and falls back to plain text when that read
fails.** That is why `{\rtf1 an unclosed group` is plain text while
`{\rtf1 \bogus x}` is RTF: both start the same way and both pass the gate, and
one is a file that falls apart and the other is not. The whole file is read to
decide, not a prefix of it, so a group that closes fifty kilobytes after the
mark is still RTF.

A name that is believed is never given up on, so a file called `notes.rtf`
holding something the RTF reader will not take is an error rather than plain
text. That is the whole difference between the two paths, and it is
`tu_fmt_read_falls_back()`. The belief is about the *reader*, not about the
contents, which is why a `.webarchive` that is not an archive is still read as
plain text rather than refused: there is no reader to give up on.

A read that fails leaves its output empty rather than half-filled, the way
`tu_meta_free()` leaves a `tu_meta_t` it never filled in empty, because a
caller that falls back to another reader goes on to use it.

## RTF: the document ends at its outermost group

An RTF file is read only as far as the brace that closes its outermost group.
Whatever follows is not part of the document and is not read, and no complaint
is made about it, so all three of these report the same one character:

    $ printf '{\\rtf1 a}garbage' > x.rtf
    $ /usr/bin/textutil -info x.rtf | tail -2
      Length:  1 character
      Contents:  a
    $ printf '{\\rtf1 a}   ' > x.rtf     # the same
    $ printf '{\\rtf1 a}{\\rtf1 b}' > x.rtf   # and this
    $ /usr/bin/textutil -info x.rtf | tail -2
      Length:  1 character
      Contents:  a

Three escapes are not control words but do stand for a character, and each is
taken as the character rather than as nothing: `\\` is a backslash, `\{` is an
open brace and `\}` is a close brace. `\~` is a non-breaking space and `\_` a
non-breaking hyphen. A backslash in front of anything else is a control symbol
that names no character, and contributes nothing to the text.

## HTML: what the reader builds, and what the reference tool builds

`-info` is the only command that reads HTML back, and what it reports is the
text an attributed-string reader lays out rather than the markup. The
paragraph structure is therefore visible in the character count:

| input | Length | text |
| --- | --- | --- |
| `a` | 1 | `a` |
| `<p>a</p>` | 2 | `a` + separator |
| `<p>a</p><p>b</p>` | 4 | `a` + separator + `b` + separator |
| `<p></p>` | 1 | separator |
| `<p></p><p></p><p></p>c` | 4 | 3 separators + `c` |
| `a<p></p>b` | 4 | `a` + 2 separators + `b` |
| `<div></div>x` | 2 | separator + `x` |

A line separator (`U+2028`) ends a paragraph, a `<br>` breaks the line inside
one, and an `<hr>` breaks it twice. A `<img>` is an attachment character
(`U+FFFC`) in the text, inside `<pre>` as well as outside it. A `<pre>` keeps
its runs of spaces, and the newline in its source is a line separator like any
other; a `<q>` is a curly quote pair, and a counted list writes its number as
digits after two tabs. An entity outside the basic plane is two characters to
`Length`, as it is everywhere else.

Of the eighty-three HTML bodies in the battery at `/tmp/tuprobe/len.sh`,
seventy-eight have the reference tool's exact character count. The five that do
not are all empty or nested block structure, where the reference tool's own
count does not follow a single rule that these cases share: a paragraph
directly inside a `<div>` (six against four), an empty `<div>` pair (six
against four), a `<p>` holding a `<div>` (six against seven), a `<hr>` straight
after another (six against five), and a list item holding a list (eleven against
ten). Each is one or two characters, each is a case where the two disagree
about whether an empty block ends a paragraph, and no single rule fixes all
five, so they are left as they are rather than traded for a rule that breaks
more cases than it fixes.

## The preview stops at a separator, and does not show what is behind it

A line separator ends the preview, exactly as a newline does, and what is
behind it is not shown. When something visible *does* follow the separator, the
cut is marked with an ellipsis, so a paragraph ends the preview with `...`:

    $ printf '<p>a</p>' > x.html;            /usr/bin/textutil -info x.html | tail -1
      Contents:  a
    $ printf '<p>a</p><p>b</p>' > x.html;    /usr/bin/textutil -info x.html | tail -1
      Contents:  a...

A separator at the very end of the document is dropped silently, which is why
the first of those previews as `a` and not as `a...`. The 30-unit cut still
applies to what is shown in front of the separator, so a long paragraph is cut
at 30 units and then marked. A `U+2029` paragraph separator is treated the same
way, since RTF writes paragraphs with it.

## WordML: a fixed envelope, and paragraphs that are separated three ways

`wordml` is a single line. Two processing instructions, the opening
`w:wordDocument` tag with its fourteen namespace declarations, then the body on
the *same* line as the tag, and no newline at the end of the file. Nothing in
the envelope varies, and the document's own font never reaches the `w:fonts`
table, which names Times New Roman in all four slots every time.

The shape of the body is the interesting part. A document is a run of
paragraphs, and the marks that end one are a carriage return, a line feed, a
carriage return and a line feed *in that order* as one mark, and `U+2029`; a
line feed followed by a carriage return is two marks and writes an empty
paragraph between them. A mark at the very end does not begin another paragraph,
so a document that ends in one does not gain an empty one, while a document of
two newlines is two empty paragraphs. Inside a paragraph a tab is `<w:tab/>` and
`U+2028` is `<w:br/>`, and the text either side of either is wrapped in a `w:t`
**whether or not it is empty**, so a paragraph that is nothing but a tab is an
empty `w:t`, the tab, and another empty `w:t`. Only a document with no text at
all is different: it is a bare `<w:p></w:p>`, with no `w:pPr` and no run.

The escaping is the three characters with a meaning of their own and nothing
else, so `>` is written `&gt;` and a quote is written as itself. The metadata
group is `o:`-prefixed and in a fixed order, `-keywords` being the one option
that is not usable: the reference tool mishandles every form of it. The run
properties are `w:rFonts`, `wx:font`, `w:sz`, `w:sz-cs`, then bold and italic,
with the size in half-points and the family the one `-font` resolves to. Bold
before italic is the opposite of the order RTF writes them in.

`-convert wordml in.txt` with no `-output` writes `in.xml`: wordml is the one
format whose name is not the extension an output file of it is given.

The one thing this writer cannot reproduce is anything the *reader* recorded
about the document rather than its text; see the divergences below.

## The embedding controls are a level the text is under, not a character

`U+202A` to `U+202E` say which way the text they cover reads rather than being
shown to the reader, so all three writers take them out of the text and keep the
stack they build instead. The stack starts empty at each paragraph and a
paragraph that is nothing but controls is not written at all, which is the same
answer a paragraph that is nothing but a mark gets.

The stack is not compared by how deep it is but by **what is in it**, and that is
the whole of the rule: text is split into runs wherever the stack changes, and
text either side of a control that leaves the stack and comes back is one run
and not two. A control that opens a level nothing is ever written under is
opened and closed again with no text between, and so contributes nothing.

Each writer says a run boundary in its own way.

- **WordML** drops the control and writes one `<w:r>` per run. It has nowhere to
  record the direction, so the run properties do not change at a boundary. An
  empty paragraph is a single empty run, and a paragraph whose text still had a
  level open over its last run is closed with an empty run after it.
- **RTF** keeps the control and writes it as an escape: the level's own code
  point to open one, and `U+202C` to close one, each with its own `\uc0`. The
  levels open are tracked separately from the levels the text is under and
  brought into line with them only when text is written, which is what makes the
  open-and-close-a-level-with-no-text-between case write nothing. A page break is
  a boundary of its own: it closes every level the reader was told of, and keeps
  them, so the text after it is written as if they were still open and a later
  control, or the end of the paragraph, closes them again.
- **HTML** names the direction rather than keeping the control. The outermost
  level of a paragraph's stack gets a class of its own, in order of first use,
  and a level inside it gets an inline style. A run of no-break spaces spans
  whatever levels the text around it is under, and only the markup is cut where
  the levels change.

## The two Unicode separators are breaks, and only the WordML writer knew it

U+2028, the line separator, and U+2029, the paragraph separator, are the only
two characters the reference tool spells out rather than writing as themselves,
and each of the writers answers a different question about them. The WordML
writer had them right. The HTML and RTF writers did not, and the reason they
did not is worth recording, because it is a hole in the suite rather than a
judgement about the behaviour:

> The fixtures for the two separators — `ps`, `psonly`, `ps2`, `ls`, `lsonly` —
> were written for the WordML writer, and were only ever checked against it.
> Nine hundred and seventy-nine checks went by without the other two writers
> being asked what they make of a U+2029. Holding the same five fixtures to
> HTML, RTF and txt is what found it, and is now part of the suite.

**A U+2029 is a paragraph mark in RTF, and not a `\u` escape.** It is written
as `\` followed by a line ending, which is what a paragraph mark is, and it
ends the paragraph in every other respect as well: the levels the text was
under are closed, and the next `\u` escape says `\uc0` again. It was
characterised by comparison rather than by argument — every shape it was tried
in, with levels and marks and page breaks and `\uc0` state included, gave the
same bytes whether the character in it was a U+2029 or a plain LF, and twenty
three shapes were run that way.

**A U+2029 ends a paragraph in HTML, and pairs with nothing.** It is a line
terminator like CR and LF, but where a CR followed by an LF is one terminator, a
CR followed by a U+2029 is two: they are terminators in their own right and
neither swallows the other. The two are told apart by the difference, `a\r` + a
separator, which the reference tool makes two paragraphs, where the same input
with an LF is one. The scan that finds the end of a line is byte by byte and
never saw the three bytes, so the separator is now recognised there and three
bytes are stepped over rather than one.

**A U+2028 is a break inside the paragraph in HTML,** written as `<br>` followed
by a line ending of its own, inside whatever span the levels want rather than
closing it. It is room the line takes up, so a line of nothing but one is not a
blank line, and it does not end the paragraph: a separator after it does, and
is then a second paragraph. The WordML and RTF writers were already right, and
in RTF a U+2028 is a `\uc0` escape where a U+2029 is a mark.

A differential over a break-heavy alphabet — the two separators, the embedding
controls, the marks, the C0 controls, page breaks, Hebrew, tabs and blanks —
puts the RTF writer at 999 of 1000, WordML at 1000 and HTML at 980, none of the
failures a regression, and all of the remaining HTML ones holding a separator
together with a mark, a level or a control. txt is at 345, and is not a
question about separators at all: the failures there are the embedding controls
and the marks, which the plain text writer is not reproducing.

## The plain text writer shows neither a level nor a mark, and knew neither

txt is the fourth answer to every question the three other writers give a
different answer to, and it is the easiest to get right, because it is the one
that writes no markup at all. It was also the one the suite had never asked, in
the same way and for the same reason as the two separators: the mark fixtures
were held to RTF and WordML, which record a direction, and the level fixtures to
the three that record one, and txt was in neither list. It is in both now, and
in the eight new shapes below, which are held to all four writers at once.

Two things are not shown. The five embedding controls are a direction given to
the text a paragraph covers rather than characters in it, so they go wherever
they are found — mid-line, at a head, a run of nothing but controls — and a
level with no text between it leaves nothing behind at all. And a mark at the
head of a paragraph is how that paragraph was told which way to read rather
than something in it, so it goes too. That is the same rule the other three
writers follow, through the same `tu_bidi_mark()`, and it was already written
down; what was missing was that the fourth writer never consulted it.

**A paragraph begins with a mark only when the mark is the very first thing in
it.** This is the part that had to be settled, and it went the opposite way to
the obvious one. An embedding control in front of the mark does not get out of
the way of it: the paragraph began with *that*, and the mark after it is text
and is kept. So `LRE LRM A` writes `LRM A` and `A CR LRE LRM B` writes
`A CR LRM B`, while `A CR LRM B` and `LRM A` write `A CR B` and `A`. One mark
is dropped, and a second is kept: `LRM LRM` writes one `LRM`, and
`A CR LRM LRE LRM B` writes `A CR LRM B`. Whitespace counts as content, so a
mark after a space, a tab, an Arabic letter mark or a U+2028 is text, and only
a mark with nothing at all in front of it names a direction.

**Four things put the next paragraph back at a head, and three of them are the
ones you would guess.** A CR, an LF, a CRLF pair and a U+2029 each end a
paragraph here and each put the mark that follows at a head of its own, so a
mark in the middle of a line is dropped and the same mark in the middle of the
next one is not. A U+2028 is the exception and the interesting one: it is a
break *inside* a paragraph, and the paragraph is not over, so the mark after it
is text. Nor do the controls, a page break, or any of the C0 controls re-arm
it — a NUL, a VT and a bell are all shown, and a shown character is the end of
whatever claim to a head was being made.

The differential that had txt at 345 of 1000 has it at 1000, RTF at 1000 and
WordML at 1000, and HTML at 991, with no regression in any of them.

## A CR and an LF pair with each other, and one mark may lie between them

A CR on its own is a paragraph mark, and a CR that pairs with the LF after it is
that same mark written as a pair, with the CR kept as text. The pairing was
already being looked for and already went over the embedding controls, since a
level between the two halves is recorded nowhere anyway. What it did not go
over was a mark, so `CR LRM LF` came out as two terminators and a paragraph of
its own, where the reference tool makes one — a single failing shape in the RTF
differential, and one that a 1470-shape sweep of the question now settles.

**The mark is taken at the head of the line the CR begins, and only then does
the search for the LF start.** So the order is the rule, and the rule is two
rules rather than one. A CR begins a line, and the head of a line may name a
direction, so a mark immediately after it is that mark and is dropped. What is
left is the CR/LF question proper, and the controls are gone over and nothing
else is. That is why the mark has to be *first*: a control in front of it means
the head of the line was that control rather than a mark, and the mark is text
between the two halves of what would have been a pair. It is also why a second
mark breaks it — the head was the first one, and the second is text.

Every combination of two and three characters over the seven characters
involved, in four positions each, was tried against the reference tool, and
these two are the whole of the answer: a mark first and then only controls
pairs, and anything else does not. A mark that is text rather than a direction,
the Arabic letter mark, breaks the pair anywhere it appears, and a CR whose LF
has anything else after it is a lone CR, which is the case the code already
had.

This is the same two rules the other writers go by, in the same order and for
the same reason, which is the main reason to think the reading is right rather
than a curve fitted to the fifteen shapes: the head of a line is the head of a
line wherever it is asked about.

The HTML and WordML writers had the wrong order for it, and it is worth saying
what was wrong rather than only that something was. Both went over the levels
*and* the marks together, and without limit, when looking for the LF. That is
right for finding the end of a line — a mark is not shown, and neither is a
level, so neither of them ends a line — and wrong for pairing, because a mark
in the middle of a paragraph is text after all, and only the one at the head of
the line the CR begins is not. Two questions had been answered with one scan
and they needed two. They are held to all eight shapes now, where the two markup
writers had been left free, and the four writers answer all eight the same way.

## Divergences

These are the known points where this port does not do what the reference tool
does. Most of them are a missing feature rather than a difference in output for
a feature that exists on both sides.

- **The remaining office containers are not written.** `doc`, `docx`, `odt` and
  `webarchive` are recognised by the parser, and `-convert` with one of them
  produces no file and a message naming the format. The reference tool writes
  all three office containers. This is the largest remaining gap; the harness's
  `recognised $fmt` cases pin the *parse* only, since a case that ran one of
  these writers would be testing the writer rather than the parser.
- **A document's readers do not record its layout, so a document read as RTF
  or as HTML loses it.** The reference tool keeps what a rich text reader
  learned — the resolved default font (`Times` for HTML, `Helvetica` for RTF,
  against the `Helvetica Light` a plain text document gets), the page size and
  margins in an `RTF` `w:sectPr`, a `w:defaultTabStop`, and the paragraph
  spacing an HTML reader applies. `tu_doc_t` holds the text and nothing else,
  so those run properties are written from the `-font` and `-fontsize` options
  and `w:sectPr` and `w:docPr` are empty whatever was read. The same gap shows
  up converting RTF to RTF, where the font table names the port's default
  instead of the document's, so it is a gap in the readers rather than in the
  wordml writer. A document read as plain text is unaffected, and is what the
  harness's wordml cases compare.
- **`webarchive` is not read.** The reference tool reads a webarchive as an
  archive of a web page and reports its title and its text. Reading one needs
  a binary property list reader, which this port does not have, so a file with
  that name has its bytes read as text. A file with that name which is *not*
  an archive matches the reference tool exactly, and is a case in the harness;
  a real one does not. `-format webarchive` is refused, as it is for every
  format this port has no reader for.
- **Single byte encodings are not converted.** `-encoding` accepts the
  `NSStringEncoding` numbers the reference tool accepts — it has a table for
  each, so a number is never an invalid encoding — but only `4` is one this
  port writes. Given any other, `-info` still behaves normally, and a command
  with output to write says so and fails. The alternative, writing UTF-8 in
  their place, would be a silent answer to a question that was not asked.
  Notably the reference tool's single byte encodings succeed on ASCII-only
  input and produce bytes identical to UTF-8, so this only shows on text that
  is not ASCII.
- **Bytes that are not valid UTF-8 are read as Mac OS Roman.** The reference
  tool decodes plain text as UTF-8 and falls back to Mac OS Roman for a byte
  sequence that is not valid UTF-8, then writes the result as UTF-8, so
  `printf 'a\xe2b'` comes back as `a‚b` (U+201A). This port passes such bytes
  through unchanged, which shows in every writer rather than in the wordml one
  alone: `txt`, `rtf`, `html` and `wordml` all differ on such input. The
  mapping is the ordinary Mac OS Roman table, so this is a small, well defined
  reader change rather than an open question, and it is not made here because it
  belongs to the plain text reader and not to any writer.
- **`-inputencoding` is accepted and ignored.** The reference tool decodes
  with it, and reports `Text encoding ... isn't applicable` for input the named
  encoding cannot represent.
- **`-font` outside the five mapped families** is not mapped, and no font
  database is consulted.
- **`-excludedelements`** selects an XHTML serialisation that is not
  implemented; the argument is parsed and validated.
- **Fifteen HTML cases where a C0 control other than NUL sits between two
  spaces differ.** A NUL among blanks is handled: it is not shown, and the
  blanks after it are at the head of their line, so they are counted as leading
  however much text came before the NUL. The other C0 controls do not do that,
  and the blanks after one of those are counted between two words where the
  reference tool writes them at the head of a line. What decides it is not the
  run on its own: a run of one space before the control gives a run that leads,
  a run of two or three does not, and a second control between the runs changes
  it again, so the rule is not written here rather than guessed at one that
  would more likely break the cases that already agree. A broad differential
  over the C0 controls, 12960 cases, agrees on all but 420 of them, and none of
  those 420 is a NUL. The NUL cases are four of the fixtures in the suite.


- **Writers write in place rather than through a temporary file**, so a
  destination that is a symlink to a directory is followed and fails where the
  reference tool replaces the link, and an existing read-only regular file
  cannot be replaced.
- **A malformed RTF broken-down date is not reproduced.** `\yr`, `\mo`, `\dy`,
  `\hr`, `\min` and `\sec` are read and normalised, but a field before 1904 has
  no representation here, and the reference tool's own handling of it is not
  uniform enough to copy.
- **A malformed `-keywords` list is taken and not acted on.** The reference
  tool mishandles every form of the option, in a different way for each form,
  so this port parses the list and does nothing with it rather than
  reproducing behaviour that looks like a fault in the original.
- **The five empty and nested HTML block cases listed above** differ by one or
  two characters each, and are left rather than fixed by a rule that would
  break more of the battery than it would repair.

## Coverage

`tests/textutil-parity.sh` is at 1069 checks, and passes in full against the
release, debug and ASan/UBSan builds. The suite compares exit status, stdout,
stderr, and the bytes of every file and directory produced, so a missing output
is caught as well as a differing one. It covers the option parser and its
error cases, `-info` including the preview and BOM fixtures, `-stdin`, the
`-cat` and `-convert` paths, all five writers, `-encoding` across the Unicode
family, `-format` validation and forcing, the read and write diagnostics, and
the destination edge cases: a missing parent, an unwritable parent, a parent
that is a file, a destination that is a directory, a bundle onto a directory
and onto a file, spaces in names, and a file whose name looks like an option.

The wordml cases take each fixture through the writer: the paragraph marks and
separators, both orders of a carriage return and a line feed, the mark that opens
a paragraph and gives it a direction, tabs at each end of a line and on their
own, the three characters escaped and the quote that is not, the empty document,
both metadata times, the font families and the sizes in half-points, and the
naming of an output with no `-output` at all. They are plain text inputs, for
the reason given in the divergences above.

The embedding controls are held to all three writers at once, by eight fixtures
that cover a level around one stretch and round a whole paragraph, a level
nested inside another with both closed one at a time, a level opened and closed
with no text between it, a level the text never closes, and a page break with a
level either side of it and one with a level inside it.

A NUL among blanks is held to all four writers by four fixtures: one blank
either side of it with a word, three blanks after it, a NUL at the head of a
line with blanks on both sides, and a line that ends with blanks after one. A
NUL is not shown, and the blanks after it are at the head of their line, so
they are counted as leading however much text came before it.

The readers are covered at the end of the suite. Each reads a file that was
written once by the reference tool into a template, which every case then
copies, so that both tools are given identical bytes rather than each other's
work. They cover RTF, a bundle and HTML as they are written, the same three
with metadata, five files that are not RTF well enough to read, seven bundles
that are each wrong in a different way, and four files whose name says one thing
and whose contents another.

Detection is covered separately, by the name cases above and by a group of
byte-gate cases: fifteen for the RTF mark and nineteen for the HTML one, each run
through both `-info` and `-convert` so that the type and the text are both
compared. Between them they pin the digit that is not wanted, the case
sensitivity, the newlines that may come before the mark and the tab that may
not, the byte each gate wants past its own spelling, the two HTML openings and
the doctype that is not one, the unclosed group and the balanced one, a
webarchive that is not an archive, and three files whose first line is empty,
which is when `-info` omits its `Contents:` field altogether.

Two fixtures are excluded from the tree comparison for reasons that belong to
`diff` rather than to the tools: `noperm.txt` cannot be read by `diff`, and the
`slink` symlink points at a sibling directory, which `diff` reports as a
directory loop once per root. Both notes name the root and would differ when
they are not dropped. The cases that use either fixture still compare exit
status, stdout and stderr.
