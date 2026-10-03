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

# icctag <file> get   -- print one line per stamped profile, as local epoch secs.
#                        Templates that were never stamped print nothing.
# icctag <file> zero  -- overwrite every profile's date with zeros.
# Both exit non-zero when the file carries no ICC profile at all.
#
# The date is six big-endian 16-bit numbers at offset 24 of the profile: year,
# month, day, hour, minute, second.  A Lab output carries the moment it was
# written there, so any two runs seconds apart differ in exactly those bytes.
# See NOTES.md for why that is local wall clock and not UTC.
#
# Every directory in the file is walked, not just the first: a multi-page image
# gets one profile per page and each is stamped on its own, so leaving the later
# ones alone makes the comparison flaky exactly when a run straddles a second.
icctag() {
    python3 - "$1" "$2" <<'PYEOF'
import struct, sys, datetime

path, mode = sys.argv[1], sys.argv[2]
tsize = {1: 1, 2: 1, 3: 2, 4: 4, 5: 8, 6: 1, 7: 1, 8: 2, 9: 4, 10: 8, 11: 4}
d = bytearray(open(path, 'rb').read())
if len(d) < 8:
    sys.exit(1)
e = '<' if d[:2] == b'II' else '>'
if struct.unpack_from(e + 'H', d, 2)[0] != 42:
    sys.exit(1)
off = struct.unpack_from(e + 'I', d, 4)[0]
seen = set()
found = False
changed = False
while off and off not in seen and off + 2 <= len(d):
    seen.add(off)
    n = struct.unpack_from(e + 'H', d, off)[0]
    for i in range(n):
        p = off + 2 + i * 12
        if p + 12 > len(d):
            break
        tag, typ, cnt = struct.unpack_from(e + 'HHI', d, p)
        if tag != 34675 or cnt * tsize.get(typ, 1) <= 4:
            continue
        vo = struct.unpack(e + 'I', bytes(d[p + 8:p + 12]))[0]
        if vo + 36 > len(d):
            continue
        found = True
        y, mo, dy, h, mi, s = struct.unpack_from('>6H', d, vo + 24)
        if mode == 'zero':
            d[vo + 24:vo + 36] = b'\0' * 12
            changed = True
        elif (y, mo, dy, h, mi, s) != (0,) * 6:
            try:
                print(int(datetime.datetime(y, mo, dy, h, mi, s).timestamp()))
            except ValueError:
                sys.exit(1)
    off = struct.unpack_from(e + 'I', d, off + 2 + n * 12)[0]
if mode == 'zero' and changed:
    open(path, 'wb').write(bytes(d))
sys.exit(0 if found else 1)
PYEOF
}

# check <label> <endian> <pages> <setup> -- <args...>
#   endian : "little" or "big"
#   pages  : space-separated page specs, or "-" for no fixture
#   setup  : function run with the case directory as $1 to add further files
#            (a compressed source, a bad source, ...; default s_none)
# The arguments after -- are handed to both tools, run inside the case dir.
# Set SKIP_STATUS in the environment to leave the exit status out of the
# comparison; see check_nostatus below for when that is the right thing.
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
    if [ -z "$SKIP_STATUS" ] && [ "$ro" != "$rm" ]; then
        why="exit($ro/$rm)"
    fi
    cmp -s "$o/o.out" "$m/o.out" || why="$why stdout"
    cmp -s "$o/o.err" "$m/o.err" || why="$why stderr"

    # A Lab output carries the moment it was written in the twelve date bytes at
    # offset 24 of its rebuilt ICC profile, so two runs seconds apart differ in
    # exactly those bytes and a plain cmp would be flaky by construction.  ICC_DATE
    # checks every stamp against the current clock first and then zeroes the field,
    # so the other 484 bytes of each profile are still compared byte for byte.
    # Checking the value rather than ignoring it is the point: it is what catches
    # a stamp written in the wrong timezone, which a mask alone would hide.
    local stamp stamp_a stamp_b now
    if [ -n "${ICC_DATE-}" ]; then
        now=$(date +%s)
        local sf
        while IFS= read -r -d '' sf; do
            stamp_a=$(icctag "$o/$sf" get) || continue
            [ -n "$stamp_a" ] || continue
            stamp_b=$(icctag "$m/$sf" get) || stamp_b=""
            if [ -z "$stamp_b" ]; then
                why="$why stamp:$sf"
                continue
            fi
            # A multi-page file has one stamp per page, so every one of them has
            # to be the current moment and not just the first.
            for stamp in $stamp_a $stamp_b; do
                if [ $((now - stamp)) -gt 5 ] || [ $((stamp - now)) -gt 5 ]; then
                    why="$why stamp:$sf"
                    break
                fi
            done
            icctag "$o/$sf" zero && icctag "$m/$sf" zero
        done < <( cd "$o" && find . -type f ! -name 'o.out' ! -name 'o.err' \
            -print0 )
    fi

    local fl fm
    fl=$( cd "$o" && find . -type f ! -name 'o.out' ! -name 'o.err' | sort )
    fm=$( cd "$m" && find . -type f ! -name 'o.out' ! -name 'o.err' | sort )
    [ "$fl" = "$fm" ] || why="$why files[$fl|$fm]"

    # Compare the bytes of every fixture and every produced file.  Null-safe so
    # that names containing spaces are handled.  A file named in SKIP_BYTES is
    # still required to exist and to be listed, but its contents are not
    # compared: see check_nobytes below.
    while IFS= read -r -d '' f; do
        case " ${SKIP_BYTES-} " in
            *" $f "*) continue ;;
        esac
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

# check, without the exit status in the comparison.  The reference tool dies of
# SIGSEGV (rc 139) on some inputs, and a clean implementation that reports the
# same messages and then stops cannot match that.  Everything else -- both
# streams byte for byte, the files present, the fixture bytes -- still is
# compared, which is more than the excluded cases below get.
check_nostatus() {
    SKIP_STATUS=1 check "$@"
}

# For the inputs the reference tool opens badly enough that its output file is
# worthless: it reports "1 image written" and leaves a zero-length file behind,
# because the conversion had already lost the image.  The file has to be there
# and has to be listed, but its bytes are not compared -- we write the image
# through instead of losing it, and that difference is deliberate.
check_nobytes() {
    local skip="$1"; shift
    SKIP_BYTES="$skip" check "$@"
}

# check, for a case whose output carries a CIELab profile.  Those are the only
# files the reference stamps with the time, and the stamp is a *local* wall
# clock, which is a question a masked comparison cannot answer on its own.
check_lab() {
    ICC_DATE=1 check "$@"
}

# check_lab with the zone forced, so the stamp is checked against a clock that is
# not UTC.  gmtime_r instead of localtime_r passes every Lab case on a host that
# happens to sit on UTC and fails all of them anywhere else, which means a suite
# run in the default zone proves nothing about it.  These cases are what stop the
# next timezone mistake from being invisible again.  TZ is restored afterwards so
# the rest of the run keeps the caller's zone.
check_lab_tz() {
    local tz="$1" was_set=0 keep="$TZ"
    [ -n "${TZ+x}" ] && was_set=1
    TZ="$tz"
    export TZ
    check_lab "${@:2}"
    if [ "$was_set" = 1 ]; then
        TZ="$keep"
        export TZ
    else
        unset TZ
    fi
}

# Extra fixtures.  Each takes the case directory as $1.
s_none()  { :; }
s_text()  { echo "not an image" > "$1/junk.tiff"; }
s_empty() { : > "$1/junk.tiff"; }
s_dir()   { mkdir -p "$1/adir"; }
# The shapes of "opened, and found not to be a TIFF".  Every byte is spelled in
# octal so that no shell quoting decides what a null is.
s_short1() { printf '\111'                > "$1/junk.tiff"; }
s_short3() { printf '\111\111\052'        > "$1/junk.tiff"; }
s_short4() { printf '\111\111\052\000'    > "$1/junk.tiff"; }
s_bmagic() { printf '\130\131'            > "$1/junk.tiff"; }
# A sound header and a version that is not 42.
s_bver()   { printf '\111\111\053\000\010\000\000\000' > "$1/junk.tiff"; }
# A sound header pointing at a directory that is not there, and at one that is
# there but holds no entries.
s_past()   { printf '\111\111\052\000\377\377\000\000' > "$1/junk.tiff"; }
s_zerodir() { printf '\111\111\052\000\010\000\000\000\000\000\000\000\000' \
    > "$1/junk.tiff"; }
# A sound header pointing at nothing at all.
s_noifd()  { printf '\111\111\052\000\000\000\000\000' > "$1/junk.tiff"; }
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
# A G4 source whose row uses the 2D extension code 0000001.  The reference
# never writes that code, so it goes in front of a strip the reference did
# write, which is the only way to get third party facsimile data into the
# comparison.  $1 is the directory and $2 the three bits that follow the code:
# the code is seven bits long, so those three are read again as the start of
# the next mode, and all eight of them are worth a fixture.
#
# The code ends the row it is in, so the image is a single row: whatever
# follows the code is then never read, and each fixture pins the row the
# extension leaves behind without depending on how the bits after it decode.
g4ext() {
    "$ORACLE" -none "$1/i.tiff" -out "$1/c.tiff" >/dev/null 2>&1 || return 1
    python3 - "$1/c.tiff" "$2" <<'PYEOF' || return 1
import struct, sys

path, sel = sys.argv[1], sys.argv[2]
d = bytearray(open(path, "rb").read())
e = "<" if d[:2] == b"II" else ">"
ifd = struct.unpack_from(e + "I", d, 4)[0]
n = struct.unpack_from(e + "H", d, ifd)[0]

strip = cnt = None
for i in range(n):
    tag, typ, count = struct.unpack_from(e + "HHI", d, ifd + 2 + i * 12)
    if typ != 4 or count != 1:          # the strip tables have to be inline
        continue
    val = struct.unpack_from(e + "I", d, ifd + 2 + i * 12 + 8)[0]
    if tag == 273:
        strip = val
    elif tag == 279:
        cnt = val
if strip is None or cnt is None:
    sys.exit("no inline strip table")

# 0000001 is not a prefix of any other mode, so it is written whole rather
# than left to whichever code its first four bits spell.
bits = "0000001" + sel
size = (cnt * 8 + len(bits) + 7) // 8
new = bytearray(size)
new[:cnt] = d[strip:strip + cnt]
for i, b in enumerate(bits):
    if b == "1":
        new[(cnt * 8 + i) // 8] |= 1 << (7 - ((cnt * 8 + i) & 7))

# The strip is repointed at the copy appended here, so nothing that was
# already in the file moves and every other tag stays as the reference wrote it.
off = len(d)
d += new
for i in range(n):
    p = ifd + 2 + i * 12
    tag = struct.unpack_from(e + "H", d, p)[0]
    if tag == 273:
        struct.pack_into(e + "I", d, p + 8, off)
    elif tag == 279:
        struct.pack_into(e + "I", d, p + 8, size)
open(path, "wb").write(bytes(d))
PYEOF
}
for g4ext_sel in 000 001 010 011 100 101 110 111; do
    eval "s_g4ext_$g4ext_sel() { g4ext \"\$1\" $g4ext_sel; }"
done
# A G4 strip rebuilt out of a bit string: $1 is the directory, $2 the bits in
# front of the strip the reference wrote, $3 keep or drop for that strip
# itself, and $4 the bits after it.  The reference writes an end of block mark
# exactly once, at the end, and never writes a code word that is in no table, so
# moving the mark or ending the strip on bits that are not a code are the only
# way to compare damaged streams.  The reference recovers from all of them
# rather than refusing the conversion, so a case here that ends the strip early
# leaves the rows it never reached at the all colour 0 line.
g4bits() {
    "$ORACLE" -none "$1/i.tiff" -out "$1/c.tiff" >/dev/null 2>&1 || return 1
    python3 - "$1/c.tiff" "$2" "$3" "$4" <<'PYEOF' || return 1
import struct, sys

path, pre, mid, post = sys.argv[1:5]
d = bytearray(open(path, "rb").read())
e = "<" if d[:2] == b"II" else ">"
ifd = struct.unpack_from(e + "I", d, 4)[0]
n = struct.unpack_from(e + "H", d, ifd)[0]

strip = cnt = None
for i in range(n):
    tag, typ, count = struct.unpack_from(e + "HHI", d, ifd + 2 + i * 12)
    if typ != 4 or count != 1:          # the strip tables have to be inline
        continue
    val = struct.unpack_from(e + "I", d, ifd + 2 + i * 12 + 8)[0]
    if tag == 273:
        strip = val
    elif tag == 279:
        cnt = val
if strip is None or cnt is None:
    sys.exit("no inline strip table")

def bits_of(b):
    return "".join("1" if (b[i >> 3] >> (7 - (i & 7))) & 1 else "0"
                   for i in range(len(b) * 8))

# The strip the reference wrote is trimmed back to its last code and given the
# end of block mark back, so a case reads the same whichever end it splices.
body = "" if mid == "drop" else bits_of(d[strip:strip + cnt]).rstrip("0") + "1"
bits = pre + body + post
size = (len(bits) + 7) // 8
new = bytearray(size)
for i, b in enumerate(bits):
    if b == "1":
        new[i // 8] |= 1 << (7 - (i & 7))

# The strip is repointed at the copy appended here, so nothing that was
# already in the file moves and every other tag stays as the reference wrote it.
off = len(d)
d += new
for i in range(n):
    p = ifd + 2 + i * 12
    tag = struct.unpack_from(e + "H", d, p)[0]
    if tag == 273:
        struct.pack_into(e + "I", d, p + 8, off)
    elif tag == 279:
        struct.pack_into(e + "I", d, p + 8, size)
open(path, "wb").write(bytes(d))
PYEOF
}
s_g4bits_eofb()      { g4bits "$1" 000000000001 keep  ""; }
s_g4bits_eofb_junk() { g4bits "$1" 000000000001 keep  111111111111111111111111; }
s_g4bits_junk()      { g4bits "$1" 111111111111111111111111 drop ""; }
s_g4bits_tail_junk() { g4bits "$1" "" keep 111111111111111111111111; }
s_g4bits_tail_zero() { g4bits "$1" "" keep 000000000000000000000000; }
s_g4bits_tail_alt()  { g4bits "$1" "" keep 101010101010101010101010; }
# A G4 strip with an explicit end of line mark after the first row.  A Group 4
# strip needs none, because every row runs to the full width on its own, and
# the reference never writes one; some senders do.  The reference reads the mark
# as the end of the strip rather than as the end of a row, so such a strip
# decodes to its first row and nothing after it.
#
# Where to put the mark comes from compressing the same image cut to one row,
# which is the same first row: the line above it is the imaginary all colour 0
# line either way.  The end of block mark is twelve bits ending in a one, so
# the zeros in front of it would survive a strip of trailing zeros, and the
# twelve bits have to come off with the mark rather than with the zeros.
g4eol() {
    local d="$1" w="$2"
    "$ORACLE" -none "$d/i.tiff" -out "$d/c.tiff" >/dev/null 2>&1 || return 1
    mktiff "$d/.row0.tiff" little "$w,1,1,1,1"
    "$ORACLE" -none "$d/.row0.tiff" -out "$d/.row0c.tiff" >/dev/null 2>&1 ||
        return 1
    python3 - "$d/c.tiff" "$d/.row0c.tiff" <<'PYEOF' || return 1
import struct, sys

def data_bits(path):
    d = bytearray(open(path, "rb").read())
    e = "<" if d[:2] == b"II" else ">"
    ifd = struct.unpack_from(e + "I", d, 4)[0]
    n = struct.unpack_from(e + "H", d, ifd)[0]
    strip = cnt = None
    for i in range(n):
        tag, typ, count = struct.unpack_from(e + "HHI", d, ifd + 2 + i * 12)
        if typ != 4 or count != 1:      # the strip tables have to be inline
            continue
        val = struct.unpack_from(e + "I", d, ifd + 2 + i * 12 + 8)[0]
        if tag == 273:
            strip = val
        elif tag == 279:
            cnt = val
    if strip is None or cnt is None:
        sys.exit("no inline strip table")
    bits = "".join("1" if (d[strip + (i >> 3)] >> (7 - (i & 7))) & 1 else "0"
                   for i in range(cnt * 8))
    return d, e, ifd, n, bits[:-12].rstrip("0")

_, _, _, _, head = data_bits(sys.argv[2])
d, e, ifd, n, body = data_bits(sys.argv[1])
bits = head + "000000000001" + body + "000000000001"

size = (len(bits) + 7) // 8
new = bytearray(size)
for i, b in enumerate(bits):
    if b == "1":
        new[i // 8] |= 1 << (7 - (i & 7))

# The strip is repointed at the copy appended here, so nothing that was
# already in the file moves and every other tag stays as the reference wrote it.
off = len(d)
d += new
for i in range(n):
    p = ifd + 2 + i * 12
    tag = struct.unpack_from(e + "H", d, p)[0]
    if tag == 273:
        struct.pack_into(e + "I", d, p + 8, off)
    elif tag == 279:
        struct.pack_into(e + "I", d, p + 8, size)
open(sys.argv[1], "wb").write(bytes(d))
PYEOF
    local rc=$?
    rm -f "$d/.row0.tiff" "$d/.row0c.tiff"
    return $rc
}
s_g4eol_16()  { g4eol "$1" 16; }
s_g4eol_200() { g4eol "$1" 200; }
s_pred() {
    "$ORACLE" -lzw -out /dev/null "$1/i.tiff" >/dev/null 2>&1
    :
}

# Fixture builder for the unknown-field warning: an uncompressed 8-bit gray
# directory carrying extra tags.  One semicolon-separated group per directory,
# and the entries inside a group go in exactly as written and are never
# reordered, so a case can pin that a warning follows the order the entries
# sit in rather than the order of the tag numbers -- and so a case can ask for
# a tag twice, which is what a directory that repeats a tag means.  A tag is
# written as "n" or as "n=value".
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
    for spec in group.split(','):
        tag, _, val = spec.partition('=')
        entries.append((int(tag), 3, 1, [int(val) if val else 1]))
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

# SGILOG is the one layout the generic builder cannot describe: the strip is
# four run-length coded byte planes *per row* rather than a row of samples, so
# the pixels have to be written plane by plane.  mklogluv takes a rows per
# strip, because that is the parameter that decides whether a strip holds one
# four-plane group or several -- and a one-row fixture cannot tell the two
# apart, which is how the plane grouping got wrong to begin with.  "sign" gives
# every pixel a set sign bit, which the reference answers with black.
mklogluv() {
    python3 - "$@" <<'PYEOF'
import struct, sys

def enc(plane):
    """SGILOG run-length coding: a control of 126 or more is that many minus
    126 copies of the next byte, 1..127 is that many literals, 0 does nothing."""
    out = bytearray()
    i, n = 0, len(plane)
    while i < n:
        run = 1
        while i + run < n and plane[i + run] == plane[i] and run < 129:
            run += 1
        if run >= 4:
            out.append(126 + run)
            out.append(plane[i])
            i += run
            continue
        start, lit = i, 0
        while i < n and lit < 127:
            run = 1
            while i + run < n and plane[i + run] == plane[i] and run < 4:
                run += 1
            if run >= 4:
                break
            i += 1
            lit += 1
        out.append(lit)
        out += plane[start:start + lit]
    return bytes(out)


path, endian, w, h, rps, sign = sys.argv[1], sys.argv[2], *map(int, sys.argv[3:])
e = '<' if endian == 'little' else '>'
magic = b'II' if endian == 'little' else b'MM'

rows = []
for y in range(h):
    row = []
    for x in range(w):
        le = (0x8000 | (y * w + x) * 7 % 0x7800) if sign else ((y * w + x) * 11 % 0x7ffe + 1)
        row.append((le << 16) | ((x * 5 + y) % 256) << 8 | ((x * 3 + y * 2) % 256))
    rows.append(row)

# The word's bytes, most significant first: Le high (sign included), Le low,
# ue, ve.
def plane_of(row, f):
    if f == 0:
        return bytes((px >> 24) & 0xff for px in row)
    if f == 1:
        return bytes((px >> 16) & 0xff for px in row)
    if f == 2:
        return bytes((px >> 8) & 0xff for px in row)
    return bytes(px & 0xff for px in row)


body = bytearray(magic + struct.pack(e + 'HI', 42, 0))
offs, cnts = [], []
s = 0
while s < h:
    n = min(s + rps, h)
    d = bytearray()
    for i in range(s, n):
        for f in range(4):
            d += enc(plane_of(rows[i], f))
    offs.append(len(body))
    body += d
    cnts.append(len(d))
    s = n

ifd = len(body)
ntags = 10
p258 = ifd + 2 + 12 * ntags + 4
p273 = p258 + 6
p279 = p273 + 4 * len(offs)
body += struct.pack(e + 'H', ntags)
# A value that fits in the four byte field goes in the field.  StripOffsets and
# StripByteCounts only go out of line when there is more than one of them, so a
# single strip fixture has to carry them inline or a reader takes the pointer as
# the value.
one = len(offs) == 1
entries = [(256, 3, 1, w), (257, 3, 1, h), (258, 3, 3, None), (259, 3, 1, 34676),
           (262, 3, 1, 32845), (273, 4, len(offs), offs[0] if one else None),
           (277, 3, 1, 3), (278, 4, 1, rps),
           (279, 4, len(cnts), cnts[0] if one else None), (284, 3, 1, 1)]
ptrs = {258: p258, 273: p273, 279: p279}
for tag, typ, cnt, val in entries:
    body += struct.pack(e + 'HHI', tag, typ, cnt)
    if val is None:
        body += struct.pack(e + 'I', ptrs[tag])
    else:
        raw = struct.pack(e + ('H' if typ == 3 else 'I'), val)
        body += raw + b'\0' * (4 - len(raw))
body += struct.pack(e + 'I', 0)
while len(body) < p258:
    body += b'\0'
body += struct.pack(e + 'HHH', 16, 16, 16)
if not one:
    while len(body) < p273:
        body += b'\0'
    body += struct.pack(e + '%dI' % len(offs), *offs)
    body += struct.pack(e + '%dI' % len(cnts), *cnts)
struct.pack_into(e + 'I', body, 4, ifd)

with open(path, 'wb') as f:
    f.write(bytes(body))
PYEOF
}

s_logluv_1x1()    { mklogluv "$1/i.tiff" little 1 1 1 0; }
s_logluv_1x4()    { mklogluv "$1/i.tiff" little 1 4 1 0; }
s_logluv_16x1()   { mklogluv "$1/i.tiff" little 16 1 1 0; }
s_logluv_16x1B()  { mklogluv "$1/i.tiff" big    16 1 1 0; }
s_logluv_40x5()   { mklogluv "$1/i.tiff" little 40 5 3 0; }
s_logluv_100x3()  { mklogluv "$1/i.tiff" little 100 3 1 0; }
s_logluv_7x7()    { mklogluv "$1/i.tiff" little 7 7 7 0; }
s_logluv_64x4()   { mklogluv "$1/i.tiff" little 64 4 4 0; }
s_logluv_sign()   { mklogluv "$1/i.tiff" little 16 4 2 1; }
s_unk_hi() { mkextra "$1/i.tiff" little 65000; }
s_unk_two(){ mkextra "$1/i.tiff" little 347,65000,65001; }
# Descending on purpose: the warning has to follow the directory, not the
# tag numbers.
s_unk_rev(){ mkextra "$1/i.tiff" little 65001,65000,347; }
# Entries that do not ascend.  A tag listed again counts as going backwards,
# which is what makes every one of these trip the same check.
s_ooo_one() { mkextra "$1/i.tiff" little 259=1,258=8; }
# The same tag twice, both entries holding the same value.
s_dup_same() { mkextra "$1/i.tiff" little 258=8,258=8; }
s_dup_unk()  { mkextra "$1/i.tiff" little 347=1,347=1; }
s_dup_two()  { mkextra "$1/i.tiff" little 347=1,347=1;258=8,258=8; }
s_dup_dir()  { mkextra "$1/i.tiff" little '347=1,347=1;347=1,347=1'; }
# The same tag twice with the entries disagreeing.  Only the first is ever set,
# so the second value is never acted on at all.
s_dup_diff() { mkextra "$1/i.tiff" little 258=8,258=4; }
s_dup_3x()   { mkextra "$1/i.tiff" little 258=8,258=4,258=2; }
s_dup_unkd() { mkextra "$1/i.tiff" little 347=1,347=2; }
# Two different unnamed tags, each listed twice, with the copies interleaved.
# Whichever entry comes first is the one that is set, so the warnings read
# 347 then 348 rather than 348 then 347.
s_dup_int()  { mkextra "$1/i.tiff" little 347=1,348=1,347=1,348=1; }
# A field that is turned down, listed twice.  The bad value first is reported
# once and the good value after it says nothing; the good value first means the
# bad one is never set, so there is nothing to report.
s_dup_soft() { mkextra "$1/i.tiff" little 266=0,266=0; }
s_dup_soft2(){ mkextra "$1/i.tiff" little 266=0,266=2; }
s_dup_soft3(){ mkextra "$1/i.tiff" little 266=2,266=0; }
# DataType and SampleFormat say the same thing in two numberings, so which one
# the report believes is decided by which entry sits later.
s_sf_dt()    { mkextra "$1/i.tiff" little 339=3,32996=2; }
s_sf_dt_6()  { mkextra "$1/i.tiff" little 32996=1,339=6; }
s_unk_dir(){ mkextra "$1/i.tiff" little '347;65000'; }
s_unk_be() { mkextra "$1/i.tiff" big 347; }
# Tag 333 is a name the reference tool has, but asking for one InkName hangs
# it for minutes, so it is left out of every fixture here.  254 and 305 are
# left out too: the first adds a Subfile Type line and the second a complaint
# about a missing null terminator, neither of which this case is about.  What
# is left are names the tool has and never mentions.
s_unk_known() { mkextra "$1/i.tiff" little 300,434,700; }

# NewSubfileType is three flags, so the name is built from the bits: 0 and
# anything past bit 2 leave it empty, and the three of them together are joined
# with a slash.  The old SubfileType (255) says nothing at all.
s_sub_none() { mkextra "$1/i.tiff" little 254=0; }
s_sub_one()  { mkextra "$1/i.tiff" little 254=1; }
s_sub_two()  { mkextra "$1/i.tiff" little 254=2; }
s_sub_three(){ mkextra "$1/i.tiff" little 254=3; }
s_sub_four() { mkextra "$1/i.tiff" little 254=4; }
s_sub_all()  { mkextra "$1/i.tiff" little 254=7; }
s_sub_past() { mkextra "$1/i.tiff" little 254=8; }
s_sub_high() { mkextra "$1/i.tiff" little 254=1000; }
s_sub_old()  { mkextra "$1/i.tiff" little 255=1; }
# ResolutionUnit names three units and turns down the rest, which costs it the
# line and earns a complaint on stderr instead.
s_ru_one()   { mkextra "$1/i.tiff" little 296=1; }
s_ru_two()   { mkextra "$1/i.tiff" little 296=2; }
s_ru_three() { mkextra "$1/i.tiff" little 296=3; }
s_ru_zero()  { mkextra "$1/i.tiff" little 296=0; }
s_ru_nine()  { mkextra "$1/i.tiff" little 296=9; }
s_ru_hi()    { mkextra "$1/i.tiff" little 296=65535; }
s_ru_big()   { mkextra "$1/i.tiff" big 296=0; }
# The complaint about a turned-down field is made after every unknown-field
# warning, not among them.
s_ru_unk()   { mkextra "$1/i.tiff" little 296=0,347=1; }

# FillOrder has two names, Orientation eight, and a value outside either costs
# the line and draws the complaint instead.
s_fo_one()   { mkextra "$1/i.tiff" little 266=1; }
s_fo_two()   { mkextra "$1/i.tiff" little 266=2; }
s_fo_zero()  { mkextra "$1/i.tiff" little 266=0; }
s_fo_three() { mkextra "$1/i.tiff" little 266=3; }
s_fo_big()   { mkextra "$1/i.tiff" big 266=0; }
for i in 0 1 2 3 4 5 6 7 8 9; do
    eval "s_or_$i() { mkextra \"\$1/i.tiff\" little 274=$i; }"
done
s_or_hi()    { mkextra "$1/i.tiff" little 274=65535; }
# All three refusals in one directory, on their own and behind an unnamed field,
# to pin that they follow the order the entries sit in.
s_ref_all()  { mkextra "$1/i.tiff" little 266=0,274=0,296=0; }
s_ref_unk()  { mkextra "$1/i.tiff" little 266=0,274=0,296=0,347=1; }

# The other four of the family refuse harder: the whole report is lost, not
# just the one line, and the tool stops on "Can't open" having said nothing
# else.  The reference tool then dies on a null image source, so these use
# check_nostatus; -dump and the write operations survive them and are compared
# normally.  SampleFormat numbers one to six and DataType zero to three;
# ExtraSamples is a list of zero to two and names the element count, not the
# element, when one of them is out of range.
s_sf_zero()  { mkextra "$1/i.tiff" little 339=0; }
s_sf_seven() { mkextra "$1/i.tiff" little 339=7; }
s_sf_hi()    { mkextra "$1/i.tiff" little 339=65535; }
s_dt_four()  { mkextra "$1/i.tiff" little 32996=4; }
s_dt_hi()    { mkextra "$1/i.tiff" little 32996=65535; }
s_td_zero()  { mkextra "$1/i.tiff" little 32998=0; }
s_td_big()   { mkextra "$1/i.tiff" big 32998=0; }
s_xs_three() { mkextra "$1/i.tiff" little 338=3; }
s_xs_hi()    { mkextra "$1/i.tiff" little 338=65535; }
# DataType is set second, so where a directory carries both it is the one named.
s_sf_dt()    { mkextra "$1/i.tiff" little 339=1,32996=3; }
s_dt_sf()    { mkextra "$1/i.tiff" little 32996=2,339=3; }
# The refusals come after the unknown-field warnings, and only the first one in
# a directory is named before the walk stops.
s_ref_unk2() { mkextra "$1/i.tiff" little 339=0,347=1,65000=1; }
s_ref_two()  { mkextra "$1/i.tiff" little 32996=4,339=0,298=7; }
for i in 0 1 2 3 4 5 6; do
    eval "s_sf_$i() { mkextra \"\$1/i.tiff\" little 339=$i; }"
done
for i in 0 1 2 3; do
    eval "s_dt_$i() { mkextra \"\$1/i.tiff\" little 32996=$i; }"
done
for i in 0 1 2; do
    eval "s_xs_$i() { mkextra \"\$1/i.tiff\" little 338=$i; }"
done
for i in 1 2 8 65535; do
    eval "s_td_$i() { mkextra \"\$1/i.tiff\" little 32998=$i; }"
done

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

# The three CIE Lab encodings.  All three come out as plain Lab (photometric 8),
# and all three make the reference build its own profile rather than copy one,
# which is the only output in this file that carries a build timestamp.
LAB="16,16,8,3,8"
LABW="64,48,8,3,8"                  # both dimensions even
LABO="15,13,8,3,8"                  # both dimensions odd
LAB16="16,16,16,3,8"                # 16-bit Lab under photometric 8
LABI="16,16,8,3,9"                  # ICELab
LABL="16,16,8,3,10"                 # ITULab
LABSTRIP="40,40,8,3,8,1,1,8"        # multi-strip
LABMULTI="16,16,8,3,8 32,32,8,3,8 8,8,8,3,8"

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

# --- CIE Lab, the one output that carries the moment it was written ------------
# The reference builds a 496-byte "Custom Lab Profile" per image rather than
# copying a template, and stamps the current time into its date field.  check_lab
# compares that value against the clock before zeroing it, so these cases verify
# the stamp and the other 484 profile bytes together.
for op in -none -lzw -packbits; do
    for shape in "$LAB" "$LABW" "$LABO" "$LAB16" "$LABI" "$LABL" \
                 "$LABSTRIP" "$LABMULTI"; do
        check_lab "$op lab $(printf '%s' "$shape" | tr ',' '-')" \
            little "$shape" s_none -- "$op" i.tiff -out o.tiff
    done
    check_lab "$op BE lab 64-48-8-3-8" big "$LABW" s_none \
        -- "$op" i.tiff -out o.tiff
done

# The same two shapes again under zones that are not UTC, so that the stamp is
# pinned to local time rather than merely agreeing with whatever zone the suite
# happens to run in.  Kolkata is on a half hour and Kiritimati is a day ahead of
# UTC, so neither can be faked by an hour-boundary slip.
for tz in America/New_York Asia/Kolkata Pacific/Kiritimati; do
    for shape in "$LAB" "$LAB16"; do
        check_lab_tz "$tz" "$tz lab $(printf '%s' "$shape" | tr ',' '-')" \
            little "$shape" s_none -- -none i.tiff -out o.tiff
    done
done

# --- LogLuv, which is decoded into 32-bit float samples -------------------
# The reference answers a SGILOG file with a float TIFF, so this is a change of
# output type rather than a conversion of one, and the depth and the sample
# format both have to be re-tagged on the way out.  The shapes cover a single
# pixel, one strip holding several rows (where the four planes repeat per row
# rather than spanning the strip), a strip per row, a single strip covering the
# whole image, both byte orders, and the sign bit that the reference answers with
# black.
for op in -none -lzw -packbits; do
    for fx in s_logluv_1x1 s_logluv_1x4 s_logluv_16x1 s_logluv_40x5 \
             s_logluv_100x3 s_logluv_7x7 s_logluv_64x4 s_logluv_sign; do
        check "$op $fx" little - "$fx" -- "$op" i.tiff -out o.tiff
    done
    check "$op s_logluv_16x1B" big - s_logluv_16x1B -- "$op" i.tiff -out o.tiff
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
# The 2D extension code.  One row per fixture, because the code ends the row
# and a second row would make the three bits after it decide the result.
for g4ext_sel in 000 001 010 011 100 101 110 111; do
    check "g4ext $g4ext_sel -none" little "16,1,1,1,1" \
        "s_g4ext_$g4ext_sel" -- -none c.tiff -out o.tiff
    check "g4ext $g4ext_sel -info" little "16,1,1,1,1" \
        "s_g4ext_$g4ext_sel" -- -info c.tiff
done
check "g4ext BE -none" big "16,1,1,1,1" s_g4ext_000 -- -none c.tiff -out o.tiff
check "g4ext -none"      little "32,1,1,1,1" s_g4ext_011 -- -none c.tiff -out o.tiff
check "g4ext 64 -none"   little "64,1,1,1,1" s_g4ext_101 -- -none c.tiff -out o.tiff
# An end of block mark ends the strip, so one in front of the rows leaves them
# all at the all colour 0 line, and bits that are in no code end the row they
# are in without taking the conversion down with them.
for g4bits_case in eofb eofb_junk junk tail_junk tail_zero tail_alt; do
    check "g4bits $g4bits_case -none" little "$B1G" \
        "s_g4bits_$g4bits_case" -- -none c.tiff -out o.tiff
done
check "g4bits eofb -info" little "$B1G" s_g4bits_eofb -- -info c.tiff
check "g4bits eofb BE"    big    "$B1G" s_g4bits_eofb -- -none c.tiff -out o.tiff
# A strip written with an explicit end of line mark after a row is a mark the
# reference never writes, and it reads the mark as the end of the strip, so
# these pin a strip that decodes to its first row and nothing after it.
check "g4eol 16 -none" little "$B1G"   s_g4eol_16  -- -none c.tiff -out o.tiff
check "g4eol 200 -none" little "$B1GL" s_g4eol_200 -- -none c.tiff -out o.tiff
check "g4eol 16 -info" little "$B1G"   s_g4eol_16  -- -info c.tiff
check "g4eol 16 BE"    big    "$B1G"   s_g4eol_16  -- -none c.tiff -out o.tiff
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
# Entries are meant to ascend, and a tag listed again counts as going
# backwards, so a repeat trips the same check a larger tag ahead of a smaller
# one does.  It is said once for the directory, ahead of everything else that
# directory has to say, and only where the directory is read through the TIFF
# library: -dump walks the raw IFD and says nothing, and a conversion says
# nothing either.
for op in -info -verboseinfo; do
    check "entries descending $op"   little "$G8W" s_ooo_one  -- "$op" i.tiff
    check "tag twice $op"            little "$G8W" s_dup_same -- "$op" i.tiff
    check "unknown twice $op"        little "$G8W" s_dup_unk  -- "$op" i.tiff
    check "tag twice interleaved $op" little "$G8W" s_dup_int -- "$op" i.tiff
    check "twice per directory $op"  little "$G8W" s_dup_dir  -- "$op" i.tiff
    check "tag twice disagree $op"   little "$G8W" s_dup_diff -- "$op" i.tiff
    check "tag three times $op"      little "$G8W" s_dup_3x   -- "$op" i.tiff
    check "unknown twice bad $op"    little "$G8W" s_dup_unkd -- "$op" i.tiff
    check "refused twice $op"        little "$G8W" s_dup_soft -- "$op" i.tiff
    check "refused then good $op"    little "$G8W" s_dup_soft2 -- "$op" i.tiff
    check "good then refused $op"    little "$G8W" s_dup_soft3 -- "$op" i.tiff
done
check "entries descending -dump" little "$G8W" s_ooo_one  -- -dump i.tiff
check "tag twice -dump"          little "$G8W" s_dup_same -- -dump i.tiff
# -dump reports the entry it is standing on, so a tag listed twice prints its
# own value on each of the two lines rather than the first value twice.
check "tag twice disagree -dump" little "$G8W" s_dup_diff -- -dump i.tiff
check "tag three times -dump"    little "$G8W" s_dup_3x   -- -dump i.tiff
check "unknown twice bad -dump"  little "$G8W" s_dup_unkd -- -dump i.tiff
# Listing a tag twice with the entries agreeing leaves nothing to act on, so
# the image comes through and the output is compared byte for byte.
for setup in s_dup_same s_dup_unk s_dup_soft s_ooo_one s_dup_dir s_dup_int; do
    check "$setup convert" little "$G8W" "$setup" -- -none i.tiff -out o.tiff
done
# Agreeing about the value is what matters here.  Two entries that disagree
# leave the reference tool unable to make sense of the image, so it loses the
# image and reports success; we write the image through and leave the output
# bytes out of the comparison.  Which of the two it believes is decided by the
# one sitting first, so a good value followed by a bad one is no worse than
# the other way round -- both lose the image.
for setup in s_dup_diff s_dup_3x s_dup_unkd s_dup_soft2 s_dup_soft3; do
    check_nobytes ./o.tiff "$setup write" little "$G8W" "$setup" \
        -- -none i.tiff -out o.tiff
done
# DataType and SampleFormat both say what the samples are, in numberings a
# count apart, so the one sitting later in the directory is the one believed.
check "sampleformat then datatype" little "$G8W" s_sf_dt   -- -info i.tiff
check "datatype then sampleformat" little "$G8W" s_dt_sf   -- -info i.tiff
check "sampleformat unrenderable"   little "$G8W" s_sf_dt_6 -- -info i.tiff
# Predictor is named only for the codecs that predict.  Under an uncompressed
# directory it is as unnamed as anything else; under LZW it is not.
check "predictor -info uncompressed" little "$G8W" s_none -- -info i.tiff
check "predictor -info lzw"    little "$G8W" s_lzw      -- -info c.tiff
# NewSubfileType is a set of flags, and its name is spelled out from them.
for op in -info -verboseinfo; do
    for setup in s_sub_none s_sub_one s_sub_two s_sub_three s_sub_four \
                 s_sub_all s_sub_past s_sub_high s_sub_old; do
        check "subfile $setup $op" little "$G8W" "$setup" -- "$op" i.tiff
    done
    # ResolutionUnit has three names and says nothing about any other value.
    for setup in s_ru_one s_ru_two s_ru_three s_ru_zero s_ru_nine s_ru_hi; do
        check "resunit $setup $op" little "$G8W" "$setup" -- "$op" i.tiff
    done
    check "resunit bad BE $op"   big    "$G8W" s_ru_big  -- "$op" i.tiff
    check "resunit bad $op dump" little "$G8W" s_ru_zero -- -dump i.tiff
    # A turned-down field complains after the unknown-field warnings.
    check "resunit bad + unknown $op" little "$G8W" s_ru_unk -- "$op" i.tiff
    # FillOrder names two orders and Orientation eight; the rest lose the line.
    for setup in s_fo_one s_fo_two s_fo_zero s_fo_three; do
        check "fillorder $setup $op" little "$G8W" "$setup" -- "$op" i.tiff
    done
    check "fillorder bad BE $op" big "$G8W" s_fo_big -- "$op" i.tiff
    for i in 0 1 2 3 4 5 6 7 8 9; do
        check "orientation s_or_$i $op" little "$G8W" "s_or_$i" -- "$op" i.tiff
    done
    check "orientation bad hi $op" little "$G8W" s_or_hi -- "$op" i.tiff
    # Every refusal in one directory, alone and behind an unnamed field.
    check "refused all $op"  little "$G8W" s_ref_all -- "$op" i.tiff
    check "refused all + unknown $op" little "$G8W" s_ref_unk -- "$op" i.tiff
    # The four that take the report with them.  Every value in range is named
    # and reported normally; out of range says so and stops.
    for setup in s_sf_1 s_sf_2 s_sf_3 s_sf_4 s_sf_5 s_sf_6 \
                 s_dt_0 s_dt_1 s_dt_2 s_dt_3 s_xs_0 s_xs_1 s_xs_2 \
                 s_td_1 s_td_2 s_td_8 s_td_65535; do
        check "$setup $op" little "$G8W" "$setup" -- "$op" i.tiff
    done
    for setup in s_sf_zero s_sf_seven s_sf_hi s_dt_four s_dt_hi \
                 s_td_zero s_xs_three s_xs_hi; do
        check_nostatus "$setup $op" little "$G8W" "$setup" -- "$op" i.tiff
    done
    check_nostatus "tiledepth zero BE $op" big "$G8W" s_td_big -- "$op" i.tiff
    check "sampleformat big-endian $op" big "$G8W" s_sf_3 -- "$op" i.tiff
    check "sampleformat both $op" little "$G8W" s_sf_dt -- "$op" i.tiff
    check "datatype both $op"    little "$G8W" s_dt_sf -- "$op" i.tiff
    check_nostatus "refused + unknown $op" little "$G8W" s_ref_unk2 -- "$op" i.tiff
    check_nostatus "two refused $op"      little "$G8W" s_ref_two  -- "$op" i.tiff
done
# -dump names the Silicon Graphics extensions, and reads the same files the
# report refuses.
for setup in s_dt_4 s_sf_1 s_xs_1 s_td_8; do
    check "$setup dump" little "$G8W" "$setup" -- -dump i.tiff
done
for setup in s_sf_zero s_dt_four s_td_zero s_xs_three; do
    check "$setup dump refused" little "$G8W" "$setup" -- -dump i.tiff
    # The reference tool loses the image here and writes an empty file while
    # reporting success; we write the image through, so the output bytes are
    # the one thing left out of the comparison.
    check_nobytes ./o.tiff "$setup write refused" little "$G8W" "$setup" \
        -- -none i.tiff -out o.tiff
done
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
# directory.  Reproducing a crash is not a parity target, so only the exit
# status is left out of those four, via check_nostatus: both streams byte for
# byte, the files present and the fixture bytes still have to match.
check "missing source"      little "$G8W" s_none  -- -none nope.tiff
check "missing source dump" little "$G8W" s_none  -- -dump nope.tiff
check "not an image"        little "$G8W" s_text  -- -none junk.tiff
check "empty file"          little "$G8W" s_empty -- -none junk.tiff
check "empty file dump"     little "$G8W" s_empty -- -dump junk.tiff
check "directory as source" little "$G8W" s_dir   -- -none adir

# Every shape of a failed open is run through all three ways of looking at one.
# The reports crash on the reference tool and -dump and the write operations do
# not, so the two families differ in how much is compared rather than in what
# is checked.
allops() {
    local setup="$1" src="$2" label="$3"

    check_nostatus "$label report"  little "$G8W" "$setup" -- -info "$src"
    check_nostatus "$label vreport" little "$G8W" "$setup" -- -verboseinfo "$src"
    check          "$label dump"    little "$G8W" "$setup" -- -dump "$src"
    check          "$label write"   little "$G8W" "$setup" -- -none "$src" -out o.tiff
    check          "$label extract" little "$G8W" "$setup" -- -extract 0 "$src" -out o.tiff
}
allops s_none junk.tiff "missing"
allops s_dir  adir      "directory"
for setup in s_text s_empty s_short1 s_short3 s_short4 s_bmagic s_bver \
             s_past s_zerodir s_noifd; do
    allops "$setup" junk.tiff "${setup#s_}"
done

# A file that is there but cannot be read is told apart from one that is not a
# TIFF, and named rather than called malformed.  Root ignores the mode, so the
# case only means anything when the suite is not run as root -- and neither
# tool can read the file back afterwards, so its bytes are left out.
s_noperm() { mktiff "$1/junk.tiff" little "$G8W"; chmod 000 "$1/junk.tiff"; }
if [ "$(id -u)" != 0 ]; then
    SKIP_STATUS=1 check_nobytes ./junk.tiff "unreadable report"  little "$G8W" s_noperm -- -info junk.tiff
    SKIP_STATUS=1 check_nobytes ./junk.tiff "unreadable vreport" little "$G8W" s_noperm -- -verboseinfo junk.tiff
    check_nobytes ./junk.tiff "unreadable dump"  little "$G8W" s_noperm -- -dump junk.tiff
    check_nobytes ./junk.tiff "unreadable write" little "$G8W" s_noperm -- -none junk.tiff -out o.tiff
fi

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
