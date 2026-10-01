#!/bin/bash
# Byte-parity harness: /usr/bin/tiffutil (oracle) vs our clean-room tiffutil.
# Compares stdout, stderr, exit status, the set of files produced, and the bytes
# of every file in the case directory.  Self-contained: no state outside its
# temp directory and no fixture files checked into the tree.
#
#   ./tests/tiffutil-parity.sh
#   make tiffutil-parity
#
# MY defaults to the Makefile release binary; override with MY=... .  A relative
# MY is resolved against the project root, so the Makefile can pass a short path
# (neither make variant allows $(shell)/$(CURDIR)).  ORACLE can be overridden to
# compare against a different reference build.
#
# Requires bash (the null-safe read loops and local -d '' are bash-only) and
# python3, which builds the TIFF fixtures.  Building the binary already needs a
# full toolchain, so python3 is present wherever this can run.
#
# The -cat family decodes every input and re-encodes it through the writer, so
# it inherits each source's shape rather than moving data around; the cases at
# the end cover the three spellings and their warnings.

ORACLE=${ORACLE:-/usr/bin/tiffutil}
HERE=$(cd "$(dirname "$0")" && pwd)
PROJROOT=$(cd "$HERE/.." && pwd)
MY=${MY:-build/release/tiffutil}
case "$MY" in
    /*) ;;
    *) MY="$PROJROOT/$MY" ;;
esac

if [ ! -x "$MY" ]; then
    echo "missing binary: $MY" >&2
    echo "build it with: make -C $PROJROOT" >&2
    exit 1
fi
if [ ! -x "$ORACLE" ]; then
    echo "missing oracle: $ORACLE" >&2
    exit 1
fi
if ! command -v python3 >/dev/null 2>&1; then
    echo "python3 is required to build the TIFF fixtures" >&2
    exit 1
fi

ROOT=$(mktemp -d "${TMPDIR:-/tmp}/tiffutil-parity.XXXXXX") || exit 1
trap 'rm -rf "$ROOT"' EXIT

PASS=0
FAIL=0
FAILED=""

# Fixture builder: writes one TIFF, one directory per page spec, so the file
# layout, the strip boundaries and every out-of-line value are ours to choose.
#
#   mktiff <path> <endian> <w,h,bps,spp,photo[,comp[,pred[,rps[,nex[,xval[,fill]]]]]]>...
#
# A trailing fill value replaces the pattern with one constant sample, which is
# how a case lays down a run long enough to want a makeup code.
#
# endian is "little" or "big".  Defaults: no compression, no predictor, one
# strip per page.  BitsPerSample, XResolution, YResolution and ColorMap are
# always written out of line, and the strip tables out of line when there is
# more than one strip, so the reader's offset handling is covered rather than
# bypassed by every field happening to fit in the value slot.
mktiff() {
    python3 - "$@" <<'PYEOF'
import struct, sys

TYPESIZE = {1: 1, 2: 1, 3: 2, 4: 4, 5: 8, 6: 1, 7: 1, 8: 2, 9: 4,
            10: 8, 11: 4, 12: 8, 13: 4, 16: 8, 17: 8, 18: 8}


def packbits(data):
    """Run-length encoder.  A valid encoding is a valid encoding: the tools
    only ever decode it, and no output of ours depends on this byte layout."""
    out = bytearray()
    i, n = 0, len(data)
    while i < n:
        run = 1
        while i + run < n and data[i + run] == data[i] and run < 128:
            run += 1
        if run >= 3:
            out.append(256 - (run - 1))
            out.append(data[i])
            i += run
            continue
        start, lit = i, 0
        while i < n and lit < 128:
            if i + 2 < n and data[i] == data[i + 1] == data[i + 2]:
                break
            i += 1
            lit += 1
        out.append(lit - 1)
        out += data[start:start + lit]
    return bytes(out)


path, endian = sys.argv[1], sys.argv[2]
e = '<' if endian == 'little' else '>'
bo = endian                       # int.to_bytes spells it the long way
magic = b'II' if endian == 'little' else b'MM'
pages = [p.split(',') for p in sys.argv[3:]]

body = bytearray(magic + struct.pack(e + 'HI', 42, 0))
ifds = []

for spec in pages:
    w, h = int(spec[0]), int(spec[1])
    bps, spp, photo = int(spec[2]), int(spec[3]), int(spec[4])
    comp = int(spec[5]) if len(spec) > 5 and spec[5] else 1
    pred = int(spec[6]) if len(spec) > 6 and spec[6] else 1
    rps = int(spec[7]) if len(spec) > 7 and spec[7] else h
    if rps > h:
        rps = h
    nex = int(spec[8]) if len(spec) > 8 and spec[8] else (1 if spp == 4 else 0)
    xval = int(spec[9]) if len(spec) > 9 and spec[9] else 2
    fill = int(spec[10]) if len(spec) > 10 and spec[10] else None
    # Photometric 6 is the one case whose samples are not stored per pixel, so
    # the coefficient tag is how a fixture selects a set other than the BT.601
    # default.  It is "kr,kg,kb" or empty for absent, which exercises the
    # default path rather than the tag being read.
    coeffs = spec[11].split(',') if len(spec) > 11 and spec[11] else None

    mask = (1 << bps) - 1
    spr = w * spp                        # samples per row
    if photo == 6 and bps == 8 and spp == 3:
        # Six bytes per 2x2 block: four luma then one chroma pair, blocks in
        # row-major order.  A strip therefore starts on a block row, so the
        # rows per strip is forced even; a fixture asking for an odd one would
        # otherwise be describing a layout the tag cannot express.
        if rps % 2:
            rps += 1
        bpl = (w // 2) * 6
        px = bytearray()
        for by in range(h // 2):
            for bx in range(w // 2):
                for p in range(4):
                    v = fill if fill is not None else \
                        (bx * 37 + by * 53 + p * 11) & 0xFF
                    px.append(v & 0xFF)
                for k in (4, 5):
                    v = fill if fill is not None else (bx * 29 + by * 61 + k * 7) & 0xFF
                    px.append(v & 0xFF)
    elif bps < 8:
        # Samples narrower than a byte are really bit packed, with each row
        # starting on a byte boundary.  Writing a whole byte per sample here
        # would make the fixture claim a depth its bytes do not honour, and
        # the reference tool would then be comparing garbage.
        bpl = (spr * bps + 7) // 8
        px = bytearray()
        for y in range(h):
            row = bytearray(bpl)
            bit = 0
            for x in range(spr):
                v = fill if fill is not None else x * 7 + y * 13
                row[bit // 8] |= (v & mask) << (8 - bps - bit % 8)
                bit += bps
            px += row
    else:
        step = bps // 8
        bpl = spr * step
        px = bytearray()
        for y in range(h):
            for x in range(spr):
                v = fill if fill is not None else x * 7 + y * 13
                px += (v & mask).to_bytes(step, bo)

    if pred == 2 and bps == 8:
        for y in range(h):
            base = y * bpl
            for i in range(spp, spr):
                px[base + i] = (px[base + i] - px[base + i - spp]) & 0xFF

    # A photometric 6 buffer holds two image rows per buffer row, so strip
    # boundaries have to be converted from image rows before they index into
    # it.  rps is forced even above, so the division is exact.
    rowdiv = 2 if photo == 6 and bps == 8 and spp == 3 else 1
    nstrips = (h + rps - 1) // rps
    chunks = []
    for s in range(nstrips):
        y0, rows = s * rps, min(rps, h - s * rps)
        raw = bytes(px[(y0 // rowdiv) * bpl:((y0 + rows) // rowdiv) * bpl])
        if comp == 32773:
            chunks.append(packbits(raw))
        elif comp == 1:
            chunks.append(raw)
        else:
            raise SystemExit('unsupported source compression %d' % comp)

    strip_off, strip_bc = [], []
    for c in chunks:
        strip_off.append(len(body))
        strip_bc.append(len(c))
        body += c

    def put(vals, typ):
        off = len(body)
        if typ == 3:
            body.extend(b''.join(struct.pack(e + 'H', v) for v in vals))
        elif typ == 4:
            body.extend(b''.join(struct.pack(e + 'I', v) for v in vals))
        elif typ == 5:
            body.extend(b''.join(struct.pack(e + 'II', a, b) for a, b in vals))
        else:
            body.extend(bytes(vals))
        return off

    bps_off = put([bps] * spp, 3) if spp > 2 else 0
    xres_off = put([(72, 1)], 5)
    yres_off = put([(72, 1)], 5)
    cm_off = 0
    if photo == 3:
        cm_off = put([0] * (3 * (1 << bps)), 3)
    xa_off = put([xval] * nex, 3) if nex else 0
    so_off = put(strip_off, 4) if nstrips > 1 else 0
    sb_off = put(strip_bc, 4) if nstrips > 1 else 0

    def entry(tag, typ, cnt, vals, off):
        n = TYPESIZE[typ] * cnt
        if n > 4:
            return struct.pack(e + 'HHII', tag, typ, cnt, off)
        fmt = {1: 'B', 3: 'H', 4: 'I'}[typ]
        b = b''.join(struct.pack(e + fmt, v) for v in vals)
        return struct.pack(e + 'HHI', tag, typ, cnt) + b + b'\0' * (4 - n)

    entries = [
        entry(256, 4, 1, [w], 0),
        entry(257, 4, 1, [h], 0),
        entry(258, 3, spp, [bps] * spp, bps_off) if spp > 2
        else entry(258, 3, 1, [bps], 0),
        entry(259, 3, 1, [comp], 0),
        entry(262, 3, 1, [photo], 0),
        entry(273, 4, nstrips, strip_off, so_off) if nstrips > 1
        else entry(273, 4, 1, strip_off, 0),
        entry(277, 3, 1, [spp], 0),
        entry(278, 4, 1, [rps], 0),
        entry(279, 4, nstrips, strip_bc, sb_off) if nstrips > 1
        else entry(279, 4, 1, strip_bc, 0),
        entry(282, 5, 1, [(72, 1)], xres_off),
        entry(283, 5, 1, [(72, 1)], yres_off),
        entry(284, 3, 1, [1], 0),
    ]
    if pred != 1:
        entries.append(entry(317, 3, 1, [pred], 0))
    if photo == 6:
        # Tag 530 states the 2x2 the samples are actually stored in.  The
        # reference reads it and ignores it, but a fixture that omits it is
        # not a faithful YCbCr file, so it is always written.
        entries.append(entry(530, 3, 2, [2, 2], 0))
        if coeffs:
            ycbcr_off = put([(int(coeffs[0]), 1000), (int(coeffs[1]), 1000),
                             (int(coeffs[2]), 1000)], 5)
            entries.append(entry(529, 5, 3, [], ycbcr_off))
    if xa_off:
        entries.append(entry(338, 3, nex, [xval] * nex, xa_off))
    if cm_off:
        entries.append(entry(320, 3, 3 * (1 << bps), [0] * 0, cm_off))
    entries.sort(key=lambda t: struct.unpack(e + 'H', t[:2])[0])

    ifd_off = len(body)
    body += struct.pack(e + 'H', len(entries)) + b''.join(entries) \
        + struct.pack(e + 'I', 0)
    ifds.append((ifd_off, len(entries)))

for i, (off, n) in enumerate(ifds):
    nxt = ifds[i + 1][0] if i + 1 < len(ifds) else 0
    struct.pack_into(e + 'I', body, off + 2 + 12 * n, nxt)
struct.pack_into(e + 'I', body, 4, ifds[0][0])
open(path, 'wb').write(bytes(body))
PYEOF
}

# check <label> <endian> <pages> <setup> -- <args...>
#   endian : "little" or "big"
#   pages  : space-separated page specs, or "-" for no fixture
#   setup  : function run with the case directory as $1 to add further files
#            (a compressed source, a bad source, ...; default s_none)
# The arguments after -- are handed to both tools, run inside the case dir.
check() {
    local label="$1" endian="$2" pages="$3" setup="$4"
    shift 4
    shift   # the literal --
    local o="$ROOT/o" m="$ROOT/m"
    rm -rf "$o" "$m"
    mkdir -p "$o" "$m"

    if [ "$pages" != "-" ]; then
        # The spec list has to word-split into one argument per page.
        # shellcheck disable=SC2086
        mktiff "$o/i.tiff" "$endian" $pages
        # shellcheck disable=SC2086
        mktiff "$m/i.tiff" "$endian" $pages
    fi
    $setup "$o"
    $setup "$m"

    ( cd "$o" && "$ORACLE" "$@" >o.out 2>o.err )
    local ro=$?
    ( cd "$m" && "$MY" "$@" >o.out 2>o.err )
    local rm=$?

    # Both tools name themselves in a few messages; normalize so the comparison
    # is about behavior rather than the install path.
    sed -e "s|$ORACLE|PROG|g" "$o/o.err" >"$o/e2" 2>/dev/null &&
        mv "$o/e2" "$o/o.err"
    sed -e "s|$MY|PROG|g" "$m/o.err" >"$m/e2" 2>/dev/null &&
        mv "$m/e2" "$m/o.err"

    local why=""
    [ "$ro" = "$rm" ] || why="exit($ro/$rm)"
    cmp -s "$o/o.out" "$m/o.out" || why="$why stdout"
    cmp -s "$o/o.err" "$m/o.err" || why="$why stderr"

    local fl fm
    fl=$( cd "$o" && find . -type f ! -name 'o.out' ! -name 'o.err' | sort )
    fm=$( cd "$m" && find . -type f ! -name 'o.out' ! -name 'o.err' | sort )
    [ "$fl" = "$fm" ] || why="$why files[$fl|$fm]"

    # Compare the bytes of every fixture and every produced file.  Null-safe so
    # that names containing spaces are handled.
    while IFS= read -r -d '' f; do
        cmp -s "$o/$f" "$m/$f" || why="$why bytes:$f"
    done < <( cd "$o" && find . -type f ! -name 'o.out' ! -name 'o.err' \
        -print0 )

    if [ -z "$why" ]; then
        PASS=$((PASS + 1))
        if [ -n "$VERBOSE" ]; then
            printf '  ok   %-34s exit=%s  %s\n' "$label" "$ro" \
                "$(printf '%s' "$fl" | tr '\n' ' ')"
        fi
    else
        FAIL=$((FAIL + 1))
        FAILED="$FAILED [$label]"
        printf '  FAIL %-34s %s\n' "$label" "$why"
        printf '       oracle stderr: %s\n' "$(head -1 "$o/o.err")"
        printf '       ours   stderr: %s\n' "$(head -1 "$m/o.err")"
    fi
}

# Extra fixtures.  Each takes the case directory as $1.
s_none()  { :; }
s_text()  { echo "not an image" > "$1/junk.tiff"; }
s_empty() { : > "$1/junk.tiff"; }
s_dir()   { mkdir -p "$1/adir"; }
s_b_same() { mktiff "$1/b.tiff" little "$G8SAME"; }
s_b_diff() { mktiff "$1/b.tiff" little "$G8DIFF"; }
# A second input, plus a name that is not a TIFF at all and one that is not
# there, for the way the concatenate family reports bad inputs.
s_cat_mix() {
    mktiff "$1/b.tiff" little "$G8DIFF"
    echo "not an image" > "$1/junk.tiff"
}
# The compressed sources are produced by the reference tool itself rather than
# by a second implementation in this script: it is the authority on the
# compressed layout, and using it to manufacture a fixture does not make the
# comparison below any less independent.
s_lzw()  { "$ORACLE" -lzw       "$1/i.tiff" -out "$1/c.tiff" >/dev/null 2>&1; }
s_packbits() { "$ORACLE" -packbits "$1/i.tiff" -out "$1/c.tiff" >/dev/null 2>&1; }
# A G4 source.  Asking for -none on a one bit gray image is what makes the
# reference compress it, so the fixture is genuine facsimile data to decode.
s_g4()   { "$ORACLE" -none     "$1/i.tiff" -out "$1/c.tiff" >/dev/null 2>&1; }
s_pred() {
    "$ORACLE" -lzw -out /dev/null "$1/i.tiff" >/dev/null 2>&1
    :
}

# Fixture builder for the unknown-field warning: an uncompressed 8-bit gray
# directory carrying tags the reference tool has no name for.  One
# semicolon-separated group per directory, and the tags inside a group go in
# exactly as written, so a case can pin that the warning follows the order the
# entries sit in rather than the order of the tag numbers.
#
#   mkextra <path> <endian> <tags>[;<tags>...]
mkextra() {
    python3 - "$@" <<'PYEOF'
import struct, sys

path, endian = sys.argv[1], sys.argv[2]
e = '<' if endian == 'little' else '>'
magic = b'II' if endian == 'little' else b'MM'

body = bytearray(magic + struct.pack(e + 'HI', 42, 0))
ifds = []
for group in sys.argv[3].split(';'):
    if not group:
        continue
    w = h = 4
    data = bytearray(w * h)
    so = len(body)
    body += data
    sc = len(data)
    entries = [(256, 4, 1, [w]), (257, 4, 1, [h]), (258, 3, 1, [8]),
               (259, 3, 1, [1]), (262, 3, 1, [1]), (273, 4, 1, [so]),
               (277, 3, 1, [1]), (278, 3, 1, [h]), (279, 4, 1, [sc]),
               (284, 3, 1, [1])]
    for tag in group.split(','):
        entries.append((int(tag), 3, 1, [1]))
    entries.sort(key=lambda x: x[0])
    fmt = {3: 'H', 4: 'I'}
    ifd = len(body)
    body += struct.pack(e + 'H', len(entries))
    for tag, typ, cnt, vals in entries:
        raw = b''.join(struct.pack(e + fmt[typ], v) for v in vals)
        body += struct.pack(e + 'HHI', tag, typ, cnt) + raw
        body += b'\0' * (4 - len(raw))
    body += struct.pack(e + 'I', 0)
    ifds.append((ifd, len(entries)))

struct.pack_into(e + 'I', body, 4, ifds[0][0])
for i in range(len(ifds) - 1):
    off, n = ifds[i]
    struct.pack_into(e + 'I', body, off + 2 + 12 * n, ifds[i + 1][0])

with open(path, 'wb') as f:
    f.write(bytes(body))
PYEOF
}

s_unk()    { mkextra "$1/i.tiff" little 347; }
s_unk_hi() { mkextra "$1/i.tiff" little 65000; }
s_unk_two(){ mkextra "$1/i.tiff" little 347,65000,65001; }
# Descending on purpose: the warning has to follow the directory, not the
# tag numbers.
s_unk_rev(){ mkextra "$1/i.tiff" little 65001,65000,347; }
s_unk_dir(){ mkextra "$1/i.tiff" little '347;65000'; }
s_unk_be() { mkextra "$1/i.tiff" big 347; }
# Tag 333 is a name the reference tool has, but asking for one InkName hangs
# it for minutes, so it is left out of every fixture here.  254 and 305 are
# left out too: the first adds a Subfile Type line and the second a complaint
# about a missing null terminator, neither of which this case is about.  What
# is left are names the tool has and never mentions.
s_unk_known() { mkextra "$1/i.tiff" little 300,434,700; }

# Single-page shapes.  w,h,bps,spp,photo.
G8="16,16,8,1,1"          # 8-bit gray
G8W="64,48,8,1,1"         # 8-bit gray, both dimensions even
G8O="15,13,8,1,1"         # 8-bit gray, both dimensions odd
RGB="32,32,8,3,2"         # 8-bit RGB
RGBA="24,20,8,4,2"        # 8-bit RGBA
G16="16,16,16,1,1"        # 16-bit gray
RGB16="16,16,16,3,2"      # 16-bit RGB
PAL="8,8,8,1,3"           # 8-bit palette
MULTI="16,16,8,1,1 32,32,8,3,2 12,8,8,4,2"
BIG="128,96,8,3,2"
# Multi-strip: the second field is rows-per-strip.
STRIPED="40,40,8,1,1,1,1,8"
STRIPED3="40,40,8,3,2,1,1,16"
# Photometric 6, which is converted rather than copied.  The trailing field is
# the coefficient tag as "kr,kg,kb", empty for the BT.601 default.  Widths hold
# an even number of 2x2 blocks: an odd count makes the reference read past the
# end of a block row, so it cannot be used to pin the conversion down.
YCBCR="16,16,8,3,6"
YCBCRW="64,48,8,3,6"                                  # both dimensions even
YCBCRS="40,40,8,3,6,1,1,8"                            # multi-strip
YCBCR601="16,16,8,3,6,1,1,16,0,2,,299,587,114"        # the default, stated
YCBCR709="16,16,8,3,6,1,1,16,0,2,,299,338,100"        # BT.709
YCBCRFULL="32,32,8,3,6,1,1,32,0,2,,229,587,114"       # full range
YCBCRF16="16,16,8,3,6,1,1,16,0,2,,100,500,400"        # another legal set
YCBCRMULTI="16,16,8,3,6 32,32,8,3,6 8,8,8,3,6"
YCBCRFLAT="32,32,8,3,6,1,1,32,0,2,7,,299,587,114"     # every luma the same
# Surplus alpha channels, then the extra-sample value: the reference tool
# keeps one of them, so these are how the collapse gets pinned down.
XA2="16,8,8,5,2,1,1,8,2"        # RGB plus two extrasamples
XA2A="16,8,8,5,2,1,1,8,2,1"     # the same, associated alpha
XA3="16,8,8,6,2,1,1,8,3"        # RGB plus three
GAXA="16,8,8,3,1,1,1,8,2"       # gray plus two
NOXA="16,8,8,4,2"               # RGB, surplus samples, no extrasamples tag
GNOXA="16,8,8,3,1"              # gray, surplus samples, no extrasamples tag
G1XA="16,8,8,2,1,1,1,8,1"       # gray with exactly one spare sample
PALXA="16,8,8,2,3,1,1,8,1"      # palette with a spare sample
G16XA="16,8,16,3,1,1,1,8,1"     # 16-bit gray plus two
RGB16XA="16,8,16,5,2,1,1,8,2"   # 16-bit RGB plus two
BWXA="16,8,8,3,0,1,1,8,1"       # white-is-zero plus two
XAMULTI="16,8,8,5,2,1,1,8,2 12,6,8,6,2,1,1,6,3"
# The hidpi rules are about the whole run, so the pair lives in one file here.
HIDPI="64,48,8,1,1 32,24,8,1,1"        # twice the width and the height
HIDPIW="64,48,8,1,1 32,48,8,1,1"       # only the width
HIDPIH="64,48,8,1,1 64,24,8,1,1"       # only the height
HIDPI3="64,48,8,1,1 32,24,8,1,1 32,24,8,1,1"
# A second input for the concatenate cases, with and without a size change.
G8SAME="64,48,8,1,1"
G8DIFF="32,24,8,1,1"

# Samples narrower than a byte.  These arrive bit packed, are handed on one
# sample to a byte, and so come out at eight bits: a palette's indices are
# widened as they stand, while a real value is stretched over the whole range
# by repeating its bits.  One bit of gray is left out on purpose, because the
# reference tool repacks and compresses that one instead of widening it.
BW1PAL="16,8,1,1,3"       # one-bit palette
B2G="16,8,2,1,1"         # two-bit gray
B2G0="16,8,2,1,0"        # two-bit white-is-zero
B4G="16,8,4,1,1"         # four-bit gray
B2RGB="16,8,2,3,2"       # two-bit RGB
B4RGB="16,8,4,3,2"       # four-bit RGB
B2PAL="16,8,2,1,3"       # two-bit palette
B4PAL="16,8,4,1,3"       # four-bit palette
B1RGB="16,8,1,3,2"       # one-bit RGB
B4GA="5,8,4,1,1"         # a width that is not a multiple of the packing

# One bit of gray is the odd one out: the reference tool does not widen it to
# eight bits like the other narrow depths, it keeps it at one bit and G4
# compresses it, so these pin down what it writes rather than what it widens
# to.  The trailing fill is a constant sample, which is how a run long enough
# to want a makeup code gets laid down; the widths sit either side of the
# 63/64 and 1728/2560 boundaries the makeup tables turn on.
B1G="16,16,1,1,1"                    # one-bit gray, min-is-black
B1G0="16,16,1,1,0"                   # one-bit gray, white-is-zero
B1GN="15,13,1,1,1"                   # neither dimension a multiple of eight
B1GL="200,3,1,1,1"                   # a long run, wanting a makeup
B1G0L="200,3,1,1,0"                  # the same the other way round
B1G63="63,2,1,1,1,1,1,2,0,2,1"       # just under the first makeup
B1G64="64,2,1,1,1,1,1,2,0,2,1"       # at it
B1G65="65,2,1,1,1,1,1,2,0,2,1"       # just over it
B1GALL1="100,2,1,1,1,1,1,2,0,2,1"    # one flat black row
B1GALL0="100,2,1,1,1,1,1,2,0,2,0"    # one flat white row
B1GW63="63,2,1,1,0,1,1,2,0,2,0"      # white-is-zero, flat
B1GW2000="2000,2,1,1,1,1,1,2,0,2,1"  # past the last short makeup
B1GW2000W="2000,2,1,1,0,1,1,2,0,2,0" # and the white-is-zero counterpart
B1GSTRIP="40,40,1,1,1,1,1,8"         # one bit of gray over several strips
B1GSTRIP0="40,40,1,1,0,1,1,8"

echo "tiffutil parity: $ORACLE vs $MY"

# --- uncompressed rewrite, one shape at a time -------------------------------
for op in -none -lzw -packbits; do
    for shape in "$G8" "$G8W" "$G8O" "$RGB" "$RGBA" "$G16" "$RGB16" "$PAL" "$BIG"; do
        check "$op $(printf '%s' "$shape" | tr ',' '-')" \
            little "$shape" s_none -- "$op" i.tiff -out o.tiff
    done
done

# --- big-endian sources ------------------------------------------------------
for op in -none -lzw -packbits; do
    for shape in "$G8W" "$RGB" "$G16" "$BIG"; do
        check "$op BE $(printf '%s' "$shape" | tr ',' '-')" \
            big "$shape" s_none -- "$op" i.tiff -out o.tiff
    done
done

# --- bit packed samples ------------------------------------------------------
for op in -none -lzw -packbits; do
    for shape in "$BW1PAL" "$B2G" "$B2G0" "$B4G" "$B2RGB" "$B4RGB" "$B2PAL" \
                 "$B4PAL" "$B1RGB" "$B4GA"; do
        check "$op $(printf '%s' "$shape" | tr ',' '-')" \
            little "$shape" s_none -- "$op" i.tiff -out o.tiff
    done
    check "$op BE 16-8-2-1-1"  big "$B2G"   s_none -- "$op" i.tiff -out o.tiff
    check "$op BE 16-8-4-3-2"  big "$B4RGB" s_none -- "$op" i.tiff -out o.tiff
    check "$op BE 16-8-1-1-3"  big "$BW1PAL" s_none -- "$op" i.tiff -out o.tiff
done

# --- YCbCr, which is converted rather than copied -----------------------------
# Only -none: the reference tool does not lay down a converted YCbCr image as
# compressed, so a compressed fixture would be comparing its failure rather
# than the conversion.  The shapes cover the default coefficients, several
# explicit ones, a multi-strip layout, several directories in one file, and a
# flat-luma image that puts the clamp on every channel.
for shape in "$YCBCR" "$YCBCRW" "$YCBCRS" "$YCBCR601" "$YCBCR709" "$YCBCRFULL" \
             "$YCBCRF16" "$YCBCRMULTI" "$YCBCRFLAT"; do
    check "-none ycbcr $(printf '%s' "$shape" | tr ',' '-')" \
        little "$shape" s_none -- -none i.tiff -out o.tiff
done
for shape in "$YCBCRW" "$YCBCR709"; do
    check "-none ycbcr BE $(printf '%s' "$shape" | tr ',' '-')" \
        big "$shape" s_none -- -none i.tiff -out o.tiff
done

# --- one bit of gray, which the reference tool G4-compresses -----------------
# The requested compression is overridden for these: the reference picks G4 on
# its own, so asking for packbits still has to come out as the same G4.
for op in -none -lzw -packbits; do
    for shape in "$B1G" "$B1G0" "$B1GN" "$B1GL" "$B1G0L" "$B1G63" "$B1G64" \
                 "$B1G65" "$B1GALL0" "$B1GALL1" "$B1GW63" "$B1GW2000" \
                 "$B1GW2000W" "$B1GSTRIP" "$B1GSTRIP0"; do
        check "$op $(printf '%s' "$shape" | tr ',' '-')" \
            little "$shape" s_none -- "$op" i.tiff -out o.tiff
    done
    check "$op BE 16-16-1-1-1" big "$B1G"  s_none -- "$op" i.tiff -out o.tiff
    check "$op BE 16-16-1-1-0" big "$B1G0" s_none -- "$op" i.tiff -out o.tiff
done

# --- G4 sources, which have to be decoded before anything else happens ------
# The fixture is a G4 file the reference wrote, so every case here runs the
# decoder and then, because the output of a one bit gray rewrite is G4 again,
# the encoder as well.
for op in -none -lzw -packbits; do
    for shape in "$B1G" "$B1G0" "$B1GN" "$B1GL" "$B1G0L" "$B1G63" "$B1G64" \
                 "$B1G65" "$B1GALL0" "$B1GALL1" "$B1GW63" "$B1GW2000" \
                 "$B1GW2000W" "$B1GSTRIP" "$B1GSTRIP0"; do
        check "g4src $op $(printf '%s' "$shape" | tr ',' '-')" \
            little "$shape" s_g4 -- "$op" c.tiff -out o.tiff
    done
    check "g4src $op BE 16-16-1-1-1" big "$B1G"  s_g4 -- "$op" c.tiff -out o.tiff
    check "g4src $op BE 16-16-1-1-0" big "$B1G0" s_g4 -- "$op" c.tiff -out o.tiff
done
for op in -info -verboseinfo; do
    check "g4src $op" little "$B1G" s_g4 -- "$op" c.tiff
done
check "g4src -dump"        little "$B1G"  s_g4 -- -dump c.tiff
# An unnamed field draws a warning on stderr, but only where the directory is
# read through the TIFF library: -info and -verboseinfo report it, -dump walks
# the raw IFD itself and says nothing, and neither does a conversion.
for op in -info -verboseinfo; do
    check "unknown tag $op"        little "$G8W" s_unk     -- "$op" i.tiff
    check "unknown tag $op high"   little "$G8W" s_unk_hi  -- "$op" i.tiff
    check "unknown tag $op BE"     big    "$G8W" s_unk_be  -- "$op" i.tiff
    check "unknown tags $op"       little "$G8W" s_unk_two -- "$op" i.tiff
    check "unknown tags $op rev"   little "$G8W" s_unk_rev -- "$op" i.tiff
    check "unknown tag $op dirs"   little "$G8W" s_unk_dir -- "$op" i.tiff
    check "named tags $op quiet"   little "$G8W" s_unk_known -- "$op" i.tiff
done
check "unknown tag -dump"      little "$G8W" s_unk     -- -dump i.tiff
check "unknown tag convert"    little "$G8W" s_unk     -- -none i.tiff -out o.tiff
# Predictor is named only for the codecs that predict.  Under an uncompressed
# directory it is as unnamed as anything else; under LZW it is not.
check "predictor -info uncompressed" little "$G8W" s_none -- -info i.tiff
check "predictor -info lzw"    little "$G8W" s_lzw      -- -info c.tiff
check "g4src -extract 0"   little "$B1G"  s_g4 -- -extract 0 c.tiff -out o.tiff
check "g4src -extract end" little "$MULTI" s_g4 -- -extract 3 c.tiff -out o.tiff
# The same narrow depth override applies to a source that is not facsimile to
# begin with, so an extract of plain one bit gray comes out as Group 4 too.
check "extract 0 one-bit gray" little "$B1G" s_none -- -extract 0 i.tiff -out o.tiff
# Concatenation decodes each input, so these go through the decoder too.
for shape in "$B1G" "$B1G0" "$B1GL" "$B1GSTRIP"; do
    check "g4src -cat $(printf '%s' "$shape" | tr ',' '-')" \
        little "$shape" s_g4 -- -cat c.tiff c.tiff -out o.tiff
    check "g4src -catnosizecheck $(printf '%s' "$shape" | tr ',' '-')" \
        little "$shape" s_g4 -- -catnosizecheck c.tiff c.tiff -out o.tiff
    check "g4src -cathidpicheck $(printf '%s' "$shape" | tr ',' '-')" \
        little "$shape" s_g4 -- -cathidpicheck c.tiff c.tiff -out o.tiff
done

# --- multiple strips ---------------------------------------------------------
for op in -none -lzw -packbits; do
    check "$op multi-strip gray" little "$STRIPED"  s_none -- "$op" i.tiff -out o.tiff
    check "$op multi-strip rgb"  little "$STRIPED3" s_none -- "$op" i.tiff -out o.tiff
done

# --- multiple directories ----------------------------------------------------
for op in -none -lzw -packbits; do
    check "$op multi-dir" little "$MULTI" s_none -- "$op" i.tiff -out o.tiff
done

# --- surplus alpha channels --------------------------------------------------
for op in -none -lzw -packbits; do
    for shape in "$XA2" "$XA2A" "$XA3" "$GAXA" "$G1XA" "$NOXA" "$GNOXA" \
                 "$PALXA" "$G16XA" "$RGB16XA" "$BWXA"; do
        check "$op $(printf '%s' "$shape" | tr ',' '-')" \
            little "$shape" s_none -- "$op" i.tiff -out o.tiff
    done
    check "$op multi-dir surplus alpha" little "$XAMULTI" s_none \
        -- "$op" i.tiff -out o.tiff
    check "$op BE surplus alpha" big "$XA2" s_none -- "$op" i.tiff -out o.tiff
    check "$op BE gray surplus" big "$GAXA" s_none -- "$op" i.tiff -out o.tiff
done

# --- separated, which carries its own ICC profile ------------------------------
CMYK="16,8,8,5,5"                 # four colour samples plus a spare
CMYKXA="16,8,8,6,5,1,1,8,1"       # four colour samples plus alpha
CMYKNOXA="16,8,8,6,5"             # the same, with no extrasamples tag
CMYK16XA="16,8,16,6,5,1,1,8,1"    # 16-bit, which must not empty out
for op in -none -lzw -packbits; do
    for shape in "$CMYK" "$CMYKXA" "$CMYKNOXA" "$CMYK16XA"; do
        check "$op $(printf '%s' "$shape" | tr ',' '-')" \
            little "$shape" s_none -- "$op" i.tiff -out o.tiff
    done
done

# --- pixels too narrow for the output format ----------------------------------
# Four samples is the classic separated TIFF and the reference tool makes
# nothing of it: an empty file, still reported as one image written.
CMYK4="16,8,8,4,5"                # separated, no spare sample at all
CMYK4XA="16,8,8,4,5,1,1,8,1"      # separated with an extrasamples tag
CMYK2="16,8,8,2,5"                # nowhere near four colour samples
RGB2="16,8,8,2,2"                 # RGB cannot be made of two samples
YCBCRXA="16,8,8,4,6"              # YCbCr is only ever read at three
MIXED="16,16,8,1,1 32,32,8,4,5"   # a usable directory after an unusable one
MIXED2="32,32,8,4,5 16,16,8,1,1"  # ... and before one
for op in -none -lzw -packbits; do
    for shape in "$CMYK4" "$CMYK4XA" "$CMYK2" "$RGB2" "$YCBCRXA"; do
        check "$op $(printf '%s' "$shape" | tr ',' '-')" \
            little "$shape" s_none -- "$op" i.tiff -out o.tiff
    done
    for shape in "$MIXED" "$MIXED2"; do
        check "$op $(printf '%s' "$shape" | tr ',' '-')" \
            little "$shape" s_none -- "$op" i.tiff -out o.tiff
    done
done

# --- compressed sources (made by the reference tool) -------------------------
for op in -none -lzw -packbits; do
    check "$op <- lzw source"       little "$G8W"  s_lzw      -- "$op" c.tiff -out o.tiff
    check "$op <- packbits source" little "$RGB"  s_packbits -- "$op" c.tiff -out o.tiff
done

# --- extract -----------------------------------------------------------------
for n in 0 1 2; do
    check "extract $n of 3"  little "$MULTI" s_none -- -extract $n i.tiff -out o.tiff
done
check "extract 0 of lzw src"  little "$G8W" s_lzw      -- -extract 0 c.tiff -out o.tiff
check "extract 0 of pack src" little "$RGB" s_packbits -- -extract 0 c.tiff -out o.tiff
check "extract 0 gray"        little "$G8"  s_none     -- -extract 0 i.tiff -out o.tiff
check "extract 0 rgba"        little "$RGBA" s_none    -- -extract 0 i.tiff -out o.tiff
check "extract 0 palette"     little "$PAL" s_none     -- -extract 0 i.tiff -out o.tiff
check "extract past end"      little "$MULTI" s_none   -- -extract 3 i.tiff -out o.tiff
check "extract missing number" little "$G8"  s_none    -- -extract

# --- reports -----------------------------------------------------------------
for op in -info -verboseinfo -dump; do
    for shape in "$G8" "$G8W" "$RGB" "$RGBA" "$G16" "$RGB16" "$PAL" "$BIG"; do
        check "$op $(printf '%s' "$shape" | tr ',' '-')" \
            little "$shape" s_none -- "$op" i.tiff
    done
    check "$op BE"      big   "$RGB"  s_none -- "$op" i.tiff
    check "$op multi"   little "$MULTI" s_none -- "$op" i.tiff
    check "$op lzw src" little "$G8W"  s_lzw  -- "$op" c.tiff
    check "$op stripes" little "$STRIPED" s_none -- "$op" i.tiff
done

# --- default output naming ---------------------------------------------------
check "default out.tiff" little "$G8W" s_none -- -none i.tiff
check "default out.tiff lzw" little "$G8W" s_none -- -lzw i.tiff
check "default out.tiff extract" little "$MULTI" s_none -- -extract 1 i.tiff

# --- argument handling -------------------------------------------------------
check "no args"          little "$G8W" s_none --
check "no operation"     little "$G8W" s_none -- i.tiff
check "two operations"   little "$G8W" s_none -- -none -lzw i.tiff -out o.tiff
check "two inputs"       little "$G8W" s_none -- -none i.tiff j.tiff -out o.tiff
check "two inputs lzw"   little "$G8W" s_none -- -lzw i.tiff j.tiff -out o.tiff
check "unknown flag"     little "$G8W" s_none -- -bogus i.tiff
check "-out missing arg" little "$G8W" s_none -- -none i.tiff -out
check "operation after out" little "$G8W" s_none -- i.tiff -out o.tiff -lzw
check "out before input" little "$G8W" s_none -- -none -out o.tiff i.tiff

# --- error paths -------------------------------------------------------------
# -info is deliberately absent from the unreadable-input cases below.  The
# reference tool dereferences a null image source and dies with SIGSEGV (rc 139)
# on every one of them -- a missing file, a non-TIFF, an empty file, a
# directory.  Reproducing a crash is not a parity target, so those four are not
# compared at all.  -dump and the write operations survive the same inputs and
# are compared normally.
check "missing source"      little "$G8W" s_none  -- -none nope.tiff
check "missing source dump" little "$G8W" s_none  -- -dump nope.tiff
check "not an image"        little "$G8W" s_text  -- -none junk.tiff
check "empty file"          little "$G8W" s_empty -- -none junk.tiff
check "empty file dump"     little "$G8W" s_empty -- -dump junk.tiff
check "directory as source" little "$G8W" s_dir   -- -none adir

# --- cat family --------------------------------------------------------------
# Each spelling over a plain source, and with the default output name.
for op in -cat -catnosizecheck -cathidpicheck; do
    check "$op gray"        little "$G8W" s_none -- "$op" i.tiff -out o.tiff
    check "$op out.tiff"    little "$G8W" s_none -- "$op" i.tiff
done

# A concatenate re-encodes every image, so each source shape has to come out
# the same way it would from a rewrite.
for op in -cat -catnosizecheck -cathidpicheck; do
    for shape in "$RGB" "$RGBA" "$PAL" "$G16" "$G8O" "$XA2" "$NOXA" \
                 "$B1G" "$B1G0" "$B1GL" "$B1GSTRIP"; do
        check "$op $(printf '%s' "$shape" | tr ',' '-')" \
            little "$shape" s_none -- "$op" i.tiff -out o.tiff
    done
    check "$op multi-dir" little "$MULTI" s_none -- "$op" i.tiff -out o.tiff
done

# The hidpi rules are about the whole run: one image, or a pair that is twice
# as wide and twice as tall.
check "-cathidpicheck one"         little "$G8W"     s_none \
    -- -cathidpicheck i.tiff -out o.tiff
check "-cathidpicheck pair"        little "$HIDPI"   s_none \
    -- -cathidpicheck i.tiff -out o.tiff
check "-cathidpicheck half width"  little "$HIDPIW"  s_none \
    -- -cathidpicheck i.tiff -out o.tiff
check "-cathidpicheck half height" little "$HIDPIH"  s_none \
    -- -cathidpicheck i.tiff -out o.tiff
check "-cathidpicheck three"       little "$HIDPI3"  s_none \
    -- -cathidpicheck i.tiff -out o.tiff

# Two files: matching point sizes are quiet, different ones get the report.
check "-cat same sizes"      little "$G8W" s_b_same -- -cat i.tiff b.tiff -out o.tiff
check "-cat other sizes"     little "$G8W" s_b_diff -- -cat i.tiff b.tiff -out o.tiff
check "-catnosizecheck two"  little "$G8W" s_b_diff -- -catnosizecheck i.tiff b.tiff -out o.tiff
check "-cathidpicheck two"   little "$G8W" s_b_diff -- -cathidpicheck i.tiff b.tiff -out o.tiff
check "-cathidpicheck two files" little "$G8W" s_b_diff -- -cathidpicheck i.tiff b.tiff -out o.tiff

# A name that is not a TIFF is passed over and the rest still goes through;
# a name that is not there at all means nothing is written.
check "-cat junk only"       little "$G8W" s_text    -- -cat junk.tiff -out o.tiff
check "-cat junk first"      little "$G8W" s_cat_mix  -- -cat junk.tiff i.tiff b.tiff -out o.tiff
check "-cat junk last"       little "$G8W" s_cat_mix  -- -cat i.tiff b.tiff junk.tiff -out o.tiff
check "-cat junk middle"     little "$G8W" s_cat_mix  -- -cat i.tiff junk.tiff b.tiff -out o.tiff
check "-cat missing only"    little "$G8W" s_none     -- -cat nope.tiff -out o.tiff
check "-cat missing last"    little "$G8W" s_b_diff   -- -cat i.tiff b.tiff nope.tiff -out o.tiff
check "-cat missing first"   little "$G8W" s_b_diff   -- -cat nope.tiff b.tiff -out o.tiff
check "-cat directory"       little "$G8W" s_dir      -- -cat adir -out o.tiff
check "-cat empty file"      little "$G8W" s_empty    -- -cat i.tiff junk.tiff -out o.tiff
check "-cathidpicheck junk"  little "$G8W" s_text    -- -cathidpicheck junk.tiff i.tiff -out o.tiff

# --- report sweep ------------------------------------------------------------
# The reports are read off a directory rather than re-encoded, so a shape the
# write paths never produce can still disagree.  Every fixture shape is swept
# through all three reports, in both byte orders, and again on a compressed
# rewrite and on a Group 4 source so the strip tables are read as well.
for endian in little big; do
    for shape in "$G8" "$G8W" "$G8O" "$RGB" "$RGBA" "$G16" "$RGB16" "$PAL" \
                 "$BIG" "$STRIPED" "$STRIPED3" "$B1G" "$B1G0" "$B1GN" \
                 "$B1GL" "$B1G0L" "$B1G63" "$B1G64" "$B1G65" "$B1GALL1" \
                 "$B1GALL0" "$BW1PAL" "$B2G" "$B2G0" "$B4G" "$B2RGB" \
                 "$B4RGB" "$B2PAL" "$B4PAL" "$B1RGB" "$B4GA" "$XA2" "$XA2A" \
                 "$XA3" "$GAXA" "$NOXA" "$GNOXA" "$G1XA" "$PALXA" "$G16XA" \
                 "$RGB16XA" "$BWXA"; do
        for setup in s_none s_g4; do
            for op in -info -verboseinfo -dump; do
                check "sweep $endian $(printf '%s' "$shape" | tr ',' '-') $setup $op" \
                    "$endian" "$shape" "$setup" -- "$op" i.tiff
            done
        done
    done
    for pages in "$MULTI" "$HIDPI" "$HIDPI3" "$XAMULTI"; do
        for op in -info -verboseinfo -dump; do
            check "sweep $endian multi $op" "$endian" "$pages" s_none -- "$op" i.tiff
        done
    done
done

echo
echo "PASS=$PASS FAIL=$FAIL"
if [ "$FAIL" != 0 ]; then
    echo "failed:$FAILED"
    exit 1
fi
