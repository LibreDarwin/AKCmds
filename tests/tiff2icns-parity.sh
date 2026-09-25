#!/bin/bash
# Byte-parity harness: /usr/bin/tiff2icns (oracle) vs our clean-room tiff2icns.
# Compares stdout, stderr, exit status, the set of files produced, and the bytes
# of every file in the case directory.  Self-contained: no state outside its
# temp directory and no fixture files checked into the tree.
#
#   ./tests/tiff2icns-parity.sh
#   make tiff2icns-parity
#
# MY defaults to the Makefile release binary; override with MY=... .  A relative
# MY is resolved against the project root, so the Makefile can pass a short path
# (neither make variant allows $(shell)/$(CURDIR)).  ORACLE can be overridden to
# compare against a different reference build.
#
# Requires bash (the null-safe read loops and local -d '' are bash-only) and
# python3, which builds the TIFF fixtures.  Building the binary already needs a
# full toolchain, so python3 is present wherever this can run.

ORACLE=${ORACLE:-/usr/bin/tiff2icns}
HERE=$(cd "$(dirname "$0")" && pwd)
PROJROOT=$(cd "$HERE/.." && pwd)
MY=${MY:-build/release/tiff2icns}
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

ROOT=$(mktemp -d "${TMPDIR:-/tmp}/tiff2icns-parity.XXXXXX") || exit 1
trap 'rm -rf "$ROOT"' EXIT

PASS=0
FAIL=0
FAILED=""

# Fixture builder: writes a multi-page, uncompressed, single-strip TIFF, one
# directory per argument, each argument being w,h,bitsPerSample,samplesPerPixel.
# Pages differ in size, so the slot the tool picks for each is observable in the
# output.  Baseline little-endian TIFF: header, then for each page the pixel
# data, the BitsPerSample array (when it does not fit in the value field), two
# RATIONAL resolutions and the IFD.  Every offset depends on the ones before it,
# so the IFD next-pointers and the header's first-IFD offset are patched last.
mktiff() {
    python3 - "$@" <<'PYEOF'
import struct, sys

path = sys.argv[1]
pages = [tuple(int(v) for v in p.split(',')) for p in sys.argv[2:]]
body = bytearray(b'II' + struct.pack('<HI', 42, 0))
ifds = []
for (w, h, bps, spp) in pages:
    base = len(body)
    # Samples are little-endian and must be bps/8 bytes wide, or the strip
    # the IFD describes is longer than the data actually written and the
    # reader runs into the following page instead of the end of the file.
    step = max(bps // 8, 1)
    body += b''.join(
        ((x * 7 + y * 13) & 0xFF).to_bytes(step, 'little')
        for y in range(h) for x in range(w * spp))
    if spp > 2:
        bps_off = len(body)
        body += b''.join(struct.pack('<H', bps) for _ in range(spp))
    else:
        bps_off = 0
    xres_off = len(body)
    body += struct.pack('<II', 72, 1)
    yres_off = len(body)
    body += struct.pack('<II', 72, 1)

    def entry(tag, typ, cnt, val):
        if typ == 3 and cnt == 1:
            return struct.pack('<HHIHH', tag, typ, cnt, val, 0)
        if typ == 3 and cnt == 2:
            return struct.pack('<HHII', tag, typ, cnt, val | (val << 16))
        return struct.pack('<HHII', tag, typ, cnt, val)

    entries = [
        entry(256, 4, 1, w),
        entry(257, 4, 1, h),
        entry(258, 3, 1, bps) if spp == 1 else
            (entry(258, 3, 2, bps) if spp == 2 else
                entry(258, 3, spp, bps_off)),
        entry(259, 3, 1, 1),
        entry(262, 3, 1, 1 if spp == 1 else 2),
        entry(273, 4, 1, base),
        entry(277, 3, 1, spp),
        entry(278, 4, 1, h),
        entry(279, 4, 1, w * h * spp * bps // 8),
        entry(282, 5, 1, xres_off),
        entry(283, 5, 1, yres_off),
        entry(284, 3, 1, 1),
    ]
    entries.sort(key=lambda t: struct.unpack('<H', t[:2])[0])
    off = len(body)
    body += struct.pack('<H', len(entries)) + b''.join(entries) \
        + struct.pack('<I', 0)
    ifds.append((off, len(entries)))
for i, (off, n) in enumerate(ifds):
    nxt = ifds[i + 1][0] if i + 1 < len(ifds) else 0
    struct.pack_into('<I', body, off + 2 + 12 * n, nxt)
struct.pack_into('<I', body, 4, ifds[0][0])
open(path, 'wb').write(bytes(body))
PYEOF
}

# check <label> <pages> [name [setup]] -- <args...>
#   pages : space-separated w,h,bps,spp specs written to <name>, or "-" for none
#   name  : the fixture's file name (default i.tiff)
#   setup : function run with the case directory as $1 to add further files
#           (a bad source, a read-only directory, ...; default s_none)
# name and setup are both optional, and the "--" that ends the header may
# follow either of them, so consume them by looking for the separator rather
# than by counting positional arguments.
# The arguments after -- are handed to both tools, run inside the case dir.
check() {
    local label="$1" pages="$2" name=i.tiff setup=s_none
    shift 2
    if [ "$1" != "--" ]; then
        name="$1"
        shift
        if [ "$1" != "--" ]; then
            setup="$1"
            shift
        fi
    fi
    shift   # the literal --
    local o="$ROOT/o" m="$ROOT/m"
    rm -rf "$o" "$m"
    mkdir -p "$o" "$m"

    if [ "$pages" != "-" ]; then
        # The spec list has to word-split into one argument per page.
        # shellcheck disable=SC2086
        mktiff "$o/$name" $pages
        # shellcheck disable=SC2086
        mktiff "$m/$name" $pages
    fi
    $setup "$o"
    $setup "$m"

    ( cd "$o" && "$ORACLE" "$@" >o.out 2>o.err )
    local ro=$?
    ( cd "$m" && "$MY" "$@" >o.out 2>o.err )
    local rm=$?

    # Both tools print their own argv[0]; normalize it so the comparison is
    # about behavior rather than the install path.
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

# Extra fixtures for the cases that are not a plain TIFF.  Each takes the case
# directory as $1.
s_none()  { :; }
s_text()  { echo "not an image" > "$1/src.tiff"; }
s_empty() { : > "$1/src.tiff"; }
s_dir()   { mkdir -p "$1/adir"; }
s_ro()    { mkdir -p "$1/ro"; chmod 555 "$1/ro"; }

S4="16,16,8,3 32,32,8,3 48,48,8,3 128,128,8,3"
S6="16,16,8,3 32,32,8,3 48,48,8,3 128,128,8,3 256,256,8,3 512,512,8,3"
S6R="512,512,8,3 256,256,8,3 128,128,8,3 48,48,8,3 32,32,8,3 16,16,8,3"

echo "tiff2icns parity: $ORACLE vs $MY"

# --- which representation sizes are used, and in what order -----------------
check "16/32/48/128"          "$S4"   -- i.tiff
check "all six, ascending"    "$S6"   -- i.tiff
check "all six, descending"   "$S6R"  -- i.tiff
check "16 only"               "16,16,8,3"   -- i.tiff
check "32 only"               "32,32,8,3"   -- i.tiff
check "48 only"               "48,48,8,3"   -- i.tiff
check "128 only"              "128,128,8,3" -- i.tiff
check "256 only"              "256,256,8,3" -- i.tiff
check "512 only"              "512,512,8,3" -- i.tiff

# --- sizes that must not be used -------------------------------------------
# 64 and 96 are not icon slots, and nothing is ever synthesized from the source.
# A 1024x1024 page matches nothing that gets written, but it does suppress the
# "no appropriate images" warning; 1023 and 1025 pin that down as an exact-size
# test rather than a size threshold.
check "64 only"               "64,64,8,3"   -- i.tiff
check "96 only"               "96,96,8,3"   -- i.tiff
check "64 only -noLarge"      "64,64,8,3"   -- -noLarge i.tiff
check "96 only -noLarge"      "96,96,8,3"   -- -noLarge i.tiff
check "1023 only -noLarge"    "1023,1023,8,3" -- -noLarge i.tiff
check "1024 only"             "1024,1024,8,3" -- i.tiff
check "1024 only -noLarge"    "1024,1024,8,3" -- -noLarge i.tiff
check "1025 only -noLarge"    "1025,1025,8,3" -- -noLarge i.tiff
check "16 with 1024"          "16,16,8,3 1024,1024,8,3" -- i.tiff
check "32 with 1024"          "32,32,8,3 1024,1024,8,3" -- i.tiff

# --- only exact square representations qualify ------------------------------
check "32x16 with 32x32"      "32,16,8,3 32,32,8,3" -- i.tiff
check "64x32 only"            "64,32,8,3"           -- i.tiff
check "24 with 32"            "24,24,8,3 32,32,8,3" -- i.tiff
check "48x64 with 48"         "48,64,8,3 48,48,8,3" -- i.tiff

# --- equal sizes: which representation wins ---------------------------------
# Both orderings are checked because the tie-break is not symmetric.
check "32@8 then 32@16"       "32,32,8,3 32,32,16,3" -- i.tiff
check "32@16 then 32@8"       "32,32,16,3 32,32,8,3" -- i.tiff

# --- representations that cannot be encoded ---------------------------------
# A matched page with more than 8 bits per sample counts as found but is not
# written, and the result is a degenerate stub rather than an empty file.
check "16-bit 16"             "16,16,16,3"  -- i.tiff
check "16-bit 32"             "32,32,16,3"  -- i.tiff
check "16-bit 48"             "48,48,16,3"  -- i.tiff
check "16-bit 128"            "128,128,16,3" -- i.tiff
check "16-bit 256"            "256,256,16,3" -- i.tiff
check "16-bit 512"            "512,512,16,3" -- i.tiff
check "16-bit 32 -noLarge"    "32,32,16,3"  -- -noLarge i.tiff
check "16-bit 16 and 32"      "16,16,16,3 32,32,16,3" -- i.tiff
check "16-bit 32 with 8-bit 16" "32,32,16,3 16,16,8,3" -- i.tiff

# -noLarge does not change which representations are written, only whether the
# "no appropriate images" warning appears.
check "16 -noLarge"           "16,16,8,3"   -- -noLarge i.tiff
check "32 -noLarge"           "32,32,8,3"   -- -noLarge i.tiff
check "128 -noLarge"          "128,128,8,3" -- -noLarge i.tiff
check "512 -noLarge"          "512,512,8,3" -- -noLarge i.tiff

# --- other sample layouts ---------------------------------------------------
check "gray 1spp"             "32,32,8,1"   -- i.tiff
check "rgba 4spp"             "32,32,8,4"   -- i.tiff
check "rgba 48"               "48,48,8,4"   -- i.tiff

# --- default output naming --------------------------------------------------
check "src.tiff -> src.icns"  "32,32,8,3" src.tiff -- src.tiff
check "a.b.tiff -> a.b.icns"  "32,32,8,3" a.b.tiff -- a.b.tiff
check "no extension"          "32,32,8,3" plain -- plain
check "uppercase .TIFF"       "32,32,8,3" UP.TIFF -- UP.TIFF
check "name with a space"     "32,32,8,3" "my icon.tiff" -- "my icon.tiff"

# --- explicit outfile ------------------------------------------------------
check "explicit outfile"      "32,32,8,3" src.tiff -- src.tiff out.icns
check "explicit outfile -noLarge" "32,32,8,3" src.tiff \
    -- -noLarge src.tiff out.icns
check "outfile in missing subdir" "32,32,8,3" src.tiff -- src.tiff sub/out.icns
check "outfile equals source" "32,32,8,3" src.tiff -- src.tiff src.tiff

# --- argument handling -----------------------------------------------------
check "no args"               "32,32,8,3" src.tiff -- 
check "only -noLarge"         "32,32,8,3" src.tiff -- -noLarge
check "-noLarge not first"    "32,32,8,3" src.tiff -- src.tiff -noLarge
check "-noLarge after out"    "32,32,8,3" src.tiff \
    -- src.tiff out.icns -noLarge
check "-noLarge twice"        "32,32,8,3" src.tiff \
    -- -noLarge -noLarge src.tiff
check "three arguments"       "32,32,8,3" i.tiff -- i.tiff o1.icns o2.icns
check "unknown flag"          "32,32,8,3" src.tiff -- -bogus src.tiff
check "-- separator"          "32,32,8,3" src.tiff -- -- src.tiff

# --- error paths -----------------------------------------------------------
check "missing source"        "32,32,8,3" src.tiff -- nope.tiff
check "missing -noLarge"      "32,32,8,3" src.tiff -- -noLarge nope.tiff
check "not an image"          "-" src.tiff s_text  -- src.tiff
check "empty file"            "-" src.tiff s_empty -- src.tiff
check "directory as source"   "-" s_none s_dir -- adir
check "unwritable outfile dir" "32,32,8,3" src.tiff s_ro -- src.tiff ro/out.icns
check "unwritable explicit"   "32,32,8,3" src.tiff s_ro -- src.tiff ro/x.icns

echo
echo "PASS=$PASS FAIL=$FAIL"
if [ "$FAIL" != 0 ]; then
    echo "failed:$FAILED"
    exit 1
fi
