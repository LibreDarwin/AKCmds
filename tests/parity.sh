#!/bin/bash
# Byte-parity harness: /usr/bin/tops (oracle) vs our clean-room tops.
# Compares stdout+stderr combined, exit status, and the state of every input
# file, for every case. Self-contained: no state outside its temp directory.
#
#   ./tests/parity.sh            # default: 108 non-TTY cases
#   ./tests/parity.sh --script   # script cases only
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

# run_script_case <label> <fixture> <script> <args...>
# Writes a custom fixture and a scriptfile for each side, runs the tool against
# the file, and diffs stdout+stderr, exit status, and the rewritten file. The
# find/replace/where/within/same surface is only reachable via -scriptfile, so
# these cases live here rather than in run_file_case.
run_script_case() {
    local lab="$1" fixture="$2" script="$3"
    shift 3
    local o="$ROOT/so" m="$ROOT/sm"
    rm -rf "$o" "$m"
    mkdir -p "$o" "$m"
    printf '%b' "$fixture" > "$o/f.m"
    cp "$o/f.m" "$m/f.m"
    printf '%s\n' "$script" > "$o/s.script"
    cp "$o/s.script" "$m/s.script"

    ( cd "$o" && "$ORACLE" -scriptfile s.script "$@" f.m > .stdout 2>&1 </dev/null )
    local orc=$?
    ( cd "$m" && "$MY" -scriptfile s.script "$@" f.m > .stdout 2>&1 </dev/null )
    local mrc=$?

    local bad=0
    cmp -s "$o/.stdout" "$m/.stdout" || bad=1
    [ "$orc" = "$mrc" ] || bad=1
    cmp -s "$o/f.m" "$m/f.m" || bad=1

    if [ "$bad" = 0 ]; then
        PASS=$((PASS + 1))
    else
        FAIL=$((FAIL + 1))
        FAILED="$FAILED script/$lab"
        echo "FAIL script/$lab  (rc $orc vs $mrc)"
        diff <(od -c -v "$o/.stdout") <(od -c -v "$m/.stdout") |
            perl -ne 'print "    stdout: $_"'
        if ! cmp -s "$o/f.m" "$m/f.m"; then
            echo "    file differs:"
            diff <(od -c -v "$o/f.m") <(od -c -v "$m/f.m") |
                perl -ne 'print "      $_"'
        fi
    fi
}

# run_cmd_case <label> <fixture> <args...>
# Same comparison as run_script_case, but the rule comes from the command
# line, which is the only way to reach the leading-dash rule keywords and to
# see how argv operands (patterns, replacements, messages) are re-quoted.
run_cmd_case() {
    local lab="$1" fixture="$2"
    shift 2
    local o="$ROOT/co" m="$ROOT/cm"
    rm -rf "$o" "$m"
    mkdir -p "$o" "$m"
    printf '%b' "$fixture" > "$o/f.m"
    cp "$o/f.m" "$m/f.m"

    ( cd "$o" && "$ORACLE" "$@" f.m > .stdout 2>&1 </dev/null )
    local orc=$?
    ( cd "$m" && "$MY" "$@" f.m > .stdout 2>&1 </dev/null )
    local mrc=$?

    local bad=0
    cmp -s "$o/.stdout" "$m/.stdout" || bad=1
    [ "$orc" = "$mrc" ] || bad=1
    cmp -s "$o/f.m" "$m/f.m" || bad=1

    if [ "$bad" = 0 ]; then
        PASS=$((PASS + 1))
    else
        FAIL=$((FAIL + 1))
        FAILED="$FAILED cmd/$lab"
        echo "FAIL cmd/$lab  (rc $orc vs $mrc)"
        diff <(od -c -v "$o/.stdout") <(od -c -v "$m/.stdout") |
            perl -ne 'print "    stdout: $_"'
        if ! cmp -s "$o/f.m" "$m/f.m"; then
            echo "    file differs:"
            diff <(od -c -v "$o/f.m") <(od -c -v "$m/f.m") |
                perl -ne 'print "      $_"'
        fi
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

# A fixture that puts the search text inside line comments, a block comment, a
# string literal, and a char literal, plus real code hits on either side. Only
# the code hits may match.
CS='// foo here\nint a = foo; /* foo and foo */\nchar *s = "foo";\nchar c = '"'"'f'"'"';\nint b = foo; // foo\n'

run_script() {
    echo "== script: comment/string/literal regions are opaque =="
    run_script_case "find skips comments+literals" "$CS" 'find "foo"' -verbose
    run_script_case "replace skips comments+literals" "$CS" 'replace "foo" with "bar"' -verbose

    echo "== script: replacemethod skips comments+strings =="
    local MS='// [obj pq:1]\n[obj pq:1]\nchar *s = "[obj pq:1]";\n/* [obj pq:1] */\n[obj pq:1]\n'
    run_script_case "rm skips comment" "$MS" 'replacemethod "pq:" with "rpq:"' -verbose
    run_script_case "rm skips string" "$MS" 'replacemethod "pq:" with "rpq:"' -verbose

    echo "== script: word boundaries and multi-word patterns =="
    run_script_case "match before semicolon" 'int a = foo;\n' 'find "foo"' -verbose
    run_script_case "replace before semicolon" 'int a = foo;\n' 'replace "foo" with "bar"' -verbose
    run_script_case "word boundary foobar" 'int foobar = 1; int xfoo = 2;\n' 'find "foo"' -verbose
    run_script_case "match inside brackets" '[obj foo:1]\n' 'find "foo"' -verbose
    run_script_case "multiword exact" 'int a = foo;\n' 'find "int a"' -verbose
    run_script_case "multiword collapses ws" 'int  a = foo;\n' 'find "int a"' -verbose
    run_script_case "multiword needs ws" 'inta = foo;\n' 'find "int a"' -verbose
    run_script_case "multiword across newline" 'a\nb\n' 'find "a b"' -verbose
    run_script_case "multiword punct" 'int; a;\n' 'find "int; a"' -verbose
    run_script_case "multiword replace" 'int a = 1; int a = 2;\n' 'replace "int a" with "X"' -verbose

    echo "== script: unterminated block comment runs to EOF =="
    run_script_case "unterminated block" 'int a = foo;\n/* foo\nint b = foo;\n' 'find "foo"' -verbose

    echo "== script: replace with same is a no-op =="
    run_script_case "replace same" 'int foo = 1;\n' 'replace "foo" with same' -verbose
    run_script_case "replace same quiet" 'int foo = 1;\n' 'replace "foo" with same'

    echo "== script: parse errors =="
    run_script_case "unterminated quote" 'foo\n' 'replace "foo with "baz"' -verbose
    run_script_case "unterminated pattern" 'foo\n' 'replace "foo' -verbose
    run_script_case "unterminated find" 'foo\n' 'find "foo' -verbose
    run_script_case "unterminated replacement" 'foo\n' 'replace "foo" with "baz' -verbose
    run_script_case "expected quote after pattern" 'foo\n' 'replace "foo" x' -verbose
    run_script_case "expected quote after with" 'foo\n' 'replace "foo" with baz' -verbose
    run_script_case "trailing junk after replace" 'foo\n' 'replace "foo" with "baz" x' -verbose
    run_script_case "bare where symbol" 'int foo = 1;\n' 'replace "foo" with "baz" where (foo) isOneOf {("1")}' -verbose
    run_script_case "bare within symbol" 'int foo = 1;\n' 'replace "foo" with "baz" within (bar) {replace "1" with "2"}' -verbose
    run_script_case "bare where match" 'int foo = 1;\n' 'replace "foo" with "baz" where ("foo") isOneOf {(1)}' -verbose
    run_script_case "comma joined rules" 'int foo = 1;\n' 'replace "foo" with "baz", find "bar"' -verbose

    echo "== script: replace argument grammar =="
    run_script_case "with keyword" 'int foo = 1;\n' 'replace "foo" with "bar"' -verbose
    run_script_case "implicit with" 'int foo = 1;\n' 'replace "foo" "bar"' -verbose
    run_script_case "same explicit" 'int foo = 1;\n' 'replace "foo" with same' -verbose
    run_script_case "same implicit" 'int foo = 1;\n' 'replace "foo" same' -verbose
    run_script_case "pattern only eof" 'int foo = 1;\n' 'replace "foo"' -verbose
    run_script_case "replacemethod quoted" '[obj pq:1]\n' 'replacemethod "pq:" with "rpq:"' -verbose

    echo "== script: where clause semantics =="
    run_script_case "where full token filter" 'foo=1; foo=2;\n' 'replace "foo=<e x>" with "X" where ("<e x>") isOneOf {("1")}' -verbose
    run_script_case "where alternatives" 'foo=1; foo=2; foo=3;\n' 'replace "foo=<e x>" with "X" where ("<e x>") isOneOf {("1") ("3")}' -verbose
    run_script_case "where two tokens" 'a=1 b=2; a=1 b=3; a=2 b=2;\n' 'replace "a=<e x> b=<e y>" with "X" where ("<e x>" "<e y>") isOneOf {("1" "2") ("2" "2")}' -verbose
    run_script_case "where clauses are anded" 'a=1 b=2; a=1 b=3; a=2 b=2;\n' 'replace "a=<e x> b=<e y>" with "X" where ("<e x>") isOneOf {("1")} where ("<e y>") isOneOf {("2")}' -verbose
    run_script_case "where missing token ignored" 'a=1 b=2; a=2 b=3;\n' 'replace "a=<e x> b=<e y>" with "X" where ("<e z>") isOneOf {("1")}' -verbose
    run_script_case "where mixed capture ignored" 'a=1 b=2; a=2 b=2;\n' 'replace "a=<e x>" with "X" where ("<e x>" "<e z>") isOneOf {("1" "9") ("2" "9")}' -verbose
    run_script_case "where find" 'foo=1; foo=2;\n' 'find "foo=<e x>" where ("<e x>") isOneOf {("2")}' -verbose
    run_script_case "where tuple arity short" 'foo=1;\n' 'replace "foo=<e x>" with "X" where ("<e x>") isOneOf {("1" "2")}' -verbose
    run_script_case "where tuple arity long" 'a=1 b=2;\n' 'replace "a=<e x> b=<e y>" with "X" where ("<e x>" "<e y>") isOneOf {("1")}' -verbose
    run_script_case "where empty tuple" 'foo=1;\n' 'replace "foo=<e x>" with "X" where ("<e x>") isOneOf {()}' -verbose

    echo "== script: error and warning clauses =="
    # The rewritten file and the occurrence count are compared in every mode
    # below.  -verbose is deliberately not used with an error or warning
    # clause: the oracle then numbers its per-match reports as if the inserted
    # diagnostic headers had already shifted the line, and no simple rule
    # reproduces that (a first match on a line excludes its own headers, later
    # ones do not, and the leading blank line counts for every match but the
    # first).  tops reports the source line instead, which is what the
    # -semiverbose and quiet cases below pin down.
    run_script_case "error on find rejected" 'foo\n' 'find "foo" error "b"' -verbose
    run_script_case "warning on find rejected" 'foo\n' 'find "foo" warning "w"' -verbose
    run_script_case "error on find extra space" 'foo\n' 'find "foo" error  "b"' -verbose
    run_script_case "error on find indented" 'foo\n' '  find "foo" error "b"' -verbose
    run_script_case "error no match" 'foo\n' 'replace "zzz" with "X" error "b"' -verbose
    run_script_case "error one match" 'foo\n' 'replace "foo" with "X" error "b"' -verbose
    run_script_case "error two matches same line" 'foo foo bar\n' 'replace "foo" with "X" error "b"' -semiverbose
    run_script_case "error two lines" 'foo\nfoo\nbar\n' 'replace "foo" with "X" error "b"' -semiverbose
    run_script_case "error second line only" 'bar\nfoo\n' 'replace "foo" with "X" error "b"' -verbose
    run_script_case "error after first line" 'foo\nbar\nfoo\n' 'replace "foo" with "X" error "b"' -semiverbose
    run_script_case "error quiet" 'foo bar\n' 'replace "foo" with "X" error "b"'
    run_script_case "warning one match" 'foo\n' 'replace "foo" with "X" warning "w"' -verbose
    run_script_case "error wins over warning" 'foo\n' 'replace "foo" with "X" error "b" warning "w"' -verbose
    run_script_case "error empty message" 'foo\n' 'replace "foo" with "X" error ""' -verbose
    run_script_case "error message with spaces" 'foo\n' 'replace "foo" with "X" error "b c d"' -verbose
    run_script_case "error no trailing newline" 'foo' 'replace "foo" with "X" error "b"' -verbose
    run_script_case "error on replacemethod" '[obj pq:1]\n[obj pq:2]\n' 'replacemethod "pq:" with "rpq:" error "b"' -semiverbose
    run_script_case "error same line replacemethod" '[obj pq:1] [obj pq:2]\n' 'replacemethod "pq:" with "rpq:" error "b"' -semiverbose
    run_script_case "error with dont" 'foo\n' 'replace "foo" with "X" error "b"' -dont -semiverbose
    run_script_case "error with two rules" 'foo bar\n' 'replace "foo" with "X" error "b"
replace "bar" with "Y"'

    echo "== script: quiet mode still rewrites =="
    run_script_case "quiet replace" 'foo bar\n' 'replace "foo" with "X"'
    run_script_case "quiet replacemethod" '[obj pq:1]\n' 'replacemethod "pq:" with "rpq:"'
    run_script_case "quiet no match" 'foo bar\n' 'replace "zzz" with "X"'
    run_script_case "quiet reports nothing" 'int a = foo;\n' 'replace "foo" with "X"'
    run_script_case "dont reports when quiet" 'int a = foo;\n' 'replace "foo" with "X"' -dont

    echo "== script: find reports in every mode =="
    run_script_case "find quiet" 'int a = foo;\n' 'find "foo"'
    run_script_case "find semiverbose" 'int a = foo;\n' 'find "foo"' -semiverbose
    run_script_case "find dont" 'int a = foo;\n' 'find "foo"' -dont
    run_script_case "find dont quiet" 'int a = foo;\n' 'find "foo"' -dont
    run_script_case "find two lines" 'int a = foo;\nint b = foo;\n' 'find "foo"'
    run_script_case "find two rules" 'a b\nc b\n' 'find "b"
find "c"'
    run_script_case "find after replace" 'a b\nc b\n' 'replace "a" with "X"
find "b"'
    run_script_case "find nofileinfo" 'int a = foo;\n' 'find "foo"' -nofileinfo
    run_script_case "find nofileinfo verbose" 'int a = foo;\n' 'find "foo"' -nofileinfo -verbose
    run_script_case "find nofileinfo nocontext" 'int a = foo;\n' 'find "foo"' -nofileinfo -nocontext
    run_script_case "replace nofileinfo keeps prefix" 'int a = foo;\n' 'replace "foo" with "X"' -nofileinfo -verbose

    echo "== script: nocontext =="
    run_script_case "nocontext find" 'int a = foo;\n' 'find "foo"' -verbose -nocontext
    run_script_case "nocontext find quiet" 'int a = foo;\n' 'find "foo"' -nocontext
    run_script_case "nocontext find multiword" 'int a = foo bar;\n' 'find "foo bar"' -verbose -nocontext
    run_script_case "nocontext replace" 'int a = foo;\n' 'replace "foo" with "X"' -verbose -nocontext
    run_script_case "nocontext without" 'int a = foo;\n' 'find "foo"' -verbose
}

run_cmd() {
    echo "== cmd: leading dash rule keywords =="
    run_cmd_case "dash find" 'int a = foo;\n' -verbose -find foo
    run_cmd_case "dash replace" 'int a = foo;\n' -verbose -replace foo with X
    run_cmd_case "dash replacemethod" '[obj pq:1]\n' -verbose -replacemethod pq: with rpq:
    run_cmd_case "bare find" 'int a = foo;\n' -verbose find foo
    run_cmd_case "bare replace" 'int a = foo;\n' -verbose replace foo with X
    run_cmd_case "dash find quiet" 'int a = foo;\n' -find foo
    run_cmd_case "dash nocontext" 'int a = foo;\n' -verbose -nocontext -find foo

    echo "== cmd: quiet mode still rewrites, dont reports anyway =="
    run_cmd_case "dash replace quiet" 'int a = foo;\n' -replace foo with X
    run_cmd_case "dash replacemethod quiet" '[obj pq:1]\n' -replacemethod pq: with rpq:
    run_cmd_case "dash find nofileinfo" 'int a = foo;\n' -nofileinfo -find foo
    run_cmd_case "dash find nofileinfo verbose" 'int a = foo;\n' -nofileinfo -verbose -find foo
    run_cmd_case "dash replace dont" 'int a = foo;\n' -dont -replace foo with X
    run_cmd_case "dash replace dont semiverbose" 'int a = foo;\n' -dont -semiverbose -replace foo with X

    echo "== cmd: error and warning operands =="
    run_cmd_case "error unquoted message" 'foo\n' -verbose replace foo with X error boom
    run_cmd_case "warning unquoted message" 'foo\n' -verbose replace foo with X warning warn
    run_cmd_case "error and warning" 'foo\n' -verbose replace foo with X error boom warning warn
    run_cmd_case "error on dash replace" 'foo\n' -verbose -replace foo with X error boom
    run_cmd_case "error replacemethod" '[obj pq:1]\n' -verbose -replacemethod pq: with rpq: error boom
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
    --all) run_non_tty; run_script; run_cmd; run_tty ;;
    --script) run_script; run_cmd ;;
    *) run_non_tty; run_script; run_cmd ;;
esac

echo
echo "PASS=$PASS FAIL=$FAIL"
if [ "$FAIL" != 0 ]; then
    echo "failed:$FAILED"
    exit 1
fi
exit 0
