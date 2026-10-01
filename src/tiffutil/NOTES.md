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
    unreadable/missing input                       0* "TIFFOpen: F: No such file or directory." +
                                                      "Error: Can't open F. ..."

* `tiffutil -info nosuch.tif` **segfaults** (rc 139) after printing a correct
  error. So does `-info` against a non-TIFF, an empty file, or a directory --
  every unreadable input, in fact, because the image source is null when the
  report is formatted. `-dump` and the write operations survive the same
  inputs. A clean-room reimplementation must not reproduce a crash; the
  message and a non-zero status are reproduced instead. This is a deliberate,
  documented divergence, and those four cases are excluded from the harness
  rather than compared.

Other details:

- The default output name is `out.tiff`, not a derived name from the input.
- Success prints `N image(s) written to F.` (singular for 1) to **stderr**, not
  stdout. Nothing is ever written to stdout on a successful write. Found by
  tests/tiffutil-parity.sh, which compares the two streams separately; every
  earlier ad-hoc check folded them together with `2>&1` and so missed it.
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

## NewSubfileType is a set of flags, and a value can be turned down

Two `-info` lines that are easy to get wrong, both measured by sweeping values
rather than by reading the spec.

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

**A value can also be refused**, which is a separate and later complaint than
an unknown field. ResolutionUnit (296) accepts 1, 2 and 3 and names them
`none`, `pixels/inch` and `pixels/cm` -- note `pixels/cm`, not
`centimeters/inch`. Anything else costs it the line entirely, prints no
substitute, and draws this on stderr instead:

    _TIFFVSetField: F: Bad value 0 for "ResolutionUnit" tag.

That comes *after* every unknown-field warning the same directory drew, not
among them, and `-dump` prints neither the line nor the complaint. Six other
tags refuse values the same way and are not handled; their measured ranges are
in the open list below.

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

Still open, and therefore *not* pinned down by anything in this file:

- Six of the seven tags whose value can be turned down still are not handled
  here; only ResolutionUnit is. Measured accepted ranges, found by sweeping
  values 0-16 through `-info`:

      266 FillOrder      1-2     274 Orientation    1-8
      296 ResolutionUnit 1-3     338 ExtraSamples  0-2
      339 SampleFormat   1-6     32996 DataType    0-3
      32998 TileDepth    1-16+   (upper bound not searched)

  FillOrder and Orientation need only what ResolutionUnit now has: drop the
  line for a value outside the range and warn. Each also has wording to fix --
  Orientation says `left`/`right` where the reference tool says `lhs`/`rhs`, and
  both print a line where the reference tool prints none.

  The other four refuse harder: a bad ExtraSamples, SampleFormat, DataType or
  TileDepth makes the file *fail to open*, printing `Error: Can't open F.
  Either it isn't readable, it isn't a TIFF file, or there are unrecognized
  tags; try tiffutil -dump for more info.` after the warning and reporting no
  directory at all. Two more measured details they leave behind: SampleFormat 3
  is `IEEE floating point` here against `floating point`, 4 is `void` against
  `undefined`, and an unnamed accepted value like 5 is printed as `5 (0x5)`
  rather than as `unknown`; and DataType (32996) makes the reference tool print
  a whole Sample Format line from a tag that is otherwise ignored here. None of
  this is pinned by the harness today.
- 16-bit LogLuv is converted rather than copied, and its conversion is still
  unknown. 8-bit YCbCr Photometric 6 is converted too, and is now reproduced;
  see the YCbCr note above for the model and for the residuals it leaves.
- The Lab profile carries a build timestamp, so a byte comparison only holds
  within a single second. Anything comparing Lab output needs synchronised
  clocks or a deterministic time injection.
- A third-party Group 4 stream using the extension code is not decoded.

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

Not characterised: a third-party strip using the G4 extension code (`0000001`)
or explicit EOLs between rows. The mode table handles H, pass, V0-V+3 and
EOFB, and a genuine EOL is the same twelve bits as an EOFB so a row still ends
correctly; the extension code is rejected as an undecodable row.

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
