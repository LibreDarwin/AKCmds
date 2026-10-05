# tiffutil reverse-engineering notes

Observations from /usr/bin/tiffutil on Darwin 25 (arm64e). All are black-box
probes; nothing was disassembled or read out of the binary.

## Output is CoreGraphics/ImageIO, not plain libtiff

Every writing mode emits a TIFF produced by Apple's own writer, which is not
libtiff's `TIFFWriteScanline` path:

- **Always big-endian**, even when the input is little-endian. A 4x4 LE input
  comes back as `MM` / big-endian.
- **Always carries an ICC profile** (tag 34675) that the input did not have.
- Injects fields the input may omit: FillOrder(266)=1, Orientation(274)=1,
  SampleFormat(339)=1, ResolutionUnit(296)=2.
- `-packbits` writes Compression 32773, the registered Apple/Adobe code point
  for PackBits, rather than 32773-by-accident; the value is 32773 either way
  but the surrounding writer behaviour is CoreGraphics'.

## The ICC profile is a per-colour-space constant

This is the key finding that makes byte parity tractable. The blob does not
depend on image size, bit depth, input endianness, or compression:

    gray  4508 bytes  class=mntr space=GRAY  tags=desc dscm cprt wtpt kTRC
    rgb   3144 bytes  class=mntr space=RGB   tags=cprt desc wtpt bkpt rXYZ
                                       gXYZ bXYZ dmnd dmdd vued view lumi
                                       meas tech rTRC gTRC bTRC
    cmyk 55280 bytes  desc "Generic CMYK Lab Profile", manufacturer "appl"

Verified identical across 4x4 and 64x64 gray, 8- and 16-bit, RGB and RGBA
sources, and across -none/-lzw/-packbits. So the profile is one of three fixed
byte strings selected by the output colour space, not a synthesis algorithm
that has to be rederived. All three are embedded verbatim in
src/tiffutil/icc_profiles.h.

Selection is by the *output* photometric, which is why separated matters:
photometric 0/1 gets gray, 5 gets CMYK, and everything else gets RGB. The
CMYK blob is 17x the size of the others, so it is worth confirming it is
really one constant: it appears byte-identical for 4-channel and 5-channel
separated sources, tagged and untagged, 8- and 16-bit, all three
compressions, and for both the drop and the keep case.

Each blob is byte-identical to the system profile of the same name, which is
where they can be re-derived from if the header ever has to be regenerated:

    /System/Library/ColorSync/Profiles/Generic Gray Gamma 2.2 Profile.icc
    /System/Library/ColorSync/Profiles/sRGB Profile.icc
    /System/Library/ColorSync/Profiles/Generic CMYK Profile.icc

Gray profile internals, for reference:

- `desc` "Generic Gray Gamma 2.2 Profile"
- `cprt` "right Apple Inc., 2012"
- `wtpt` XYZ = (62289, 65536, 71372)
- `kTRC` a 1024-entry `curv`. It is *not* a clean 2.2 power function:
  index 100 is 634 and index 512 is 14057, whereas a true 2.2 curve gives
  22774 and 47845. Embedding the table verbatim is the only exact route.

## Predictor=2 is emitted for LZW only, and only at 8 bits/sample

    input        -none    -lzw    -packbits
    gray 8-bit   (none)   2       (none)
    gray 16-bit  (none)   (none)  (none)
    rgb  8-bit   (none)   2       (none)

The predictor is applied, not merely tagged: 16-bit samples round-trip
bit-exact through -none (0, 7, 14, ... preserved), and 8-bit data is
reconstructed correctly after differencing.

## -none is a lossless repack

Pixel samples survive unchanged in value; only their byte order follows the
new big-endian container. 8-bit samples are therefore byte-identical, 16-bit
samples are byte-swapped, and the ICC profile plus the injected fields are
added.

## Surplus samples are collapsed, but the collapsing is not uniform

The output never carries more than `ncolor + 1` samples, where `ncolor` comes
from the photometric (0/1/3 -> 1, 2/6 -> 3, 5 -> 4), and a white-is-zero
photometric is first rewritten to black-is-zero without touching samples. What
survives, and *which* bytes are read, took a matrix to pin down:

- An `ExtraSamples` tag is authoritative: the first value in it becomes the
  output's alpha value, and `ncolor + 1` samples are kept. A later value in
  the same tag is ignored, so the output always carries at most one
  `ExtraSamples` entry.
- With no tag, `spp == ncolor + 1` is treated as tagged and the spare sample
  gets the synthesised value 2 (associated alpha).
- With no tag and more samples than that, nothing is associated: the output
  keeps `ncolor` and carries no `ExtraSamples` at all.
- `ncolor == 3` (RGB, YCbCr) copies per pixel, striding by the *source*
  `spp`, so the kept samples are interleaved correctly.
- Anything else copies a contiguous run from the start of each row, so a
  single-row crop of the strip is not equivalent to a full-width one. The
  uncopied tail has to be closed up with `memmove`; leaving it in place
  duplicates the beginning of the row.
- Untagged non-RGB is the odd one out: it strides by `ncolor + 1`, not by the
  source `spp`, so the samples kept are a different set again.

The IFD is written in ascending tag order, which puts `Colormap` (320) before
`ExtraSamples` (338) and `SampleFormat` (339). Getting that wrong only shows up
on palette images with spare samples.

## 16-bit RGB wider than RGBA comes out black

Once more than one byte per sample is involved, a three-channel pixel that is
wider than RGBA is dropped to an all-zero image. Photometric 2 with `spp > 4`
at 16 or 32 bits per sample yields a correct header and a strip of the right
length that is entirely zero:

    16-bit rgb spp4   data preserved
    16-bit rgb spp5   all zeros        (regardless of the tag or nex)
    16-bit rgb spp6   all zeros
    16-bit rgb spp7   all zeros
    16-bit gray spp7  data preserved
    16-bit cmyk spp6  data preserved
    8-bit  rgb spp7   data preserved

Photometric 6 is exempt, and so is every 8-bit case, so the condition is
narrow: `bps > 8 && photometric == 2 && spp > 4`. It is not a decoding
failure -- there is no diagnostic on stderr and the exit status is 0.

## A pixel too narrow for the output format produces an empty file

The reference tool cannot build an image at all when the file carries fewer
samples than the output needs, and says so by writing a zero-byte file while
still printing `1 image written to ...` and exiting 0:

    photo 0, 1, 3   any spp   written normally
    photo 2         spp >= 3  written normally
    photo 5         spp >= 5  written normally
    photo 6         spp == 3  written normally, as photometric 2
    photo 2         spp < 3   empty file
    photo 5         spp < 5   empty file
    photo 6         spp != 3  empty file

`photo 5, spp 4` is the interesting one: a plain four-sample separated TIFF is
the ordinary way to write CMYK, and it is the one case that yields nothing.
The count in the message comes from the number of inputs, not from what was
written, so the message is unchanged either way.

The threshold is on the *file's* sample count, not on what survives the
surplus collapse above: six untagged separated samples collapse to four and
are still written, while four tagged ones are not.

Within a multi-directory file the unusable directory is dropped and the
surviving ones are byte-identical to writing them on their own, with no
leftover bytes in between. So the model is a plain per-directory skip, and a
file whose directories are all unusable comes out empty.

## YCbCr is converted, not copied

A three-sample photometric 6 file is rewritten as photometric 2 with the
samples colour-converted, so it is not a repack. The samples arrive six bytes
per 2x2 block — four luma then one chroma pair — and leave as twelve, so the
strip grows by half and the pixels come out in `(B,G,R)` order.

Blue and red are luma plus a per-chroma contribution table, and each table
entry is rounded *on its own* rather than against the luma. That is what puts
two identical-looking ties on opposite sides: with the default blue
coefficient a Cb of 3 contributes -221.5, which the reference rounds to -222,
so `239 + -222` is 17 where rounding the whole sum would have given 18. Green
is the exception — it is one expression rounded once, and its ties fall toward
zero (`190.5 -> 190`, `252.5 -> 252`) rather than away. Building green from two
separately rounded chroma tables instead costs 1050 mismatches over a
6400-record corpus.

Coefficients come from tag 529 and default to the ITU-R BT.601 set
`(299,587,114)/1000` when it is absent. Tags 530 and 532 are read by the
reference and then ignored: it always treats the data as 2x2 whatever the
subsampling tag claims, and it never scales the luma by the reference
black-white tag.

Three residuals remain, all of them reference-side quirks rather than gaps in
the model:

- Two exotic coefficient sets round one channel one step away from the
  formula. `(299,299,402)` wants `-149` where nearest rounding of `-149.5`
  gives `-150`, which moves a handful of bytes in blue; `(70,290,22)` costs a
  single red byte on some inputs. An exhaustive search over float widths,
  accumulation orders and offsets could not find arithmetic that reproduces
  both, and no model that fixes them keeps the sane coefficient sets exact.
- Widths holding an *odd* number of 2x2 blocks make the reference read past
  the end of a block row: the last block's bottom luma pair comes from the
  next block's top pair, and the final block resolves to white. The output is
  still deterministic but is not a conversion of the file, so it is not
  reproduced here.
- With a pathologically small `Kg` the green channel overruns 255 and the
  reference occasionally lands a step either side of the clamp.

The sane coefficient sets — including every BT.601 and BT.709 variant, and
multi-strip layouts — are byte-identical.

## Text modes are deterministic and directly comparable

`-info` and `-verboseinfo` print per-directory summaries; `-verboseinfo` adds
a `Strips (Offset, ByteCount):` block that `-info` omits. `-dump` prints the
raw IFD. These are pure text and are the strongest parity signal for
diagnostics and for the operations that rewrite files.

One warning is mixed into `-info` and `-verboseinfo`, and **not** into `-dump`
or any of the write operations:

    TIFFReadDirectory: Warning, Sum of Photometric type-related color channels
    and ExtraSamples doesn't match SamplesPerPixel. Defining non-color
    channels as ExtraSamples..

It fires when the ExtraSamples count falls **short** of what
`SamplesPerPixel` needs over the photometric's channel count; a sum that comes
out *long* is not checked, and a photometric with no known channel count is
skipped. It goes to **stderr**, not into the listing, so folding the two
streams together hides the mistake. Found by the report sweep in
`tests/tiffutil-parity.sh`, which runs all three reports over every fixture
shape in both byte orders -- the write-path cases never touched it because a
page the reference has just re-encoded carries consistent tags.

## Diagnostics and exit codes

    no arguments                                  1  usage
    -info with no file                            1  usage
    unknown option, or -out with no command        1  "Error: No valid command provided."
    two commands                                  1  "Error: One input file name expected."
    -extract N beyond the last image              5  "Error: F has only M image." +
                                                      "No output file created due to errors."

### A failed open is worded three different ways

The same unreadable input gets a different diagnostic in every mode, so the
open layer classifies the failure and the caller doing the reporting chooses
the words. The shapes, and what each mode says about them:

| input | `-info` / `-verboseinfo` | `-dump` | write ops |
|---|---|---|---|
| not there | `TIFFOpen: F: No such file or directory.` | `F: No such file or directory` (rc 0) | `Error: Failed to create image source for file F.` + `Error: Can't read from file B.` |
| not readable | `TIFFOpen: F: Permission denied.` | `F: Permission denied` (rc 0) | `Error: Can't open F. ...` |
| a directory | `F: Cannot read TIFF header.` | `(null): Error while reading TIFF header.` | as "not there" |
| under 8 bytes | `F: Cannot read TIFF header.` | `(null): Error while reading TIFF header.` | `Error: Can't open F. ...` |
| version ≠ 42 | `F: Cannot read TIFF header.` | `(null): Error while reading TIFF header.` | `Error: Can't open F. ...` |
| magic not II/MM | `F: Not a TIFF or MDI file, bad magic number N (0xH).` | `(null): Not a TIFF or MDI file, ...` | `Error: Can't open F. ...` |
| header fine, no directories | *nothing* | header only, rc 0 | `Error: Can't open F. ...` |
| directory walk fails | `TIFFFetchDirectory: ...` + `TIFFReadDirectory: Failed to read directory at offset N.` | header, then `(null): Error while reading directory count.` or `(null): No space for TIFF directory.` | `Error: Can't open F. ...` |

Three things are easy to get wrong and all three were measured:

- **The magic is the little-endian value of the two bytes**, whichever order
  the file claims. `XY` (0x58 0x59) is reported as `22872 (0x5958)`, not
  `22617 (0x5859)`.
- **The reports name the file they were given; `-dump` names `(null)`.** So
  `-dump` of a bad magic prints `(null): Not a TIFF...` while `-info` of the
  same file prints `F: Not a TIFF...`.
- **The write operations name nothing at all.** They print only the refusal
  itself — no `TIFFOpen:` line, no `TIFFFetchDirectory:` line — so any
  diagnostic emitted during a write is wrong. Only a file that is not there at
  all is named, as `Failed to create image source for file F.`

`-dump` shows a header it managed to read even when the directory walk is what
failed, and reports a sound header with no directories as a **success** (rc 0).
`-info` blames nothing for the same file: it just cannot open it.

The `-info` and `-verboseinfo` refusal line is a *third* wording, with one
extra clause: `Either it isn't readable, it isn't a TIFF file, or there are
unrecognized tags`. The write operations leave `isn't readable` out.

### Divergences on this path, both deliberate

1. `-info` and `-verboseinfo` **segfault** (rc 139) on every unreadable input --
   a missing file, a non-TIFF, an empty file, a directory -- because the image
   source is null when the report is formatted. Reproducing a crash is not a
   parity target, so the same messages and a clean non-zero status are emitted
   instead. The harness drops only the exit status for those cases
   (`check_nostatus`) and still compares both streams, the files present and the
   fixture bytes.

2. When a tag is refused hard enough to lose the image (see below), the write
   operations report `1 image written to F.` and exit 0 while leaving a
   **zero-length file** behind. Re-running that write destroys the image
   silently, which is why it is not reproduced: the port writes the image
   through. `check_nobytes` keeps those cases in the harness with the output
   bytes left out -- the file must still be created and listed, and both
   streams and the exit status still have to match.

Other details:

- The default output name is `out.tiff`, not a derived name from the input.
- Success prints `N image(s) written to F.` (singular for 1) to **stderr**, not
  stdout. Nothing is ever written to stdout on a successful write. Found by
  tests/tiffutil-parity.sh, which compares the two streams separately; every
  earlier ad-hoc check folded them together with `2>&1` and so missed it.
- `-out` is positional, not a flag: it must come **after** the input name.
  `tiffutil -none -out o.tiff i.tiff` is a usage error, and a test written that
  way silently compares two usage errors instead of the thing it meant to test.
  Four harness cases were vacuous for exactly this reason.
- An existing output file is overwritten silently.
- `-extract 0` succeeds and writes a file.
- `-extract` keeps the source directory's compression rather than rewriting
  uncompressed, but the narrow-depth overrides still apply, so `-extract 0` of
  a **one-bit gray** page comes out `Compression 4` at one bit even when the
  source itself was uncompressed. The override is therefore made in one place
  shared with the rewrite path, or an extract and a rewrite of the same
  directory disagree.
- `-cat` warns `Warning: Sizes of concatenated images are not the same; this
  will lead to problems in choosing the appropriate image in some cases.` and
  then prints a per-image geometry line. `-catnosizecheck` and
  `-cathidpicheck` select the other policies.

## An unnamed field draws a warning, but only where a directory is read

    TIFFReadDirectory: Warning, Unknown field with tag 347 (0x15b) encountered.

Walking a directory that holds one candidate tag at a time, over the whole tag
space, settles what "unknown" means here: **152 tags** are named, and every
other one of the 65536 draws the warning above. The set is in
`known_tags[]` in tiff_text.c; it was measured, not guessed, and it is not the
same set `-dump` names tags from -- 347 is `JPEGTables` in `-dump` output and
an unknown field in `-info`, because the two go through different
vocabularies.

- The warning goes to stderr, one line per unknown tag, and only from `-info`
  and `-verboseinfo`. `-dump` walks the raw IFD itself and stays quiet, as do
  the converting modes.
- It appears once per directory, in the order the entries sit in the file, not
  in the order of the tag numbers -- a directory listing tags descending warns
  in descending order. (TIFF wants IFD entries ascending; the reference tool
  reads them as it finds them.)
- It comes before the directory's other complaints, so it precedes the
  ExtraSamples line when both fire.
- **Predictor (317) is the one conditional name**: named under compression 5,
  8, 32909 and 32946 (LZW, both Deflate spellings, PixarFilm), and unknown
  under everything else, including a directory with no Compression tag at all.
  Everything else in the 152 is named under every compression probed: none,
  LZW, Deflate, PackBits, G4, YCbCr, palette, CMYK, Lab, LogLuv and RGB16.

The reference tool hangs for minutes when a directory holds tag 333 (InkNames)
with a one-element count, so no fixture uses it.

## NewSubfileType is a set of flags, and a value can be refused

Three `-info` behaviours that are easy to get wrong, all settled by sweeping
values rather than by reading the spec.

**NewSubfileType (254)** is three flags, not a number. Each one that is set is
named, in bit order, and the names are joined with a slash:

    bit 0  reduced-resolution image
    bit 1  multi-page document
    bit 2  transparency mask

A value with none of the three set -- zero, or anything above bit 2 -- leaves
the name empty and the line still goes out, right under `Directory at` and
before `Image Width`:

    Subfile Type: (0 = 0x0)
    Subfile Type: reduced-resolution image (1 = 0x1)
    Subfile Type: reduced-resolution image/multi-page document/transparency mask (7 = 0x7)

The names carry their own trailing space, which is what puts one between the
name and the value: the empty case has only the space that follows the colon.
The deprecated **SubfileType (255)** prints nothing at all, and carrying both
tags prints one line, from 254.

**A value can also be refused**, which is a separate and later complaint than an
unknown field. Three tags are refused this way, and each one simply loses its
line and draws this on stderr instead:

    _TIFFVSetField: F: Bad value 0 for "ResolutionUnit" tag.

    tag   accepted   named
    266 FillOrder   1-2        msb-to-lsb, lsb-to-msb
    274 Orientation 1-8        row 0 top/lhs, row 0 rhs, and the rest
    296 ResolutionUnit 1-3     none, pixels/inch, pixels/cm

All three start at 1, so zero is refused by all three, and no substitute is
printed for a refused value -- not the number, and not the word unknown.
Orientation's four rotated names say `lhs` and `rhs`, not `left` and `right`,
which is the same word the top-left and bottom-left names already used.

The complaint comes *after* every unknown-field warning the same directory
drew, not among them, one per offending field and in the order the entries sit
in, and `-dump` prints neither the line nor the complaint. Four more tags
refuse values but refuse harder, taking the whole report with them; they are
described under "A refused value can take the whole report with it" below.

### A refused value can take the whole report with it

The other half of the family refuses so hard that no directory is reported at
all. These four, and only these, print a warning followed by this:

    _TIFFVSetField: F: Bad value 0 for "SampleFormat" tag.
    Error: Can't open F. Either it isn't readable, it isn't a TIFF file, or
    there are unrecognized tags; try tiffutil -dump for more info.

    tag     accepted   refused
    338 ExtraSamples   0-2     a list; any element outside 0-2 refuses
    339 SampleFormat   1-6
    32996 DataType     0-3
    32998 TileDepth    1-65535 only 0 is refused

The walk stops at the *first* refusal in a directory, so with both 32996 and
339 out of range only the one the entries reach first is named. The two lines
come after that directory's unknown-field warnings, as with the softer family.
Nothing reaches stdout -- not even the directory heading, and not the lines for
directories before this one. `-dump` reads the same files without complaint, so
the *diagnostic* is confined to `-info`/`-verboseinfo`.

**The write operations are not unaffected, though.** A file refused this hard
loses its image, and `-none` on one prints `1 image written to F.` and exits 0
while leaving a **zero-length file** behind -- the conversion had already lost
the source. The softer family (33996/33998/338 at 0-2, and 266 at 0) does not
do this: those files convert normally and come out byte-identical. The port
writes the image through rather than reproducing the data loss, so those four
harness cases keep everything but the output bytes (`check_nobytes`). See the
second divergence under *Diagnostics and exit codes*.

**ExtraSamples names the count, not the element.** It is a list, so it is
refused when any element is out of range, but the number in the complaint is the
number of elements: a one-element list holding 3 refuses naming `1`, and a
two-element `[3, 0]` refuses naming `2` even though 3 is the element that was
wrong. Measured against one- and two-sample-per-pixel images; the three- and
four-sample fixtures this would need died on something else before the
complaint, so nothing is claimed for those.

After the two lines the reference tool dereferences the null image source left
by the failed open and dies of SIGSEGV -- rc 139, observed as signal 11. Like
the four unreadable inputs below, that is a crash and not a parity target, so
this port prints the same two lines and stops with a non-zero status. The
harness compares both streams and all the bytes for these cases and leaves only
the exit status out (`check_nostatus`).

**Whichever of DataType and SampleFormat sits later is the one reported.** The two
tags say the same thing in numberings a count apart, and a directory may carry
either or both, so the report follows the entry that comes last rather than
preferring one tag: with 32996 after 339 the report is named from 32996, and
with 339 after 32996 it is named from 339. DataType's enumeration is
SampleFormat's with 1 and 2 exchanged:

    value    SampleFormat        DataType
    0        refused             void
    1        unsigned integer    signed integer
    2        signed integer      unsigned integer
    3        IEEE floating point IEEE floating point
    4        void                refused
    5,6      5 (0x5), 6 (0x6)    refused

An accepted value with no name of its own is printed as the number and its
hex (`5 (0x5)`), not as `unknown`. Note that SampleFormat is not ignored: the
reference tool injects `SampleFormat(339)=1` when the input omits it.

**`-dump` names all four Silicon Graphics extensions with their vendor**, and
marks the first two obsolete on top of that -- including the reference tool's
own spelling of the first, which is `Matteing` and not `Matting`:

    32995 OBSOLETE Matteing (Silicon Graphics)
    32996 OBSOLETE DataType (Silicon Graphics)
    32997 ImageDepth (Silicon Graphics)
    32998 TileDepth (Silicon Graphics)

## The argument grammar is positional, not a flag soup

Probed exhaustively; this is the whole of the parser's behaviour.

    tiffutil <operation> [num] <infile> [-out <outfile>]

- The operation must be `argv[1]`. `tiffutil infile -out out -lzw` is
  rejected with `Error: No valid command provided.`, because the first
  argument is not a keyword.
- But a *single* argument is never enough, so `tiffutil i.tif` and
  `tiffutil -foo` both print the bare usage block with no complaint, even
  though neither names an operation. The test is on the argument count first,
  then on `argv[1]`.
- The infile is the token immediately after the operation (after the number
  for `-extract`). It is taken positionally, *even when it begins with a
  dash*: `tiffutil -none -out o.tif in.tif` therefore reports
  `Error: One input file name expected.`, because `-out o.tif` is read as the
  output pair and `in.tif` is then a spare input.
- `-out` closes the command line. Its value must be the final argument, so
  both `tiffutil -none in.tif -out out.tif extra` and
  `tiffutil -lzw in.tif -out out.tif -out two.tif` report
  `Error: One input file name expected.`, as does a trailing bare `-out`.
- `-extract` is the one operation with its own diagnostics, because its
  number is parsed as a number rather than a name:
  `Error: Image number to be extracted expected.` for a non-numeric number,
  and `Error: Input file name expected.` when the name is missing. Every
  other operation says `Error: One input file name expected.`
- `-info`, `-verboseinfo` and `-dump` take one or more infiles. Their first
  name is positional even when it looks like an option, so
  `tiffutil -info -out z.tif i.tif` tries to open a file literally named
  `-out`. Only a `-out` *after* the first name is diagnosed, with
  `Error: Can't specify output file name for -info, -verboseinfo, or -dump.`
- Every one of these failures prints the error line and then the same fixed
  usage block, which is 660 bytes, starts with a capital `Usage: tiffutil`,
  and is written to stderr. The block is reproduced in `usage_text` in
  main.c.
- Reports exit 0 even when a report fails, because the failure is reported
  inline rather than as a command-line error.

## Coverage

`tests/tiffutil-parity.sh` is the executable form of this file: it builds
synthetic fixtures (both endiannesses, 8- and 16-bit, gray/RGB/RGBA/palette,
single- and multi-strip, single- and multi-directory) and diffs stdout, stderr,
exit status, the produced file set and every produced byte against
`/usr/bin/tiffutil`. Compressed fixtures are manufactured by running the
reference tool's own compressor, so the harness contains no second
implementation of LZW or PackBits encoding.

Known gaps, as reported by that harness and not yet fixed:

- *Nothing is currently failing.* The list below used to name `-cat`, palette
  carry-through, multi-directory rewriting and `-extract N` for N > 0; all
  four are implemented and covered now, and each was verified again by hand
  outside the harness as well. Kept here so the earlier claims are visibly
  retired rather than quietly dropped.

**A tag listed twice is set once, from the entry that comes first.** A directory's
entries are meant to ascend, and a repeat counts as going backwards, so a tag
listed twice trips the entry-order check exactly as a larger tag written ahead
of a smaller one does. It is said once for the directory rather than once for
the entry that gave it away, and ahead of everything else that directory has to
say -- even ahead of an unnamed-field warning for an entry that comes first:

    TIFFReadDirectoryCheckOrder: Warning, Invalid TIFF directory; tags are not
    sorted in ascending order.

Two directories that both descend get a warning each, and `-verboseinfo` says
the same thing `-info` does. `-dump` walks the raw IFD itself and says nothing,
and neither does a conversion.

Because only the first entry for a tag is ever set, the entries after it are
never acted on at all, and every complaint about a tag is made where its first
entry sits. An unnamed field listed twice draws one warning, not two, and two
different unnamed tags whose copies interleave warn in the order their *first*
entries sit -- 347 then 348 for entries 347, 348, 347, 348. A field that is
turned down behaves the same way, which is why the value named is the first
entry's and not the last: FillOrder 0 then 2 warns once and says nothing about
the 2, while FillOrder 2 then 0 says nothing at all, because a good value is
never replaced by a bad one that follows it.

`-dump` is the exception, because it is reporting entries rather than fields. Each
line shows the entry it is standing on, so a tag listed twice prints its own
value on each of the two lines instead of the first value twice.

**Two entries that disagree cost the image.** A tag listed twice whose entries
carry different values leaves the reference tool unable to make sense of the
directory: `-none` reports success and writes a zero-length file, the same
failure as a hard refusal. A tag listed twice whose entries agree is harmless,
and so is a directory that merely descends. Which of the two disagreeing values
is believed makes no difference -- a good value followed by a bad one loses the
image just as the other way round does -- because the first entry is the one
that is set and the second is ignored, but the duplicate itself is enough to
cost the image. This port writes the image through instead, so those cases
compare everything but the output bytes (`check_nobytes ./o.tiff`).

LogLuv is decoded now, in `main.c`, and pinned by fixtures (`bf652e9`). What
follows is the measurement trail that fixed the arithmetic rather than guessed
it, kept because the details in it are the reason the decode is bit exact.

- The way in was open throughout: a fixture can be written by hand, and one has
  been. The reference tool hands the file to ImageIO and lets ImageIO decode
  it: `local/AKCmds-main/tiffutil/tiffutil.m`
  opens a `CGImageSource` and adds frames to a `CGImageDestination` without
  touching a pixel itself, and the libtiff it bootstraps with `-Dlogluv=ON` is
  there to serve `TIFFPrintDirectory`, not the conversion. So there is no C in
  the reference to port and nothing to read the numbers off but its output,
  which is why every constant below was pinned against bytes rather than
  against a description of the format.

  Retracted below: two claims that were once taken for granted here were read
  out of the reference tool's output by a probe that was reading 3 bytes a
  pixel out of an answer that carries 12. Every pixel it compared was three
  bytes out of phase. That probe is what produced both the "compression 1
  narrows the samples" reading and the "this layout decodes to noise" reading,
  and neither is evidence. Both have to be measured again before they are
  written down as fact.

The strip layout, on the other hand, is settled. It comes from the header
  comment in `tif_luv.c`, and a 16-bit LogLuv pixel is one 32-bit word:

      1       15           8        8
      |-+---------------|--------+--------|
      S       Le           ue       ve

  Those four bytes are separated into byte planes and run-length encoded, as
  four runs in this order, **once per row**:

      Le high byte | Le low byte | ue | ve

  So a W-wide strip holding H rows is `H` repetitions of that four-plane group,
  not four planes of `W*H` bytes: `4*W*H` bytes before RLE either way, but the
  grouping differs and only one of them decodes. libtiff registers
  `LogLuvDecode32` as the *row* decoder and its strip decoder merely loops the
  row decoder over the strip (`LogLuvDecodeStrip` -> `tif_decoderow`), which is
  why the runs repeat per row.

  This was measured, not assumed. A 8x3 single-strip fixture built both ways
  disagrees completely:

      per-row planes    row0 = 1.0071, 1.0003, 0.9902   (the known Le=16384 value)
                        row1 = 0.0073, 0.0010, 0.0204
                        row2 = 3.6925, 21.8449, 3.9544
      per-strip planes  row0 = 0.4989, 1.4345, 0.9841
                        row1 = 0.0000, -0.0000, 0.0000
                        row2 = 2.15e+11, -9.21e+09, 3.74e+11

  A single-row fixture cannot tell the two apart, which is exactly why the
  first pass got this wrong: the H1 fixtures were one row tall.

  One byte per pixel in each plane. The control bytes are the ones libtiff
  writes:

      control byte >= 128   a run of (control - 126) identical bytes, then the value
      control byte 1..127   that many literal bytes follow
      control byte 0        nothing
      runs only pay off from 4 pixels up

  A literal run is capped at 127 bytes, so a plane longer than 127 needs more
  than one literal control byte. An encoder that emits a whole plane under a
  single control byte is read back as a *run* instead, which is what made this
  layout look wrong at first -- a 162-byte plane came back as a run of 36.

  Confirmed by alignment, which is the only test that can settle a layout. If
  the strip is understood then `[A,B]` must decode to `f(A), f(B)`, and must
  agree with `[A,A]` on its first pixel and with `[B,B]` on its second. It
  does, exactly:

      [A,B] -> 928801103, 905323085, 941186262 | 2961410514, 828267634, 822561048
      [A,A] -> 928801103, 905323085, 941186262 | 928801103, 905323085, 941186262
      [B,B] -> 2961410514, 828267634, 822561048| 2961410514, 828267634, 822561048

  So a fixture can be written by hand after all, and libtiff does not need to
  be asked for one. What is now missing is the arithmetic on the far side, and
  the first measurement of that has already overturned an assumption:

      BitsPerSample 32,32,32   SampleFormat 3,3,3   Photometric 2   Compression 1
      strip -> (-9.570675e-10, 3.235780e-09, 1.968663e-09)

  The reference tool answers a LogLuv file with a 32-bit IEEE float TIFF
  holding linear light on the order of 1e-9, not with 8-bit sRGB. That is the
  right order of magnitude for `Y = 2^((Le + 0.5)/256 - 64)`, which comes to
  2.1e-9 at `Le = 9000`. It followed that the white fill in `main.c` was not an
  unmeasured approximation of the right answer but the wrong output type
  altogether, and that matching this path meant emitting a float TIFF, which the
  writer could not do at the time.

  Settled already, and worth not measuring twice:

      Y = 2^((Le + 0.5)/256 - 64), Le = L & 0x7fff, sign in bit 31 of the word
      r =  2.690 X - 1.276 Y - 0.414 Z      CCIR-709 primaries, plain 2.0
      g = -1.022 X + 1.978 Y + 0.044 Z      gamma, no colour management
      b =  0.061 X - 0.224 Y + 1.163 Z

  (The `(int)(256 * sqrt(c))` quantisation and 0..255 clamp that libtiff's
  `XYZtoRGB24` applies belong to its 8-bit path, which is not the path in
  evidence here.)

  The chroma question is closed too, and the answer is not the one that was
  assumed. `ue` and `ve` are *not* mapped onto the usual CIE u'v' range --
  doing that drove `y` negative on the probe chroma, which is what first made
  the layout look wrong. They are scaled by `1/410` and fed to the Libjxl
  encoder inverse, and the scale is picked so that LogLuv's own white point
  lands on CIE u'v' `(4/19, 9/19)` exactly:

      u = (ue + 0.5) / 410
      v = (ve + 0.5) / 410
      s = 1 / (6*u - 16*v + 12)
      x = 9*u*s
      y = 4*v*s

  That `y` is positive and well behaved over the whole code range, and
  `X/Y = 9u/(4v)` -- a useful exactness check, since it holds independently of
  luminance. `ue = 86, ve = 194` is LogLuv's white-point *chroma*: it is the
  pair nearest D65's u'v' `(4/19, 9/19)`, because `ue = 410*4/19 - 0.5` and
  `ve = 410*9/19 - 0.5`. That is a statement about the chroma only -- the
  decoded value still needs `Le` to carry the luminance, and `16384` is the
  nearest `Le` to white:

      (8192, 86, 194)  ->  2.344797e-10, 2.328940e-10, 2.305413e-10
      (16384, 86, 194) ->  1.007082, 1.000272, 0.990167

  So the samples are RGB, not XYZ, and the full decode is the matrix applied to
  the derived XYZ. What matters is not negotiable, and it is not the whole
  chain in one width -- this is the part worth writing down, because an
  all-double implementation lands within 1 ULP of only about 58% of values:

  - The chroma chain (`u`, `v`, `s`, `x`, `y`, and both divisions) runs in
    **double**. Rounding any of it to float costs 20-25% of values.
  - `X`, `Y` and `Z` are each **narrowed once**, on assignment. This matches
    libtiff's `LogLuv32toXYZ(..., float *X, float *Y, float *Z)` signature: it
    computes in double and stores narrow.
  - `X` and `Z` are derived from the **unrounded** `Y`, not from `Yf`. This is
    the single detail worth the most -- getting it wrong costs 3% of values,
    and the error it produces is invisible except as a systematic one-ULP
    shift in `X` alone, which arrives at the samples in the ratio
    `2.690 : 1.022` that the matrix columns give it.
  - The matrix is accumulated **from the Y term outwards with a fused
    multiply-add per step**: the Y term is a plain product, and each of X and
    Z is then folded in with `fma`. This is the one detail that is invisible
    in a decimal diff and costs 54 values on its own; see below.

  Written as it should be implemented, with `F` a float32 store, `m` an FMA and
  every other operation double:

      Y  = exp2((Le + 0.5)/256 - 64)
      u  = (ue + 0.5)/410 ,  v = (ve + 0.5)/410
      s  = 1/(6*u - 16*v + 12)
      x  = 9*u*s ,  y = 4*v*s
      X  = F((x/y) * Y)          Yf = F(Y)          Z = F(((1 - x - y)/y) * Y)
      r = F( m(-0.414, Z, m( 2.690, X, -1.276*Yf)))
      g = F( m( 0.044, Z, m(-1.022, X,  1.978*Yf)))
      b = F( m( 1.163, Z, m( 0.061, X, -0.224*Yf)))

  `Le == 0` returns zero without touching the chroma, and the mask is not
  cosmetic: `Le` is 15 magnitude bits and bit 15 of the 16-bit field is a sign
  flag rather than a luminance. libtiff reads the field as a signed `int`, so a
  signed `Le` comes out of `LogL16toY` negative and `LogLuv32toXYZ` sends
  anything `<= 0` to zero -- which is why 32768 and above decode to black here.

  The accumulation order is the last thing to get right, and it is worth
  spelling out because it is *not* the obvious reading of
  `a*X + b*Y + c*Z`. Two measurements pin it:

  - Plain left-to-right double, `((a*X) + (b*Y)) + (c*Z)`, gives
    `343989/344043` = 99.984%. The 54 misses are single-channel one-ULP
    offsets, and they are not the oracle being sloppy: of those 54, 41 are the
    *correctly rounded* answer under left-to-right and only 13 are correctly
    rounded under FMA. So the oracle is neither plain nor exactly rounded, and
    the tie has to be broken by measurement.
  - Enumerating all six term orders crossed with plain and FMA accumulation,
    and with both associations, one form matches every value:
    `F(fma(c, Z, fma(b, Y, a*X)))` with the Y product unrounded on the way in.

  A grouped association, `a*X + (b*Y + c*Z)`, is worse than either. Plain
  float32 matrix arithmetic is far worse -- 51.9% -- and so is narrowing the
  three products but summing in double, at 54.6%, which is a useful reminder
  that "round the inputs, keep the sum wide" is not what this does.

  Measured over 114681 pixels in four suites -- random across the whole
  `Le`/`ue`/`ve` space, every `Le` at the white-point chroma, the `Le`
  extremes with saturated chroma, and a full 256x256 chroma grid -- this
  reproduces `344043/344043` = **100.0000%** of the reference's float32 values
  bit-exactly, with no misses in any suite.

  Two formulation choices turned out not to matter at all, which is worth
  knowing because it means the code does not have to be written to match them:
  `exp2((Le+.5)/256 - 64)` and libtiff's `exp(M_LN2/256*(Le+.5) - M_LN2*64)`
  give identical results on every value tested, as do dividing by 410 and
  multiplying by its reciprocal. The float32 narrowing of `X`, `Y` and `Z`
  absorbs both differences.

  The ICC profile that comes out on the float path is a red herring, and
  measuring it was worth the detour only because it is the obvious next
  suspicion. `/tmp/luvp/c.tif` carries a 572-byte APPL `mntr` RGB profile,
  version 4, whose `chad` matrix is

      [  1.047882  0.022919 -0.050201 ]     wtpt  0.964203  1.000000  0.824905
      [  0.029587  0.990479 -0.017059 ]
      [ -0.009232  0.015076  0.751678 ]

  It is attached metadata and is *not* applied: the luma row measured back out
  of the reference's own pixels is exactly CCIR-709's
  `(0.2562008, 0.6782583, 0.0655409)`, not the profile's primaries. Trusting
  the embedded profile instead of the pixels costs the whole decode. Worth
  knowing if this ever has to be reproduced byte for byte, and not worth
  re-deriving.

  ICC v4 tag tables are interleaved -- `sig`, offset, size per entry, twelve
  bytes each -- not v2's two separate arrays. Reading this profile with the v2
  layout produces offsets like 1733843290 out of a 572-byte file, which is how
  the first pass at it failed.

  This is now implemented rather than only characterised. `sgilog_decode()` in
  `main.c` decodes the four run-length coded byte planes per row into packed
  host-order 32-bit words, `logluv32_to_rgb()` does the arithmetic above and
  `logluv_to_float()` re-tags the buffer as three 32-bit IEEE floats, with the
  writer emitting `SampleFormat 3` and the 572-byte float profile. 27 checks in
  `tests/tiffutil-parity.sh` compare it against the reference byte for byte.

  Two things are worth recording because both produce plausible-looking output.
  The word is assembled a byte at a time rather than loaded back out of the
  plane buffer: on a little-endian machine that load reverses the four bytes and
  moves the sign bit to bit 31 of the little end, so every pixel with a large
  `Le` decodes to black. And the big-endian 16-bit sample swap further down
  `load_image` must not run for this photometric at all -- a LogLuv pixel is one
  32-bit word, not a pair of 16-bit samples, so the swap only rearranges bytes
  that were never a sample pair. That one costs 190 wrong bytes in a 16x1
  fixture and nothing at all in the little-endian case, which is what makes it
  easy to miss.
- 8-bit YCbCr Photometric 6 is converted too, and is now reproduced; see the
  YCbCr note above for the model and for the residuals it leaves.
- CIE Lab is reproduced for all three encodings, and the one thing about it that
  no other output shares is that the reference builds a fresh 496-byte
  "Custom Lab Profile" per image rather than copying a template, stamping the
  moment it wrote it into the profile's date field. Two things follow, and both
  were found by measurement rather than by reading the specification.

  The stamp is **local wall clock, not UTC**. ICC says the header date is UTC
  and `tiff_stamp_lab()` used `gmtime_r()` accordingly, which matched the
  reference for as long as the host sat on GMT. Running the reference under
  `TZ=America/New_York` settles it: it stamps the wall clock `TZ` names, four
  hours behind UTC in October. `localtime_r()` is what the reference does, and
  matching the reference is the job, so that is what the code does. A suite run
  in a UTC zone cannot tell the two apart at all -- `gmtime_r` passes every Lab
  case on this host and fails all of them in New York, Kolkata and Kiritimati --
  so the harness now runs Lab cases under those three zones on purpose. Kolkata
  is on a half hour and Kiritimati is a day ahead, so neither can be faked by an
  hour-boundary slip.

  And a multi-page image gets **one profile per page, each stamped
  separately**, not one profile for the file. That matters for any byte
  comparison: zeroing the first page's date and comparing leaves the later ones
  live, which matches when both runs happen to land in the same second and fails
  when they do not. It showed up only under the sanitizer, where a run is slow
  enough to straddle a second boundary -- a flaky failure that a release build
  hides, which is the worst kind to have in a comparison harness.

  The Lab cases (27 shapes across the three encodings, both byte orders, a
  multi-strip layout, a multi-directory file, and the six forced-zone runs)
  check each stamp against the current clock and then zero it, so the other
  484 bytes of every profile are still compared byte for byte. Masking the field
  without checking its value would have been easier and would have hidden exactly
  the bug above.
- The third-party Group 4 extension code is decoded; see "The 2D extension code"
  below for what it does and for the fixtures that pin it.

The reference tool segfaults on `-info`/`-verboseinfo` against a missing or
non-TIFF input. Those cases are excluded from the harness rather than
compared, per the policy above.


## CCITT Group 4 (photometric 0/1, bps 1) — target fully characterised

Differential testing against an independent libtiff encoder
(`/opt/homebrew/bin/tiffcp -c g4`) shows the reference tool's Group 4 output is
**byte-for-byte identical to libtiff's** across 65 randomised cases (widths
1-300, heights 1-50, random bilevel data, both source photometric values), plus
every hand-checked case. This removes the main risk in implementing a Group 4
encoder: a spec-conformant T.6 encoder validated against `tiffcp -c g4` will
match the reference exactly.

Two reference-specific behaviours fall out of that:

- **Source photometric 0 (WhiteIsZero) is inverted before encoding.** The
  reference emits output photometric 1 (MinIsBlack) and the stored G4 stream
  equals libtiff's encoding of the *bitwise-inverted* samples. Source
  photometric 1 passes through unchanged. This was confirmed on 60/60
  randomised cases and on hand-built files.
  Consequence: the 56- vs 57-byte discrepancy seen on a 16x8 fixture is **not**
  a reference quirk. Both fixtures stored identical bits; photometric 0 means
  the opposite visual content, so the two images legitimately encode to
  different lengths. Any harness case pair differing only in photometric must
  expect different G4 output.
- **The reference always writes a single strip**, setting
  `RowsPerStrip = ImageLength`, regardless of the input's strip layout. Its
  output equals libtiff's single-strip encoding even when the input declares
  `RowsPerStrip` of 1, 2, 3, 4, 5 or 8. This is not G4-specific; multi-strip
  input is collapsed to one strip.

Output shape for a 1-bit grayscale source: `BitsPerSample = 1`,
`Photometric = 1`, `Compression = 4`, no `Predictor`, for `-none`, `-lzw` and
`-packbits` alike — the requested output mode is ignored for this depth, the
same way the 1-bit palette case ignores LZW/PackBits and forces Compression 1.

### G4 decoding algorithm, byte-exact (settled by differential testing)

A clean-room decoder now reads a Group 4 strip, so a Group 4 source can be
re-encoded and fed through `-cat`. It is the exact inverse of the encoder
above, and three things about it are not guessable from the spec:

- **A run is a series of makeups and then, always, exactly one terminating
  code.** The reference emits that terminator even when a makeup has already
  covered the run exactly, so a decoder that stops at the makeup leaves the
  line short by it and reads the remainder of the strip as the wrong pixels.
  The makeup tables therefore have to be told apart from the terminating ones;
  the combined table is matched shortest-prefix, so mixing them loses the
  distinction. This was the last decoder bug: it only showed on runs at or
  above 64, and only shifted the *next* row's start.
- **A vertical code carries `a1 - b1`, not `a1 - a0`**, where `b1` is the
  first changing element on the reference line after `a0` and the current
  colour. This is the usual T.4 reading, and the reference tool is a fixpoint
  on its own output, so a strip the reference wrote is decoded by the same
  rule it wrote it with.
- **Each strip starts from the imaginary all-colour-0 line**, so the reference
  line is reset per strip rather than carried across strips, and rows within a
  strip chain normally.

Rows come back packed MSB-first and padded to a byte, which is what the writer
and the reports both want; the same widening as every other sub-byte depth is
*not* applied, because one bit of gray keeps its width.

Validation, all against `/usr/bin/tiffutil` as the oracle:

- `496/496` direct encoder comparisons and `2376/2376` whole-TIFF comparisons
  against the validated Python encoder.
- `3600/3600` cases across flat runs and the long-run makeup boundaries.
- The parity harness decodes reference-generated G4 sources under `-none`,
  `-lzw`, `-packbits`, `-cat` and `-extract`, in both byte orders, over
  makeup-boundary widths (63/64/65, 127/128, 1727/1728/1729, 2560/2561).
- 2400 randomised round trips: random widths up to 3000, heights to 40, both
  polarities, constant and random-run pages, each through `-none`, `-lzw` and
  `-cat`.
- `tests/tiffutil-parity.sh` passes `953/953` against the release, debug and
  ASan/UBSan builds; the four sibling suites are unchanged at `167`, `65`,
  `62` and `15`. Eleven of those checks are the YCbCr conversion, across the
  default coefficients, four explicit sets, a multi-strip layout, several
  directories in one file, a flat-luma image, and both byte orders.

Characterised, and measured rather than assumed: a genuine end of line mark is
the same twelve bits as an end of block mark, and the reference reads it as the
end of the strip rather than as the end of a row. So a third-party strip written
with one after each row decodes to its first row and nothing after it, and the
row the mark follows is kept whole. The `g4eol` fixtures pin that; see "Damaged
G4 strips" below.

The G4 extension code is characterised and decoded (see "The 2D extension code"
below).

### The 2D extension code (settled by differential testing)

`0000001` is the two-dimensional extension code. It has three properties that
together make it unlike every other mode, and each one was checked against
`/usr/bin/tiffutil` rather than inferred:

* **It is exactly seven bits, not a prefix.** The first four bits `0001` are the
  pass code, so a decoder that matches mode prefixes will never reach it. The
  seven-bit pattern is recognised as a whole, before the prefix table is
  consulted at all.
* **It ends the row it is in.** The rest of the row is painted in the colour
  current at that point, and decoding moves on to the next line. It does not
  skip to the end of the strip, and it does not end the strip.
* **The bits after it are read again.** Only the seven bits are consumed, so the
  three bits that happen to follow are the start of the next mode. That is why
  the code's effect on a multi-row image depends on whatever follows it, and
  why the fixtures use a one-row image: the row the extension leaves behind is
  then fixed by the code alone.

The reference build's own decoder (`local/AKCmds-main/tiffutil` pins libtiff at
`b6a17e5`) reaches the same decision through a lookup table rather than a
special case. Its byte accumulator is filled from a bit-reversed byte table, so
`0000001` arrives there as the value 64, and `TIFFFaxMainTable[64]` is
`{S_Ext, 7, 0}`. Its `S_Ext` branch then appends the run from the current
position to the end of the line and jumps to the row-end handler. The patches in
that build do not touch the fax decoder, so the two routes agree by
construction rather than by coincidence.

The extension code is not the only way to end a row early, so the fixtures here
use a one-row image: the row the extension leaves behind is then fixed by the code
alone, rather than by whatever the three bits after it happen to decode to.

### Modes are looked up by seven bits, not scanned as prefixes

The decoder used to read a bit at a time and retest at every length until a mode
code matched, reaching the extension code by a special case in front of that scan.
It now does what the reference does: read seven bits, look them up in one table,
and consume the width the entry carries.

The table is built from the same mode codes already listed here, expanded so that
every one of the 128 seven-bit windows resolves to a mode. Compared entry by entry
against `TIFFFaxMainTable` at the pinned revision, all 128 agree on width and on
which mode they select. The index is little endian in the bit sense -- the first
bit of a code is the low bit of the index -- which is the reference's byte-reversed
accumulator showing through, so the look ahead is turned round before indexing.

Two behaviours follow from the width being carried in the table:

* **A mode code shorter than seven bits leaves the surplus bits to the next code.**
  The scan found the same mode, because the real codes are a prefix code, but this
  is now true by construction rather than as a side effect.
* **An end of block mark spends eleven bits, not twelve.** Its table entry is the
  seven-bit window `0000000` with width seven, and the reference then reads four
  more. The twelfth bit, the flag that marks the mark as the second of a pair, is
  left unread -- which is exactly why the mark's acceptance window had drifted by a
  bit against ours.

The table is complete, so there is no main-table failure path to write. That is
also why the branch a prefix scan needed, for bits that are in no table, is gone:
the reference has no such branch either, and every seven-bit window has an entry.

**One difference remains, and it is not in the mode lookup.** A strip whose bits
are shifted by a few bits -- so that the codes decode, but to the wrong places --
still diverges from the reference partway down the image. The cause is on the other
side of the decoder, not this one. libtiff keeps `b1`, the position in the row
above, as a cursor into that row's run list: it advances it a run at a time in
`CHECK_b1`, steps it forward once after a vertical or pass mode, and steps it
*backwards* one run after a vertical-left mode. This decoder instead recomputes a
single "next changing element" per mode from the finished reference row. The two
agree while the codes are real, and part company once a mode is asked to step left
of a position the reference row has no transition for, which is where the shifted
strips separate. Closing it means carrying the cursor, not changing this table.

**A 2D extension code reaches the same fault from the other end**, and is worth
recording separately because two separate experiments turned out to be one. Splicing
a 2D extension code in front of a real row makes the codes resolve to the wrong
places in exactly the way a shifted strip does. Running that experiment two ways --
a three-bit prefix, and a full ten-bit extension code -- produced results that
differed by exactly one row on every family and agreed on which families diverge,
which is what a longer prefix sliding the decoded image up one row looks like. They
are one experiment, not two, and the twenty-six differing rows one of them reported
were never twenty-six rows of a separate fault.

Against the shipped row, five of the eight extension codes diverge: `000`, `001`,
`010`, `100` and `101`; `011`, `110` and `111` agree. All five are pixel level, which
is what makes them the same debt as the 28 windows above rather than a new one. An
earlier note in this file put the count at four failing prefixes. That was measured
on a different row and is wrong for the shipped one -- and the disagreement is the
point, not an embarrassment to correct: which codes diverge is a property of the row
they are spliced onto, so a list of them is only meaningful next to its fixture.

**How much of the table actually agrees, measured rather than argued.** The parity
suite now decodes all 128 seven-bit windows against the reference row that changes
on every pixel, so the claim "the table is complete" is backed by a sweep instead of
an entry-by-entry reading. 100 of the 128 windows decode row 0 exactly as the
reference does; **28 do not**, and each of those is a pixel-level difference, not a
re-encoding one. Those 28 are named in `tests/tiffutil-parity.sh` and reported as
known divergences, so a 29th fails rather than quietly joining them.

That count is measured against the shipped 16x16 fixture and is specific to it. The
same sweep against a different base image -- a 4-row one, as used while developing
-- produces a *different* set of divergent windows, because what the row decodes into
changes which modes are reached. A list of known divergences is therefore only
meaningful next to the fixture it was measured against, which is why the list lives
in the harness rather than in this file.

A second, unrelated debt turned up in the same work, and it is worth keeping
distinct from the first. Appending a prefix that ends a few bits into a real row
leaves the two decoders disagreeing. Nine reference rows all show it.

**This paragraph was wrong about how it shows, and the correction changed the
size of the debt.** It used to say the two agree on every pixel and that the
converted files differ only in `StripByteCounts` (15 against 10) and in where the
directory sits. They do not agree on the pixels: re-encoding both outputs through
the reference and comparing puts the first difference at the *first byte of image
data* for all nine rows. Differing `StripByteCounts` was a symptom of the pixels
differing, not a milder version of the same thing, and reading it as the milder
thing is what kept this out of the pixel-level count.

Extending the sweep from cuts one to six out to cuts one to sixteen found the rest
of it: **cut twelve diverges too**, and had never been tested. Cuts one to four,
six to eleven and thirteen to sixteen all agree. So the family is two cuts, not
one, and both are pixel level -- eighteen cases, not nine.

That leaves two faults rather than three, because this and the truncation sweep
below are the same fault. Cut five leaves eleven bits and cut twelve leaves four,
and both are lengths the sweep also diverges at, which is a reason to merge them
and not yet a proof.

### Two ways a parity case can pass without testing anything

Both of these were live in the suite while the sweep above was being added, and both
produced green runs. They are recorded because the failure mode is silent -- the
case passes, the suite passes, and the fixture was never decoded.

- **A generated case name that does not match its generator.** `printf '%07b' N`
  pads with *spaces*, not zeros, so building a function name from it produced
  `s_g4w_      0`, which is not a legal function name. No setup ran, both tools then
  failed on a missing input in exactly the same way, and 128 cases reported success
  having decoded nothing. `check` now fails a case whose setup function does not
  exist, and one whose `-none` input the setup did not produce; cases that
  deliberately name a missing file set `SKIP_FIXTURE`.
- **A fixture builder that fails quietly.** `mktiff` crashed on any YCbCr shape with
  explicit coefficients -- the triple is flat-split across the page spec rather than
  being one field -- so five YCbCr conversions were comparing two identical failures.
  `check` ignores a builder's status, which is what let this hide; the fixture
  assertions above are what surfaced it. All eleven YCbCr cases now convert for real.

A third, smaller one: a `-dump` loop named `s_dt_4`, which the generator never
defines -- it stops at `s_dt_3`, and `s_dt_four` belongs to the refused group -- so
that case was also comparing nothing.

**Where the end of block mark lands, which is a third fault of its own.** Sweeping
every truncation of a fixture's reference strip, with the mark appended, finds a
divergence that belongs to neither of the two above. Varying the height is what
settles it:

| fixture | strip | failing truncations |
| --- | --- | --- |
| 16x8 | 447 bits | 72 |
| 16x16 | 827 bits | 72, every one at `k` <= 88 |
| 16x4 | 257 bits | 72 |
| 16x2 | 162 bits | 72 |
| 16x1 | 113 bits | 38 |

**Read the strip before the numbers, because the numbers are misleading on their
own.** It is `payload + EOFB + EOFB`, and at width 16 the payload is 89 bits for
row 0 -- 5.6 bits a pixel for a row of alternating pixels, which is an ordinary
horizontal mode encoding, not a malformed strip. So every `k` at or above 89
leaves that payload untouched and the two decoders agree, and *that agreement is
not evidence of anything*: an intact strip has to decode. Everything below 89 is a
genuinely truncated payload, where the decoders must recover, and here they do not
recover the same way. An earlier version of this note read the `k >= 89` agreement
as "the whole image survives", which inverts the sense of the sweep.

What holds of the failures is worth keeping. The set is identical from one row to
sixteen rows, and at sixteen rows every failing `k` is at or below 88, so the whole
of it is inside row 0 and no later row contributes anything. It also survives on a
*single row* image, where there is no row above for a `b1` cursor to consult, which
settles the attribution: this is not the fault the mode windows turn on. On that
single row the failing truncations are exactly those `k` congruent to 11, 0, 1 or 2
modulo twelve, twelve being the width of the mark.

Whether it is the *same* fault as the cut family above is now measured, on one
fixture and with one set of bits so that only the position varies. `P` is a real
89-bit row from a 16x1 fixture; each variant truncates a row and puts it
somewhere:

| truncated row | where | failing lengths |
| --- | --- | --- |
| `P`, 89 bits | row 0, alone | 72 of 89 |
| `P` | row 1, after a whole `P` | none |
| `P` | row 1, after a reference row | none |
| `G4TC`, 16 bits | row 0, alone | 4: 4, 11, 14, 15 |
| `G4TC` | row 1, after a reference row | 2: 4, 11 |

So it is one fault, and row 0 is where it bites. The same bits that fail as row 0
pass untouched one row down, which rules out the loose version of the claim --
this is not "a truncated row" in general, there is something about row 0 having no
row above it. But the cut family does fail in row 1, at 4 and 11, and those are
two of the lengths it fails at in row 0. So the cut family and this sweep are one
fault, with row 0 being the loud version of it rather than a separate thing.

That is worth stating carefully, because it also narrows where a fix would have to
look: whatever disagrees is not the cursor, since with no row above the cursor
cannot be the input, and it is not position, since the identical bits one row down
are fine.

None of that makes it urgent to fix. Height is irrelevant to it, the abort path it
lands in is already covered by the grid's extension and end of line cases, and it
takes a deliberately mangled strip to reach. It is measured by `EOFPROBE=1`, with
`EOFSHAPE` for the fixture, rather than folded into the gate: marking hundreds of
divergences whose mechanism is not understood would bury the two that are. That is a
judgement about what the debt list is for, and it is the one thing here worth
arguing with.

An earlier note in this file gave this as a pair of narrow boundary lists, the
reference separating at `119..122` where we separate at `121..124`. That did not
reproduce, and it never reached this file -- it is recorded here only because the
difference is instructive: it was almost certainly produced by searching the strip
bytes for the mark and finding byte padding instead, which is the same mistake that
cost time elsewhere in this work. The measurement above reads the mark's position
from the directory rather than searching for it. Sweeping the height, which cost one
flag and four runs, would have caught that at the time.

#### What the EOFPROBE sweep actually draws

The sweep compares two G4 *files*, so until now it only ever said that some byte
in the code stream differs. Reading the pixels instead needs a real decoder, and
the reference will not do it: `tiffutil -none` writes a Group 4 file, tag 259
says 4, so the bytes after the header are packed codes rather than pixels and
walking them by hand reports the wrong thing. Pillow is used instead, and
`EOFDUMP=<k>` prints the row.

At width 16 the payload is 89 bits: six horizontal pairs of twelve
bits each -- mode `001`, white run `000111`, black run `010`, painting two pixels
-- then a vertical tail of seven, six, three and one bits. Truncating the payload
to `k` bits and appending the mark gives the painted prefix of row 0 below. `0` is
white, and the two right-hand columns are what each tool drew before the first
line fix described at the end of this section; ours now matches the reference.

    k            0-14   15-22   23-26   27-34   35-38   39-46   47-50
    reference    empty  2 px   empty   4 px   empty   6 px   empty
    ours         empty  2 px    2 px   4 px    4 px   6 px    6 px

    k           51-58  59-62  63-70  71-88   89
    reference   8 px  empty  10 px  empty   16 px
    ours         8 px  10 px  10 px  16 px   16 px

Two things follow that the byte comparison had hidden. The reference's row is
*not* a prefix of what it paints for larger `k`: at 23 it has given back the two
pixels it drew at 15, and at 71 the ten from 63. Painted length is not monotone in
`k` at all, which is why the family looks like a list of unrelated boundary bugs
from the outside. And the two agree exactly on the runs the table marks clean,
so the debt list is a list of places the pictures differ, not merely the files.

The regression *is* statable per row, but not per line: it is the strip's first
line that differs, and only that one. A mid-row mark leaves the first line at the
all colour 0 it starts from, discarding every code the line had already coded;
every other line pads in the colour it had reached, as the note below describes.
The two are easy to confuse because a one-row sweep only ever exercises the first
case. Repeating the same bits one row down -- prefixing a vertical code that
closes row 0 so the code under test lands on row 1 -- agrees with the reference at
every width run tried, while the identical bits on row 0 do not.

    sweep (width 16, heights 1,2,3,4,5,8,16, prefixes empty/V0/VL1/pair x1..3,
    and 0-5 bits of tail before the mark)  divergent fixtures
      before this fix    180 / 252
      after this fix       0 / 252

So `g4_decode_line` takes a `firstrow` flag, and its mid-row mark branch paints
the whole line in colour 0 when set. Effects, all three configurations:

    full gate            PASS=1659 FAIL=0 known-divergent=51  (unchanged debt)
    EOFPROBE 16x4 sweep  72 -> 34 failures, a strict subset: all 38 one-row
                           cases fixed, none newly broken

The whole-line reset is still the wrong fix, and for the same reason this one is
right: it applies the first line's rule to every line, and costs 181 cases across
the gate, `g4grid`, `g4win` and `g4cut` alike.

What the fix does not reach is a second and separate mechanism, which is why 34
sweep cases remain. Those are no longer row 0: row 0 now matches at every `k`, and
the disagreement has moved to row 1. Row 0 reads its mode as horizontal and then looks
for a white run where the mark already is; we find no run code at all, burn thirteen
bits failing, and start row 1 at bit 16, where everything reads as zero and the line
comes out empty. The reference's row 1 is `...............#` -- a vertical pad of one
pixel at x=15 -- so it reads the leftovers differently.

**What actually happens: the mark is spent as eleven bits, not thirteen.** The offset
reading was wrong, and it was wrong because of a defect in the instrument rather than in
the model. `/tmp/luvp/trace.c` passed `firstrow = 1` for *every* candidate row, so any
start bit that reached the mark blanked its whole row through the first row branch and
could never match anything. No bit appeared to work, which is what made the difference
look structural. With `firstrow` defaulted to 0 -- the value every row other than the
strip's first has to use -- the same search finds `B = 1` and `B = 16` producing
`...............#` for `H+EOL+V0+VL1`. That rerun is what produced the rule below.

The rule: **a seven zero window reached inside a run lookup is the start of an end of
block mark.** No run code in either colour's table begins with seven zeros -- the longest
leading zero run any of them has is six -- so a seven zero window inside a run can only
be the start of a mark. The reference spends on it the same bits it
spends on a mark read as a mode: the seven it looked at plus the four after them. The
mark therefore ends the line *and leaves the cursor one bit inside itself*. On
`H+EOL+VL1` that puts row 1 at bit 14, which is the mark's own final `1` -- read as a
`V0`, which runs to the width and returns without painting -- so row 2 starts on the
`010` after it, and the mark at the end of that row is what puts the pixel at x=15.
That is the whole row accounting, and it is why the pixel lands on row 2 rather than
row 1: no row is shifted, eleven bits are spent where we spend thirteen.

The repro is much smaller than the sweep, which is the useful consequence. Row 0 is a
horizontal code, then a mark, then a tail; width 16, any height:

| strip | reference |
| --- | --- |
| `H+EOL` | every row empty |
| `H+EOL+VL1` | **row 2** = `...............#` |
| `H+EOL+VL1+V0` | row 2 = `...............#` (we already match this one) |
| `H+EOL+VL1+VL1` | every row empty |
| `H+EOL+V0+VL1` | **row 3** = `...............#` |
| `H+EOL+00+VL1` | every row empty |
| `H+EOL+000+VL1` | row 2 = `..............##` |

`H+EOL+VL1` is eighteen bits of input and reproduces a real divergence on its own, with
no truncation sweep involved. `H+EOL+000+VL1` painting *two* pixels says the
reference is not merely shifting where a row lands either -- it is painting a
different span of the row. `H+EOL+VL1+V0` already matching us says the tail
participates in the rule.

The eleven bit rule accounts for every row of that table, but the *paints* in it need
saying, because they are not what the code names suggest. In this table `010` carries
d = -1 and `011` carries d = +1 -- the mirror of T.4, where `010` is VL1 and `011` is
VR1 -- and the reference decodes it that way. The single pixel rows above are the proof,
because nothing else in them can produce a pixel at x=15.

`H+EOL+VL1` is not a vertical pad stepping to x=15. On row 2 the `010` moves a0 from 16
down to 15 and paints [0,15) white, flips the colour to 1, and the mark that follows pads
[15,16) with it; that padding is the black pixel. `H+EOL+000+VL1` is the same shape one
code out: `000010` carries d = -2 and moves a0 to 14, and the mark pads [14,16) in
colour 1, which is the two pixels. Nothing in either row paints those pixels the way a
vertical mode would; the mark does, every time, in whatever colour the line had reached.

That also accounts for the `VL1` count without a parity rule. Each `010` moves a0 to 15
and flips the colour without advancing any further, so `m` of them leave the line in
colour `m mod 2` and the mark pads x15 with that: odd `m` paints it, even `m` does not.
And `H+EOL+V0+VL1` differs from `H+EOL+VL1` only in that the extra `V0` spends a row of
its own, moving the `010` from row 2 to row 3.

Two of the fixture names above are therefore misnomers worth flagging, because reading
them against T.4 sends you looking for a rule that is not there: the `VL1` in
`H+EOL+VL1` is a code with d = -1 here, and `000010` in `H+EOL+000+VL1` is a code with
d = -2. The mode table has been cross checked entry by entry against what the reference
does with each code, and it is self consistent: `vert[]` is indexed by `d + 3` and the
code recorded against each `d` is the one the reference uses. Worth knowing for a
separate reason, too -- a table mistake of this shape is invisible to any fixture whose
strips were encoded by the same table that decodes them, which is most of them, so only
hand written strips like these can catch it.

`/tmp/luvp/candU` implements exactly that and reproduces the reference on **every row
of all fourteen cases measured against it** -- the seven above plus `H+EOL`,
`H+EOL+V0*n` for n = 0..3, `H+EOL+VL1*m` for m = 1..4, `H+EOL+V0*2+VL1`,
`H+EOL+H(w1b1)`, `H+EOL+pass` and `H+V0+EOL`: 84 rows, 84 correct. On the faithful
16x4 sweep it takes the failures from 34 to 7:

    sweep 16x4    34 -> 7   (k = 9, 10, 18, 30, 42, 54, 66)
    full gate     FAIL 0 -> 16

It is still not in the tree, because of those 16, and they are one family: the mark
landing *after* a terminating run code of the pair (`H` + `00110101` + mark). That is
`g4cut ... -4` across all nine reference rows, `g4win struct 1110010`, and six `g4src`
cases. There the reference paints the row *after* the mark solid black, or
`1111111111111110` when a `VL1` follows, and neither eleven nor thirteen bits produces
that. Restricting detection to the first run of the pair only (`/tmp/luvp/candV`)
restores all 16 and re-breaks 10 sweep cases, 17 divergent, so which run of the pair the
mark falls in is not the discriminator either. That family is the next thing to measure.

Five repairs were built before the eleven bit rule against the offset reading, and all
five cost more than they fix, so none is in the tree:

| candidate | idea | gate |
| --- | --- | --- |
| D | a first line mark ends the line, not the strip, so row 1 continues | 46/258 sweep divergent, up from 34 |
| E | charge the four bit tail to a run lookup that matched the mark | no effect; the run table holds no such entry, the lookup simply fails |
| F | as E, plus restoring the cursor after a failed run read | 1 fixed, 35 regressed |
| G | a mark in a run position ends the line instead of failing the pair | 2 fixed, 16 regressed, across `g4src`, `g4win` and `g4cut` |
| R, S | force row 1's start bit, or `a0`/`colour`, to whatever reproduces the oracle | no case fixed that the plain fix had not already fixed |
| U | a seven zero window in a run is a mark, spent as 7 + 4, ending the line but not the strip | 7/258 sweep divergent, 16 gate failures |
| V | as U, but only in the first run of a pair | 17/258, gate restored |

G is the closest of these to U and the comparison is the useful part: G got the "the
line ends" half right and left the cursor thirteen bits on, so it fixed 2 and cost 16;
U keeps the eleven bit accounting and gets 34 down to 7. The two halves were being
guessed separately and they had to be measured together.

Two lessons, and the second one is the one that cost the most. A rule measured on the
sweep alone is not yet a rule: every candidate in the table above looks right on the
258 truncation fixtures and is wrong somewhere in the ordinary cases, so the gate has
to be the arbiter and not the sweep. And a rule measured by comparing two *different*
strips is not a measurement at all -- that is what the withdrawn offset arithmetic was,
and it produced a confident, specific, wrong answer with tidy percentages attached. The
minimal repro table is worth more than that arithmetic because every row of it is a
strip whose contents can be written down and checked.

A third lesson, which is the one this section has now cost twice: **an instrument that
returns "no match" for every input is not evidence that no match exists.**
`trace.c` reporting no bit position could produce the row is only information if the
instrument can produce a match at all, and passing `firstrow = 1` to every row made it
blank each one through the branch documented above. The trace harness has been fixed to
take `firstrow` as an argument defaulting to 0; the check that it works is that it now
reports hits, and the check that it is measuring the right thing is that those hits land
on rows the reference actually produces.

### Damaged G4 strips

There is no separate "bad code word" path to recover along, which is worth
recording because it is easy to assume there is. All 128 entries of libtiff's
`TIFFFaxMainTable` are valid, so its seven bit main table lookup always returns a
state and never falls through to `unexpected("MainTable")`. Its
`unexpected("WhiteTable")`, `unexpected("BlackTable")` and `unexpected("VL")`
branches are reachable, but only from strips no encoder produces.

What is reachable, and what the reference actually does, is recovery:

* **An end of block mark ends the strip, not just one row.** Rows past it are
  left at the all colour 0 line the decoder starts from, which is what a strip
  carrying the mark up front decodes to. libtiff consumes that mark while
  finishing the row before it, so the next row would otherwise start on the wrong
  bit.
* **A mark partway down a line finishes that line in the colour it had reached.**
  This is the other half of the same rule and it is easy to get wrong, because the
  two cases look alike from the outside. A mark that arrives when the line is
  already full is consumed and nothing is painted, which is the case above. A mark
  that arrives partway along a line paints the rest of that line in the colour the
  line was in when the mark turned up -- so a line that had reached black keeps its
  tail black -- and then stops. Reading it as "the tail is left alone" instead
  leaves the tail at the starting colour, which is white for every line, and that
  is visibly wrong the moment a vertical mode has flipped the colour before the
  mark.
* **An explicit end of line mark ends the strip too, and the row it follows is
  kept whole.** A Group 4 strip needs no such mark, because every row runs to
  the full width on its own, so the reference never writes one; some senders do
  anyway. libtiff reads it as the end of the strip rather than as the end of a
  row, which means such a strip decodes to its first row and nothing after it,
  and not to a strip with a mark wedged somewhere inside it. The row the mark
  follows survives intact, so the behaviour is worth pinning rather than
  assuming from the mark's name.
* **Bits that are in no table end the row they are in.** The rest of the row
  keeps the colour current at that point and decoding moves on to the next row.
  The reference says nothing about it, because its seven bit table always has a
  state and so it never runs out of codes; the two quiet paths agree without a
  message being involved.
* **Neither of them fails the conversion.** A decoder that treats an unreadable
  code as fatal turns both cases into an error the reference does not raise.

The `g4bits` fixtures cover the first two of these by rebuilding a strip out of
a bit string: an end of block mark in front of the rows, bits that are in no
code in place of the rows, and bits that are in no code after the end of block
mark. The `g4eol` fixtures cover the third by putting a mark after the first
row, which takes the width of that row from compressing the same image cut to
one row — the same first row, because the line above it is the imaginary all
colour 0 line either way. Finding that width needs the end of block mark taken
off before the trailing zeros are stripped: the mark is twelve bits ending in a
one, so a strip of trailing zeros stops at its own last one and leaves the
eleven zeros in front of it behind. The remaining difference is the width of the
window in which the reference accepts a mark that starts a few bits off a row
boundary, which is a property of how far its table lookup can see rather than of
the recovery itself.

### G4 encoding algorithm, byte-exact (settled by differential testing)

A clean-room encoder was built and validated against `/usr/bin/tiffutil` on
every case below, so the algorithm below is settled rather than provisional.

* **Framing.** The strip is `payload + EOFB*2 + zero fill to a byte boundary`,
  where `EOFB = 000000000001`. No fill bits and no byte alignment inside the
  payload. Verified exact on 150/150 sub-64 cases.
* **Start of *every* line.** `a0 = -1` with `a0`'s colour white — an imaginary
  white pixel to the left of the line. This is *not* special to the first line;
  getting it wrong for lines 2..n is the single easiest mistake to make, and it
  silently still matches for one-row images.
* **`b1` is colour-filtered.** `b1` is the first changing element on the
  reference line to the right of `a0` *whose colour is opposite to a0's
  colour*, falling back to the line width. Using the first changing element of
  any colour instead is wrong and costs ~70% of cases.
* **`b2`** is the next changing element after `b1` of any colour.
* **Pass mode is required.** When `b2 < a1`, emit `0001` and set `a0 = b2`
  keeping a0's colour unchanged. Clamping `b2` up to `a1` instead is wrong;
  with everything else correct it accounts for the last 46/364 failures.
* **Mode choice.** `d = a1 - b1`; if `-3 <= d <= 3` emit the vertical code and
  set `a0 = a1`, flipping the colour. Otherwise emit horizontal mode `001`
  followed by two run codes: `a0..a1` in a0's colour and `a1..a2` in the
  opposite colour, where `a2` is the next change on the *coding* line past `a1`
  whose colour differs from `a1`'s run. Then `a0 = a2`. `b2` plays no part in
  the horizontal runs.
* **A horizontal pair that overruns the line is not a line.** When the two runs
  add up to more than the line width, the reference ends the line where the pair
  starts and fills the rest of it in the colour it had reached, so none of the
  pair survives and the line comes out blank when that colour is white. Clamping
  the pair to the width instead keeps a tail of it and paints a line where the
  reference leaves none. A pair that ends exactly on the width is still good, so
  the test is strictly greater than, not greater or equal. Verified over every
  pair of white and black runs up to 13 and 6 that crosses the boundary.
* **An end-of-line code found mid-row finishes the line off rather than
  abandoning it.** When a mode word turns out to be an end-of-line partway down
  a line, the reference pads the rest of the row in the colour the line had
  reached and stops, so a mark that lands partway down a line does not throw
  that colour away. Only rows whose mark stops before they ever start stay at
  the all-zero line they began from, and those are handled before any mode is
  read. An extension code ends the line the same way. The colour really is the
  one reached rather than simply white: checked against a structured reference
  row, where a vertical step turns the line black before the end-of-line is
  found, the remainder comes out black in both. Both fills are load-bearing --
  leaving the row alone instead costs 45 of the 104 grid cases -- so they are
  the opposite of what an overshooting vertical does.
* **Codes that are in no mode leave the row alone.** When the bits at the
  changing element match no mode, or a horizontal mode's first run cannot be
  read, the reference stops the line there and does not touch the rest of the
  row -- it does not fill the remainder in with the colour the line had
  reached. Filling it in scores the same on every grid case, because all of them
  end while still white, so the two are only separable against a reference row
  that goes on to reach black. There it is worth three more of the 128 single
  mode words, and it costs nothing on the malformed-strip cases. This is the same
  rule as an overshooting vertical, and the opposite of an end-of-line or
  extension mark, which really do fill.
* **A vertical mode that cannot step back ends the line.** A vertical-left code
  that would land left of where the line has already painted has nowhere to go:
  the reference ends the line there and moves on to the next one. Clamping the
  landing point back to the start of the run instead paints the whole rest of the
  line one colour and flips it, turning a line the reference leaves white into a
  solid one. The step only ever runs left, so this cannot catch a forward code.
  This is reachable without any damage -- an all-black reference line has no
  changing element at all, so every vertical-left code against it lands here.
  The rest of the line is **not** filled in with the colour the line had reached:
  the reference leaves the unpainted remainder of the row as it stands. That was
  originally implemented as a fill, and it passed every grid case only because
  each of those cases reached the end while still white. Removing the fill and
  holding the grid at 104/104 cut one more malformed row, and a differential scan
  of all 128 single mode words against a structured reference row agreed on 113
  rather than 111. An end-of-line code found mid-row and an extension code *do*
  fill the remainder in, and dropping those fills instead costs 45 grid cases, so
  the two are genuinely different and only the overshoot leaves the row alone.
* **Runs.** A run is coded as makeups plus a terminating code, and **the
  terminating code is emitted even when a makeup consumes the run exactly** —
  a zero-length run still has code `term[0]`. Dropping it breaks every run that
  is an exact multiple of 64.
* **Makeup selection.** Below 1792 take the largest multiple of 64 not
  exceeding the run; at 1792 and above cap the makeup at 2560 and finish with
  further codes.

### The black makeup table is NOT the standard one

`/usr/bin/tiffutil` uses the standard T.4 shared makeup table for **white**
runs (all 40 entries 64..2560 verified), and the standard extended entries for
**black** runs at 1792..2560, but black runs of 64..1728 use a different set
of longer codes. These were extracted empirically and are reproduced verbatim in
`src/tiffutil/g4.c`:

| run | code | | run | code |
|---|---|---|---|---|
| 64 | `0000001111` | | 896 | `0000001110010` |
| 128 | `000011001000` | | 960 | `0000001110011` |
| 192 | `000011001001` | | 1024 | `0000001110100` |
| 256 | `000001011011` | | 1088 | `0000001110101` |
| 320 | `000000110011` | | 1152 | `0000001110110` |
| 384 | `000000110100` | | 1216 | `0000001110111` |
| 448 | `000000110101` | | 1280 | `0000001010010` |
| 512 | `0000001101100` | | 1344 | `0000001010011` |
| 576 | `0000001101101` | | 1408 | `0000001010100` |
| 640 | `0000001001010` | | 1472 | `0000001010101` |
| 704 | `0000001001011` | | 1536 | `0000001011010` |
| 768 | `0000001001100` | | 1600 | `0000001011011` |
| 832 | `0000001001101` | | 1664 | `0000001100100` |
| | | | 1728 | `0000001100101` |

White makeups are the standard table; black 1792..2560 are the standard
extended entries (`00000001000` .. `000000011111`).

### Validation totals for the reference algorithm

364/364 exhaustive (width 2..4 x height 1..2), 420/420 randomised widths
< 64, 783/783 byte-exact over widths 1..5000 including all-black, all-white and
long-run patterns, and 320/320 byte-exact on a fresh randomised seed with
heights up to 8. No mismatches remain.

### The C port, and the two mistakes it caught

`src/tiffutil/g4.c` implements the algorithm above and is byte-exact
against `/usr/bin/tiffutil`, encoder and decoder both. Porting the encoder
from the Python model caught two errors
that the Python model had made easy to avoid, both of which produce plausible
looking output on one-row images:

- **The vertical code table must be indexed by `d + 3` with `V(-1) = 010`.**
  Writing the seven codes out in the order `0, +1, +2, +3, -1, -2, -3` and then
  indexing that array by `d + 3` silently shifts every negative `d` by two
  places, so `V(-1)` becomes `000010` and `V(-2)` becomes `0000010`. The
  positive half stays correct, which is why the error survives on any image
  whose only transitions are to the right of the reference.
- **`b1` may only be a genuine changing element.** Scanning for the first
  *pixel* whose colour differs from `a0`'s returns the interior of a run, not
  its start: on an all-white reference line the first `b1` must fall back to the
  line width, because there is no changing element at all. Returning the first
  differing pixel instead shifts `b1` one position left, and with it `d` and the
  mode. This is the same distinction as the colour filter above, one level
  further in: the filter picks the right *transition*, the scan has to happen
  over transitions in the first place.

Measured against the reference after the fix:

- 496/496 direct strip comparisons against the Python model, widths 1..4000,
  heights 1..5, random rows.
- 2376/2376 whole-file byte comparisons, covering both photometrics, three
  input `RowsPerStrip` layouts and all three requested output modes.
- 3600/3600 whole-file byte comparisons over flat rows and long single runs at
  widths either side of every makeup boundary (63/64, 127/128, 255/256,
  511/512, 1023/1024, 1727/1728/1792, 2559/2560/2561).
- 333/333 in `tests/tiffutil-parity.sh` at the point the encoder landed, which
  then carried 51 one-bit-gray cases. The suite has grown since, to 953 checks
  including a report sweep, and passes in full against the release, debug and
  ASan/UBSan builds.

The decoder added a third error of the same kind, worth recording because its
symptom points somewhere other than its cause:

- **A run always ends in a terminating code, even when a makeup has already
  covered the run exactly.** Matching one run code and stopping left the line
  short by that terminator, so the *next* row was decoded from the wrong bit
  and came out with a couple of leading pixels wrong -- nothing wrong at all
  with the row being decoded. It appeared only on runs of 64 or more, and only
  on the row *after* the long run, so the trace pointed at vertical mode and
  the reference line rather than at run parsing. The fix is to mark table
  entries as makeup or terminating and accumulate until a terminator arrives.
