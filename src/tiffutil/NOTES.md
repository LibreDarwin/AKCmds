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

Verified identical across 4x4 and 64x64 gray, 8- and 16-bit, RGB and RGBA
sources, and across -none/-lzw/-packbits. So the profile is one of two fixed
byte strings selected by the output colour space, not a synthesis algorithm
that has to be rederived. Both are embedded verbatim in
src/tiffutil/tiffutil.m.

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

## Text modes are deterministic and directly comparable

`-info` and `-verboseinfo` print per-directory summaries; `-verboseinfo` adds
a `Strips (Offset, ByteCount):` block that `-info` omits. `-dump` prints the
raw IFD. These are pure text and are the strongest parity signal for
diagnostics and for the operations that rewrite files.

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
  error. A clean-room reimplementation must not reproduce a crash; the message
  and a non-zero status are reproduced instead. This is a deliberate,
  documented divergence.

Other details:

- The default output name is `out.tiff`, not a derived name from the input.
- Success prints `N image(s) written to F.` (singular for 1) to stdout.
- An existing output file is overwritten silently.
- `-extract 0` succeeds and writes a file.
- `-cat` warns `Warning: Sizes of concatenated images are not the same; this
  will lead to problems in choosing the appropriate image in some cases.` and
  then prints a per-image geometry line. `-catnosizecheck` and
  `-cathidpicheck` select the other policies.
