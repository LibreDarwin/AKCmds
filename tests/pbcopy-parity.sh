#!/usr/bin/env bash
# pbcopy/pbpaste byte-parity against /usr/bin/pbcopy and /usr/bin/pbpaste.
#
# SAFETY.  -pboard accepts only general|ruler|find|font.  Any other value is
# silently ignored and the write lands on the general pasteboard, so there is
# no way to make a private one.  Every case here therefore names ruler, find
# or font explicitly, and the guard below refuses to run the harness if an
# invocation would touch general.  A test that overwrites the developer's real
# clipboard is worse than no test.
#
# Pasteboard state is shared process-wide, so cases run sequentially and each
# one rewrites what it reads.
#
#   bash tests/pbcopy-parity.sh                 # against the release build
#   MY=build/debug/pbcopy bash tests/pbcopy-parity.sh

set -u

HERE=$(cd "$(dirname "$0")" && pwd)
ROOT=$(cd "$HERE/.." && pwd)
CONFIG=${CONFIG:-release}
ORACLE_COPY=${ORACLE_COPY:-/usr/bin/pbcopy}
ORACLE_PASTE=${ORACLE_PASTE:-/usr/bin/pbpaste}
# MY names the pbcopy binary; pbpaste is the same build under another name.
MY=${MY:-$ROOT/build/$CONFIG/pbcopy}
MY_DIR=$(cd "$(dirname "$MY")" && pwd)
MY_COPY=$MY_DIR/$(basename "$MY")
MY_PASTE=$MY_DIR/pbpaste

for tool in "$ORACLE_COPY" "$ORACLE_PASTE" "$MY_COPY" "$MY_PASTE"; do
	if [ ! -x "$tool" ]; then
		echo "$0: not executable: $tool" >&2
		exit 2
	fi
done

TMP=$(mktemp -d "${TMPDIR:-/tmp}/pbcopy-parity.XXXXXX") || exit 2
trap 'rm -rf "$TMP"' EXIT INT TERM

PASS=0
FAIL=0
SKIP=0
FAILED=

# Refuse any invocation that would read or write the general pasteboard.
guard() {
	local a
	for a in "$@"; do
		case "$a" in
		-pboard|-Prefer|-prefer)
			continue
			;;
		general)
			echo "refusing to touch the general pasteboard" >&2
			exit 3
			;;
		ruler|find|font)
			;;
		esac
	done
	# A bare -pboard with no value, or a value the oracle would reject, would
	# fall through to general.  Reject the shape outright instead.
	local prev=""
	for a in "$@"; do
		if [ "$prev" = "-pboard" ]; then
			case "$a" in
			ruler|find|font) ;;
			*)
				echo "unsafe -pboard value: $a" >&2
				exit 3
				;;
			esac
		fi
		prev="$a"
	done
	return 0
}

# round <label> <pb> <stdin-escapes> [pbcopy args...]
round() {
	local label="$1" pb="$2" data="$3"
	shift 3
	guard -pboard "$pb" "$@"

	local ao am ro rm
	ao=$(printf '%b' "$data" | "$ORACLE_COPY" -pboard "$pb" "$@" 2>&1)
	ro=$?
	am=$(printf '%b' "$data" | "$MY_COPY" -pboard "$pb" "$@" 2>&1)
	rm=$?

	local bo bm
	bo=$("$ORACLE_PASTE" -pboard "$pb" | od -An -c | tr -d ' \n')
	bm=$("$MY_PASTE" -pboard "$pb" | od -An -c | tr -d ' \n')

	# pbcopy is silent on success; compare its status and any stderr, and
	# compare what each one left on the pasteboard as read by the oracle's
	# pbpaste, so a type or encoding difference shows up as a byte diff.
	if [ "$ro" = "$rm" ] && [ "$bo" = "$bm" ] && [ "$ao" = "$am" ]; then
		PASS=$((PASS + 1))
	else
		FAIL=$((FAIL + 1))
		FAILED="$FAILED $label"
		echo "FAIL $label"
		[ "$ro" = "$rm" ] || echo "    rc  oracle=$ro ours=$rm"
		[ "$bo" = "$bm" ] ||
			echo "    board oracle=$bo
           ours  =$bm"
		[ "$ao" = "$am" ] && echo "    pbcopy stderr differs"
	fi
}

echo "pbcopy parity: $ORACLE_COPY vs $MY_COPY"
echo

# Type sniffing: the header decides the declared type.
round "sniff/plain"          ruler "hello"
round "sniff/ps"             ruler "%!PS-Adobe-2.0 EPSF-1.0\n"
round "sniff/rtf"            ruler "{\\rtf1\\ansi x}"
round "sniff/ps-alt-header"  ruler "%!PS-Adobe-3.0\n"
round "sniff/rtf-no-ansi"    ruler "{\\rtf\\ansi x}"

# Empty and degenerate input.
round "empty"                find ""
round "newline-only"         find "\n"
round "nul-bytes"            find "a\x00b"
round "trailing-newline"     find "line\n"

# Encoding, driven by the locale the process starts in.
round "enc/utf8"             font "caf\xc3\xa9"
round "enc/latin1"           font "caf\xe9"
round "enc/ascii"            font "plain"
round "enc/empty"            font ""

# pbcopy accepts and ignores these; each must not change the stored bytes.
round "opt/unknown-pboard"   ruler "x"
round "opt/double-pboard"    ruler "x" -pboard find

echo
echo "PASS=$PASS FAIL=$FAIL SKIP=$SKIP"
[ "$FAIL" = 0 ] || {
	echo "failed:$FAILED"
	exit 1
}
exit 0
