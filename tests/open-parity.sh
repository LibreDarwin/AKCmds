#!/bin/bash
#
# open parity: /usr/bin/open vs the locally built open, byte for byte.
#
# Only non-launching invocations are exercised.  Anything that would actually
# start an application is left out on purpose: the point of this harness is to
# compare argument handling, diagnostics and exit status, not to disturb the
# machine it runs on.  Every case below is one that the tools reject (or that
# resolves to a plain file) before any LSApplication is asked to launch
# anything.
#
# The comparison is per case: exit status, stdout and stderr, plus the set of
# files each run leaves behind.  stderr is normalised first, because several
# messages embed the program path and the case directory, both of which differ
# between the two runs by construction.
#
# Env:
#   ORACLE   path to the reference open          (default /usr/bin/open)
#   MY       path to the binary under test       (default build/$CONFIG/open)
#   CONFIG   build configuration, when MY is unset (default release)
#   VERBOSE  set to list passing cases too
#   VERIFY   set to print both sides of every failure
#
set -u

ROOT=$(cd "$(dirname "$0")/.." && pwd)
CONFIG=${CONFIG:-release}
ORACLE=${ORACLE:-/usr/bin/open}
MY=${MY:-$ROOT/build/$CONFIG/open}

# The Makefile passes MY relative to the project root, but every case runs
# from inside a scratch directory, so both paths have to be absolute before
# anything is executed.
ORACLE=$(cd "$(dirname "$ORACLE")" && pwd)/$(basename "$ORACLE")
MYDIR=$(cd "$(dirname "$MY")" 2>/dev/null && pwd) || {
	echo "$0: not executable: $MY" >&2
	exit 2
}
MY=$MYDIR/$(basename "$MY")

for tool in "$ORACLE" "$MY"; do
	if [ ! -x "$tool" ]; then
		echo "$0: not executable: $tool" >&2
		exit 2
	fi
done

TMP=$(mktemp -d "${TMPDIR:-/tmp}/open-parity.XXXXXX") || exit 2
TMP=$(cd "$TMP" && pwd -P)   # mktemp may hand back a symlinked path
trap 'rm -rf "$TMP"' EXIT INT TERM

pass=0
fail=0
failed=""

# t <label> -- <args...>
#
# Runs the oracle and the candidate in separate sibling directories and
# compares exit status, stdout, stderr and the resulting file list.  The two
# case directories deliberately have the same base name, because a few
# messages contain the working directory and it must not be the thing that
# differs.
t() {
	local label="$1"
	shift 2   # drop the label and the literal --
	local o="$TMP/case/o" m="$TMP/case/m" rc_o rc_m bad=
	rm -rf "$TMP/case"
	mkdir -p "$o" "$m"

	(cd "$o" && "$ORACLE" "$@" >stdout 2>stderr); rc_o=$?
	(cd "$m" && "$MY" "$@" >stdout 2>stderr); rc_m=$?

	sed -e "s|$ORACLE|PROG|g" "$o/stderr" >"$o/stderr.n" &&
		mv "$o/stderr.n" "$o/stderr"
	sed -e "s|$MY|PROG|g" "$m/stderr" >"$m/stderr.n" &&
		mv "$m/stderr.n" "$m/stderr"
	sed -e "s|$o|CASEDIR|g" "$o/stderr" >"$o/s2" && mv "$o/s2" "$o/stderr"
	sed -e "s|$m|CASEDIR|g" "$m/stderr" >"$m/s2" && mv "$m/s2" "$m/stderr"

	[ "$rc_o" = "$rc_m" ] || bad="$bad exit($rc_o/$rc_m)"
	# Every case here is meant to stop before anything can be launched, so
	# the tools must both fail.  A case that starts succeeding has reached
	# the launch path and belongs in neither this harness nor a test run.
	if [ "$rc_o" = 0 ]; then
		echo "  LEAK $label: exited 0, this case can launch something" >&2
		bad="$bad launched"
	fi
	cmp -s "$o/stdout" "$m/stdout" || bad="$bad stdout"
	cmp -s "$o/stderr" "$m/stderr" || bad="$bad stderr"

	# Compare what each run left in its directory, so that a run which prints
	# the same complaint but writes a different output is still a failure.
	local lo lm
	lo=$(cd "$o" && find . ! -name stdout ! -name stderr |
		LC_ALL=C sort | tr '\n' ' ')
	lm=$(cd "$m" && find . ! -name stdout ! -name stderr |
		LC_ALL=C sort | tr '\n' ' ')
	[ "$lo" = "$lm" ] || bad="$bad files"

	if [ -z "$bad" ]; then
		pass=$((pass + 1))
		[ -n "${VERBOSE:-}" ] &&
			printf '  ok   %-40s exit=%s  %s\n' \
			    "$label" "$rc_o" "$(head -1 "$o/stderr")"
		return 0
	fi
	fail=$((fail + 1))
	failed="$failed[$label] "
	printf '  FAIL %-40s [%s]\n' "$label" "$bad"
	if [ -n "${VERIFY:-}" ]; then
		printf '       exit   oracle=%s ours=%s\n' "$rc_o" "$rc_m"
		printf '       stderr oracle: %s\n' \
		    "$(head -2 "$o/stderr" | tr '\n' '|')"
		printf '       stderr ours  : %s\n' \
		    "$(head -2 "$m/stderr" | tr '\n' '|')"
		printf '       files  oracle: %s\n' "$lo"
		printf '       files  ours  : %s\n' "$lm"
	fi
	return 1
}

echo "open parity: $ORACLE vs $MY"
echo

# --- option parsing -----------------------------------------------------
t "-help is not an option"        -- -help
t "--help is not an option"       -- --help
t "-x is not an option"           -- -x /etc/hosts
t "-v is accepted"                -- -v /nonexistent-zz/file.txt
t "--no-recents is accepted"      -- --no-recents /nonexistent-zz/file.txt
t "unknown long option"           -- --nosuchoption-zz
t "unknown short option"          -- -Z /etc/hosts
t "-- ends option parsing"        -- -- --stdin

# --- no operands -------------------------------------------------------
t "no arguments"                  --
t "only -a"                       -- -a
t "only -b"                       -- -b
t "only -R"                       -- -R
t "only --arch"                   -- --arch
t "only -n"                       -- -n

# --- missing files and applications -------------------------------------
t "missing absolute file"         -- /nonexistent-zz/file.txt
t "missing relative file"         -- nosuchfile-zz.txt
t "missing directory"             -- /nonexistent-zz/dir/
t "missing application -a"        -- -a NoSuchApplicationZZ
t "missing bundle -b"             -- -b com.no.such.bundle.zz
t "two missing files"             -- /nonexistent-zz/a /nonexistent-zz/b

# --- --arch is validated after files, applications and usage ----------
t "--arch bogus + missing file"   -- --arch bogus /nonexistent-zz/file.txt
t "--arch bogus + missing app"    -- --arch bogus -a NoSuchApplicationZZ
t "--arch bogus + no operands"    -- --arch bogus
t "--arch bogus alone"            -- --arch bogus /nonexistent-zz/file.txt
t "--arch empty"                  -- --arch "" /nonexistent-zz/file.txt
t "--arch bogus + no app"         -- --arch bogus -a NoSuchApplicationZZ
# A recognised --arch can only be observed if the file and application checks
# are both satisfied, which is exactly the point at which something is
# launched, so the accepted spellings (ARM64, Arm64, x86_64, i386, ppc) are
# left alone here.  The ordering is pinned from both sides instead: an
# unrecognised arch with a missing file and with a missing application both
# report the other error, and an application error still precedes the arch
# check for an accepted spelling.
t "--arch valid + missing app"    -- --arch arm64 -a NoSuchApplicationZZ
t "--arch valid + missing file"   -- --arch arm64 /nonexistent-zz/file.txt
t "--arch bogus + no app, no file" -- --arch bogus -a NoSuchApplicationZZ

# --- an argument is a URL only if it starts with a valid scheme --------
# [A-Za-z][A-Za-z0-9+.-]*:  Anything else is a file path, so these must be
# reported as missing files rather than handed to LaunchServices.
t "empty scheme"                  -- "://"
t "empty scheme, one slash"       -- ":/"
t "bare colon"                    -- ":"
t "empty scheme with authority"   -- "://x"
t "colon in a path component"     -- "a/b:c"
t "scheme starting with a digit"  -- "1://y"
t "scheme starting with a plus"   -- "+a://b"
t "space before the scheme"       -- "a b://c"
t "empty argument"                -- "" /nonexistent-zz/file.txt
t "known-looking scheme"          -- "x://y"
t "scheme with a plus"            -- "ma+il:x"
t "scheme with a dash and dot"    -- "a-b.c://z"
t "unhandled scheme + missing"    -- "x://y" /nonexistent-zz/file.txt

# A scheme that some installed application claims, such as mailto:, is
# deliberately absent: the lookup succeeds and something really launches.
# The schemes below are unclaimed, so both tools stop at the LaunchServices
# lookup with kLSApplicationNotFoundErr and nothing is started.
#
# --- file: URLs that do not name a file -------------------------------
t "file:// with no path"          -- "file://"
t "file:// before missing file"   -- "file://" /nonexistent-zz/file.txt
t "file:// after missing file"    -- /nonexistent-zz/file.txt "file://"
t "file:// before -a lookup"      -- "file://" -a NoSuchApplicationZZ

# --- option plumbing that cannot reach a launch -----------------------
t "--args without an app"         -- -a NoSuchApplicationZZ --args one two
t "--env without an app"          -- -a NoSuchApplicationZZ --env FOO=bar
t "--stderr without an app"       -- -a NoSuchApplicationZZ --stderr
t "--stdout without an app"       -- -a NoSuchApplicationZZ --stdout
t "-F without an app"             -- -a NoSuchApplicationZZ -F
t "-j without an app"             -- -a NoSuchApplicationZZ -j
t "-i without an app"             -- -a NoSuchApplicationZZ -i
t "-o without an app"             -- -a NoSuchApplicationZZ -o
t "-e with a missing file"        -- -e /nonexistent-zz/file.txt
t "-t with a missing file"        -- -t /nonexistent-zz/file.txt
t "-g with a missing file"        -- -g /nonexistent-zz/file.txt
t "-s with a missing file"        -- -s /nonexistent-zz/file.txt
t "-W with a missing file"        -- -W /nonexistent-zz/file.txt
t "-n with a missing file"        -- -n /nonexistent-zz/file.txt
t "-R with a missing file"        -- -R /nonexistent-zz/file.txt
t "-R with a missing directory"   -- -R /nonexistent-zz/dir

echo
echo "PASS=$pass FAIL=$fail"
[ -n "$failed" ] && echo "failed:$failed"
[ "$fail" = 0 ]
