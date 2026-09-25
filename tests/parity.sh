#!/bin/bash
# Byte-parity harness: /usr/bin/tops (oracle) vs our clean-room tops.
# Compares stdout+stderr combined, exit status, and the state of every input
# file, for every case. Self-contained: no state outside its temp directory.
#
#   ./tests/parity.sh            # default: 64 non-TTY cases
#   ./tests/parity.sh --tty      # 17 TTY cases under `script`
#   ./tests/parity.sh --all      # both
#
# Also reachable as `make parity` / `make test` and as the xcodeproj's
# `tops-parity` target, so keep this file tracked: the build depends on it.
#
# MY defaults to the Makefile release binary; override with MY=... .
# A relative MY is resolved against the project root, so the Makefile and the
# Xcode build phase can both pass a short path (neither can use $(shell)/pwd,
# and bmake has no $(CURDIR)).
#
# Requires bash: the diagnostics below use process substitution, so invoking
# this with `sh parity.sh` is a syntax error. Both build systems call bash.

ORACLE=${ORACLE:-/usr/bin/tops}
HERE=$(cd "$(dirname "$0")" && pwd)
PROJROOT=$(cd "$HERE/.." && pwd)
MY=${MY:-build/release/tops}
case "$MY" in
    /*) ;;
    *) MY="$PROJROOT/$MY" ;;
esac
ORACLE_DIR=$(cd "$(dirname "$ORACLE")" && pwd) || exit 1
ORACLE="$ORACLE_DIR/$(basename "$ORACLE")"

if [ ! -x "$MY" ]; then
    echo "missing binary: $MY" >&2
    echo "build it with: make -C $PROJROOT" >&2
    exit 1
fi
if [ ! -x "$ORACLE" ]; then
    echo "missing oracle: $ORACLE" >&2
    exit 1
fi

ROOT=$(mktemp -d "${TMPDIR:-/tmp}/tops-parity.XXXXXX") || exit 1
trap 'rm -rf "$ROOT"' EXIT

PASS=0
FAIL=0
FAILED=""

# The fixture needs parts that collide with the short 1-2 char patterns used by
# the matcher cases, plus enough distinct messages for the 8-rule case.
FIXTURE='alpha
[obj pq:1]
[obj rs:9]
[obj tu:5]
[obj vw:3]
[obj xx:4]
[obj yy:8]
[obj zz:6]'

# run_file_case <label> <filecount> <args...>
# Builds a pristine copy of the fixture for each side and diffs everything.
run_file_case() {
    local lab="$1" n="$2"
    shift 2
    local o="$ROOT/o" m="$ROOT/m" i=1 names=""
    rm -rf "$o" "$m"
    mkdir -p "$o" "$m"
    while [ "$i" -le "$n" ]; do
        printf '%s\n' "$FIXTURE" > "$o/f$i.m"
        cp "$o/f$i.m" "$m/f$i.m"
        names="$names f$i.m"
        i=$((i + 1))
    done

    ( cd "$o" && "$ORACLE" "$@" $names > .stdout 2>&1 </dev/null )
    local orc=$?
    ( cd "$m" && "$MY" "$@" $names > .stdout 2>&1 </dev/null )
    local mrc=$?

    local bad=0
    cmp -s "$o/.stdout" "$m/.stdout" || bad=1
    [ "$orc" = "$mrc" ] || bad=1
    for f in $names; do
        cmp -s "$o/$f" "$m/$f" || bad=1
    done

    if [ "$bad" = 0 ]; then
        PASS=$((PASS + 1))
    else
        FAIL=$((FAIL + 1))
        FAILED="$FAILED $lab"
        echo "FAIL $lab  (rc $orc vs $mrc)"
        diff <(od -c -v "$o/.stdout") <(od -c -v "$m/.stdout") |
            perl -ne 'print "    stdout: $_"'
        for f in $names; do
            if ! cmp -s "$o/$f" "$m/$f"; then
                echo "    file $f differs:"
                diff <(od -c -v "$o/$f") <(od -c -v "$m/$f") |
                    perl -ne 'print "      $_"'
            fi
        done
    fi
}

# run_stdin_case <label> <args...>
# Feeds the fixture on stdin; output is compared byte for byte.
run_stdin_case() {
    local lab="$1"
    shift
    local a b
    a=$(printf '%s\n' "$FIXTURE" | "$ORACLE" "$@" 2>&1)
    local orc=$?
    b=$(printf '%s\n' "$FIXTURE" | "$MY" "$@" 2>&1)
    local mrc=$?

    if [ "$a" = "$b" ] && [ "$orc" = "$mrc" ]; then
        PASS=$((PASS + 1))
    else
        FAIL=$((FAIL + 1))
        FAILED="$FAILED stdin/$lab"
        echo "FAIL stdin/$lab  (rc $orc vs $mrc)"
        diff <(printf '%s\n' "$a") <(printf '%s\n' "$b") |
            perl -ne 'print "    $_"'
    fi
}

# tty_stdin_case <label> <args...>
# Same, but with stdout attached to a pty. The pty's ONLCR turns every \n into
# \r\n for both binaries alike, so a plain cmp is still valid here.
tty_stdin_case() {
    local lab="$1"
    shift
    local a b
    a=$(script -q /dev/null "$ORACLE" "$@" < "$ROOT/fixture.m" 2>&1)
    b=$(script -q /dev/null "$MY" "$@" < "$ROOT/fixture.m" 2>&1)
    if [ "$a" = "$b" ]; then
        PASS=$((PASS + 1))
    else
        FAIL=$((FAIL + 1))
        FAILED="$FAILED tty-stdin/$lab"
        echo "FAIL tty-stdin/$lab"
        diff <(printf '%s\n' "$a") <(printf '%s\n' "$b") |
            perl -ne 'print "    $_"'
    fi
}

# tty_file_case <label> <filecount> <args...>
tty_file_case() {
    local lab="$1" n="$2"
    shift 2
    local o="$ROOT/to" m="$ROOT/tm" i=1 names=""
    rm -rf "$o" "$m"
    mkdir -p "$o" "$m"
    while [ "$i" -le "$n" ]; do
        cp "$ROOT/fixture.m" "$o/f$i.m"
        cp "$ROOT/fixture.m" "$m/f$i.m"
        names="$names f$i.m"
        i=$((i + 1))
    done

    ( cd "$o" && script -q /dev/null "$ORACLE" "$@" $names > .stdout 2>&1 </dev/null )
    ( cd "$m" && script -q /dev/null "$MY" "$@" $names > .stdout 2>&1 </dev/null )

    local bad=0
    cmp -s "$o/.stdout" "$m/.stdout" || bad=1
    for f in $names; do
        cmp -s "$o/$f" "$m/$f" || bad=1
    done

    if [ "$bad" = 0 ]; then
        PASS=$((PASS + 1))
    else
        FAIL=$((FAIL + 1))
        FAILED="$FAILED tty/$lab"
        echo "FAIL tty/$lab"
        diff <(od -c -v "$o/.stdout") <(od -c -v "$m/.stdout") |
            perl -ne 'print "    stdout: $_"'
    fi
}

printf '%s\n' "$FIXTURE" > "$ROOT/fixture.m"

# rules_for <n> -- a chained replacemethod rule string with n comma-joined rules
rules_for() {
    local n="$1" r="" i=1
    for nm in pq rs tu vw xx yy zz; do
        [ "$i" -gt "$n" ] && break
        if [ -z "$r" ]; then
            r="replacemethod $nm: with r${nm}:"
        else
            r="$r, replacemethod $nm: with r${nm}:"
        fi
        i=$((i + 1))
    done
    printf '%s' "$r"
}

run_non_tty() {
    local n r
    echo "== non-TTY: bar geometry across rule counts =="
    for n in 1 2 3 4 5 6 7 8; do
        r=$(rules_for "$n")
        run_stdin_case "N=$n" -verbose $r
        run_stdin_case "N=$n semi" -semiverbose $r
    done
    for n in 1 2 3 4 5; do
        r=$(rules_for "$n")
        run_file_case "1file N=$n" 1 -verbose $r
        run_file_case "3file N=$n" 3 -verbose $r
    done

    echo "== non-TTY: partial match (last rule finds nothing) =="
    r='replacemethod pq: with rFirst:, replacemethod rs: with rSecond:, replacemethod zzz: with rNone:'
    run_stdin_case "partial" -verbose $r
    run_file_case "partial" 2 -verbose $r

    echo "== non-TTY: pattern prefix semantics =="
    for pat in 'pq:rs' 'pq:rs:' 'pq:r' 'p:rs' 'p:q:' 'p:' 'pq:' 'rs:' 'begin:' \
               'tu:' 'tu' 'pq' 'rowAt:andFree:'; do
        run_stdin_case "pat=$pat" -verbose replacemethod "$pat" with rA:rB:
    done

    echo "== non-TTY: replacement selector truncation =="
    for rep in 'same' 'same,' 'rFirst:sSecond' 'rFirst:sSecond:tThird' 'ZZ:' \
               'Z:Q:' 'Z1:Q2:' 'Q:' ':' 'rA:rB:rC:' 'A:B:'; do
        run_stdin_case "rep=$rep" -verbose replacemethod pq: with "$rep"
    done

    echo "== non-TTY: verbosity / reporting flags =="
    r='replacemethod pq: with rFirst:'
    run_stdin_case "dont quiet" -dont $r
    run_stdin_case "dont semi" -dont -semiverbose $r
    run_stdin_case "dont verbose" -dont -verbose $r
    run_stdin_case "quiet" $r
    run_stdin_case "nomatch" -verbose replacemethod begin: with rFirst:
    run_file_case "dont quiet" 1 -dont $r
    run_file_case "dont semi" 1 -dont -semiverbose $r
    run_file_case "quiet" 1 $r
    run_file_case "1 file" 1 -verbose $r
    run_file_case "3 nomatch" 3 -verbose replacemethod begin: with rFirst:
    run_file_case "3 quiet" 3 $r
    run_file_case "3 dont" 3 -dont $r
}

run_tty() {
    local r2='replacemethod pq: with rFirst:, replacemethod rs: with rSecond:'
    local r3='replacemethod pq: with rFirst:, replacemethod rs: with rSecond:, replacemethod tu: with rThird:'
    local r1='replacemethod pq: with rFirst:'
    echo "== TTY: row order, report placement, eager close =="
    tty_file_case "1 rule" 1 -verbose $r1
    tty_file_case "2 rules" 1 -verbose $r2
    tty_file_case "3 rules" 1 -verbose $r3
    tty_file_case "2 files" 2 -verbose $r1
    tty_file_case "3 files" 3 -verbose $r1
    tty_file_case "nomatch" 1 -verbose replacemethod begin: with rFirst:
    tty_file_case "semiverb" 1 -semiverbose $r1
    tty_file_case "quiet" 1 $r1
    tty_file_case "dont verb" 1 -dont -verbose $r1
    tty_file_case "dont quiet" 1 -dont $r1

    echo "== TTY: stdin echoes original text, no reports =="
    tty_stdin_case "1 rule" -verbose $r1
    tty_stdin_case "2 rules" -verbose $r2
    tty_stdin_case "3 rules" -verbose $r3
    tty_stdin_case "semiverb" -semiverbose $r1
    tty_stdin_case "quiet" $r1
    tty_stdin_case "dont quiet" -dont $r1
    tty_stdin_case "nomatch" -verbose replacemethod begin: with rFirst:
}

case "${1:---non-tty}" in
    --tty) run_tty ;;
    --all) run_non_tty; run_tty ;;
    *) run_non_tty ;;
esac

echo
echo "PASS=$PASS FAIL=$FAIL"
if [ "$FAIL" != 0 ]; then
    echo "failed:$FAILED"
    exit 1
fi
exit 0
