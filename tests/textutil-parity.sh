#!/bin/bash
# Byte-parity harness: /usr/bin/textutil (oracle) vs our clean-room textutil.
# Compares stdout, stderr, exit status, the set of paths produced, and the
# bytes of every file and directory in the case directory.  Self-contained: no
# state outside its temp directory and no fixture files checked into the tree.
#
#   ./tests/textutil-parity.sh
#   make textutil-parity
#
# MY defaults to the Makefile release binary; override with MY=... .  A relative
# MY is resolved against the project root, so the Makefile can pass a short path
# (neither make variant allows $(shell)/$(CURDIR)).  ORACLE can be overridden to
# compare against a different reference build.
#
# Requires bash.  The fixtures are laid down with printf and a small repeat
# helper.  python3 is needed only by the docx and odt cases, whose ZIP
# timestamps are normalized before the comparison; see zipfix below.
#
# Scope.  This harness covers the plain text surface: the option parser, -info,
# and the txt, rtf, rtfd, html, wordml, webarchive, docx and odt writers.  The
# doc writer is recognised by the parser so that the option surface matches,
# but is not written by this port, so it is not compared here; see
# src/textutil/NOTES.md.

ORACLE=${ORACLE:-/usr/bin/textutil}
HERE=$(cd "$(dirname "$0")" && pwd)
PROJROOT=$(cd "$HERE/.." && pwd)
MY=${MY:-build/release/textutil}
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

ROOT=$(mktemp -d "${TMPDIR:-/tmp}/textutil-parity.XXXXXX") || exit 1
trap 'rm -rf "$ROOT"' EXIT

PASS=0
FAIL=0
FAILED=""

# NORMALIZE, when set, is run inside each of the two case trees after the tool
# and before anything is compared, for an output whose bytes hold a value that
# is not a property of the document.  The office containers are ZIP archives
# and every entry in one carries the DOS time it was written at, which is the
# wall clock of the run rather than anything the text said, so the two tools
# can never agree on those four bytes.  zipfix zeroes them in place, in the
# local header and in the central directory, and what is left is everything
# else in the archive -- which is the whole of the comparison, since the
# battery found no other difference.  Only the docx and odt cases set it, and
# they are the only ones that need python3.
NORMALIZE=""
if command -v python3 >/dev/null 2>&1; then
    HAVE_PY=1
else
    HAVE_PY=0
fi
cat >"$ROOT/zipfix.py" <<'PYEOF'
import os, struct, sys
for root, dirs, files in os.walk(sys.argv[1]):
    for name in files:
        if not (name.endswith(".docx") or name.endswith(".odt")):
            continue
        path = os.path.join(root, name)
        b = bytearray(open(path, "rb").read())
        eocd = b.rfind(b"PK\x05\x06")
        if eocd < 0:
            continue
        cd_off = struct.unpack_from("<I", b, eocd + 16)[0]
        cd_len = struct.unpack_from("<I", b, eocd + 12)[0]
        pos = cd_off
        end = cd_off + cd_len
        while pos < end:
            if struct.unpack_from("<I", b, pos)[0] != 0x02014b50:
                break
            local = struct.unpack_from("<I", b, pos + 42)[0]
            b[pos + 12:pos + 16] = b"\0" * 4
            b[local + 10:local + 14] = b"\0" * 4
            n, m, c = struct.unpack_from("<HHH", b, pos + 28)
            pos += 46 + n + m + c
        open(path, "wb").write(bytes(b))
PYEOF
zipfix() {
    python3 "$ROOT/zipfix.py" .
}

check_read() {
    READFIX="mkread" check "$@"
}

# check_zip is check with the ZIP timestamps taken out of the comparison, for
# the two office containers.  Everything else about the case is compared as it
# is everywhere else, bytes included.
check_zip() {
    NORMALIZE='zipfix' check "$@"
}

# rep <char> <count>: a run of one character, used to build the fixtures that
# sit either side of -info's 30 character preview cut.
rep() {
    local ch="$1" n="$2" out="" i=0
    while [ "$i" -lt "$n" ]; do
        out="$out$ch"
        i=$((i + 1))
    done
    printf '%s' "$out"
}

# The fixture set.  Every case starts from this same tree so that a case can
# name whichever input it needs, and so that both tools see identical bytes.
#
# The naming is deliberate: "line2" and "nonl" differ only in their trailing
# newline, "n29"/"n30"/"n31"/"n40" bracket the preview cut, and "eol1"/"eol2"
# separate a lone newline from a second one, which is the difference between a
# preview with and without an ellipsis.
mkfix() {
    local d="$1"
    printf 'Hello, world.\nSecond line.\n' >"$d/line2.txt"
    printf 'no trailing newline' >"$d/nonl.txt"
    : >"$d/empty.txt"
    printf 'x' >"$d/one.txt"
    printf 'a\n' >"$d/eol1.txt"
    printf 'a\n\n' >"$d/eol2.txt"
    printf 'a\nb\n' >"$d/two3.txt"
    rep y 29 >"$d/n29.txt"
    rep y 30 >"$d/n30.txt"
    rep y 31 >"$d/n31.txt"
    rep y 40 >"$d/n40.txt"
    rep y 30 >"$d/head30.txt"
    printf '\n' >>"$d/head30.txt"
    printf 'z' >>"$d/head30.txt"
    printf 'a<b & c>d\n' >"$d/amp.txt"
    printf 'A\\B {C} D\tE\nF\rG\n' >"$d/special.txt"
    printf 'a\n\n\nb\n' >"$d/blank.txt"
    printf '  \n' >"$d/wsonly.txt"
    printf '  indented\n' >"$d/indent.txt"
    printf 'tab\there\n' >"$d/tabs.txt"
    printf 'caf\xc3\xa9 na\xc3\xafve\n' >"$d/accent.txt"
    printf '\xe4\xb8\xad\xe6\x96\x87 \xf0\x9f\x98\x80\n' >"$d/cjk.txt"
    printf '\xef\xbb\xbfHello\n' >"$d/bom8.txt"
    printf '\xff\xfeH\x00i\x00\n\x00' >"$d/bom16le.txt"
    printf '\xfe\xff\x00H\x00i\x00\n' >"$d/bom16be.txt"
    # Buffers that are not UTF-8, which the reference tool reads as Mac OS Roman
    # rather than repairing.  The whole buffer is the fallback, so the valid é
    # in macutf8 comes back as the two Mac OS Roman characters its own bytes
    # spell, and not as é, and a byte sequence that is one byte short of UTF-8
    # is not the character it was reaching for either.
    printf 'a\xc3\xa9\xe2b\n' >"$d/macutf8.txt"
    printf 'a\x80\x81b\xffc3d\n' >"$d/macmix.txt"
    # Every byte from 0x80 up, once with a line feed after each so that each one
    # stands alone in the file, and once with none so that all 128 of them are
    # one run.  0xA9 is the one byte the reference tool will not read as Mac OS
    # Roman, and the two files pin both sides of that: it means © in the table,
    # and it does read as © as soon as the file holds any other byte above 0x7F,
    # so the 0xA9 on its own line of macall is © and not the U+FFFD that maca9,
    # whose only bytes above 0x7F are 0xA9, gets for all three of its 0xA9.
    : >"$d/macall.txt"
    : >"$d/macrun.txt"
    i=128
    while [ "$i" -lt 256 ]; do
        b=$(printf '\\%03o' "$i")
        printf "$b\n" >>"$d/macall.txt"
        printf "$b" >>"$d/macrun.txt"
        i=$((i + 1))
    done
    printf '\xa9\xa9\xa9\n' >"$d/maca9.txt"
    printf 'A\xa9A\n' >"$d/macquote.txt"
    # A well formed sequence keeps 0xA9 from condemning the text, so a file that
    # is ASCII, 0xA9 and valid UTF-8 and nothing else is read as UTF-8, and its
    # 0xA9 bytes are the only thing in it that is repaired.  macutf8a9 is U+FFFD,
    # ì and U+FFFD, and the well formed \303\254 in it is what keeps both 0xA9
    # from being read from the table, so the two files pin both sides: macquote,
    # which is the same text without the sequence, has the same two repairs, and
    # macseq, which has a sequence one byte short of complete, is Mac OS Roman
    # and spells all three of its own bytes: © é and nothing repaired.
    printf '\xa9\xc3\xac\xa9\n' >"$d/macutf8a9.txt"
    printf '\xa9\xc3\xa9\n' >"$d/macseq.txt"
    # The overlong form is not a sequence at all, so it does not keep 0xA9 from
    # condemning the text, and it is read from the table along with the byte
    # beside it: 0xA9 0xC0 0x80 is © and ¿Ä.
    printf '\xa9\xc0\x80\n' >"$d/macoverlong.txt"
    # A UTF-8 mark in front of text that is not UTF-8.  The mark does not pick
    # the encoding on its own: the whole buffer has to be UTF-8 for the mark to
    # be dropped, and when it is not the mark is read as text along with
    # everything else, so bommac has five characters and not the two its last
    # two bytes spell.  The 0xA9 rule is the one thing weighed without the
    # mark, and when it fires the mark goes with it, so boma9 is A and U+FFFD
    # and not three Mac OS Roman characters.
    printf '\xef\xbb\xbf\x80\x81' >"$d/bommac.txt"
    printf '\xef\xbb\xbfA\xa9' >"$d/boma9.txt"
    # A NUL and the control characters either side of a high byte, which reach
    # the output as themselves in every format and are not dropped as text.
    printf 'a\x00\x01\x07\x1b\x7f\x80z' >"$d/ctl.txt"
    # A run of blanks is rebuilt by the HTML writer a pair at a time, so where
    # the run sits in its line decides what the odd space left over becomes: a
    # no-break space at the head of the line, a space as typed between two
    # words, and a no-break space past the pairs at the end of the line.  spcrun
    # has a run of each width in each of the three places, and the single space
    # between two words is in it because that one is not rebuilt at all.
    printf '   A\nA   \nA   B\nA B\n  A  B  \n' >"$d/spcrun.txt"
    # A line of nothing but blanks is a paragraph of its own class rather than a
    # break, and it closes like a run at the end of a line does but keeps the
    # no-break space that on a line with words would fall outside the span.
    printf 'A\n  \n   \nB\n' >"$d/spcblank.txt"
    # A tab sits inside the run of blanks around it and is written as a tab of
    # its own, but it is not a space, so it neither makes a pair nor takes one
    # away, and a run either side of one keeps the pairs the whole run would
    # have had.  It does move the run off the head of its line, so a run that
    # would have led the line is treated as one between two words, and a blank
    # line that starts with a tab fills like a run closing a line.  spctab has
    # both widths of run at both sides of a tab, in each of the places a run can
    # sit, and the runs of a blank line with and without a leading tab.
    printf '  A\nA  \n A \nA B\nA  B\n  A  \n \t \n\t \n\t  \nA\t\tB\n' \
        >"$d/spctab.txt"
    # A line is written with dir="rtl" when the first character in it that
    # carries a direction of its own is written right to left.  A letter of the
    # left to right kind ahead of one of the other kind settles the line then
    # and there, and the digits and marks ahead of one settle nothing.  A left
    # or right to left mark at the head of a line is left out of the line and is
    # what gave the line its direction, so a line of one mark and one space is a
    # blank line, and a line of two marks keeps the second of them.  A line of
    # nothing but a mark is an empty line, and bidionly and bidiend are the two
    # ends of the text that an empty line is not a line: with no terminator
    # closing it, the last one is not written at all.
    printf '\xd7\x90\nA\xd7\x90\n\xd7\x90A\n\xe2\x80\x8fA\n\xe2\x80\x8eA\nA\xe2\x80\x8f\n\xe2\x80\x8f\rA\n\xe2\x80\x8f\xe2\x80\x8fA\n\xe2\x80\x8f \n1.\xd7\x90\n\xd9\xa1\n' \
        >"$d/bidi.txt"
    printf '\xe2\x80\x8f' >"$d/bidionly.txt"
    printf 'A\r\xe2\x80\x8f' >"$d/bidiend.txt"
    # A mark that opens a paragraph is taken out of the text and put into the
    # paragraph's properties, and there the two marks are not the same thing:
    # the right-to-left one names the direction and is recorded, the
    # left-to-right one names the direction the paragraph already had, which
    # the reference tool records by writing no direction word at all.  RTF
    # writes a paragraph's properties when they are first needed and again
    # whenever they change, so a document that runs plain, right to left and
    # plain again carries the block three times, and two paragraphs that agree
    # carry one block between them.  bidimark is that shape, bidimarkhead opens
    # with a mark rather than in the middle, and bidimarktwice has a line of two
    # marks, of which only the first is the paragraph's.
    printf 'A\n\xe2\x80\x8fB\nC\n\xe2\x80\x8eD\n\xe2\x80\x8fE\n\xe2\x80\x8fF\n' \
        >"$d/bidimark.txt"
    printf '\xe2\x80\x8fA\nB\n' >"$d/bidimarkhead.txt"
    printf '\xe2\x80\x8f\xe2\x80\x8fA\n\xe2\x80\x8e\xe2\x80\x8eB\n' \
        >"$d/bidimarktwice.txt"
    # The embedding controls are directions too, but unlike the two marks they
    # can open over a stretch of the text and nest, so no writer is told a
    # paragraph's direction by one alone: the RTF and WordML writers keep them
    # as levels and cut a run where the levels over the text change, and the
    # HTML writer names each stretch in a style.  embedone is a level around
    # one stretch and embedall one round the whole paragraph; embednest opens
    # one inside another and closes them one at a time, and embedtwo is the
    # nesting pair.  embedmerge opens and closes a level with no text between
    # it, which leaves the text either side under one level and not two, and
    # embedopen is a level the text never closes.  The two page breaks are a
    # level either side of one, which takes the reader's state with it, and a
    # level with the break inside it.
    printf 'A\xe2\x80\xaaB\xe2\x80\xacC\n' >"$d/embedone.txt"
    printf '\xe2\x80\xaaA\xe2\x80\xac\n' >"$d/embedall.txt"
    printf 'A\xe2\x80\xabB\xe2\x80\xacC\xe2\x80\xacD\n' >"$d/embednest.txt"
    printf '\xe2\x80\xaaA\xe2\x80\xac\xe2\x80\xaaB\xe2\x80\xac\n' >"$d/embedmerge.txt"
    printf '\xe2\x80\xaaA\xe2\x80\xaaB\xe2\x80\xac\xe2\x80\xac\n' >"$d/embedtwo.txt"
    printf 'A\xe2\x80\xaaB\n' >"$d/embedopen.txt"
    printf '\xe2\x80\xaaA\fB\n' >"$d/embedpage.txt"
    printf '\xe2\x80\xaa\f\xe2\x80\xacA\n' >"$d/embedpage2.txt"
    # A mark is at the head of a paragraph only when it is the very first thing
    # in it, and an embedding control in front of it means the paragraph began
    # with that instead, so the mark after it is text.  markctlhead is a control
    # and then a mark at what would otherwise be a head, and markctlmid puts a
    # mark after a control in the middle of a line, where it is text either way.
    printf '\xe2\x80\xaa\xe2\x80\x8eA\n' >"$d/markctlhead.txt"
    printf 'A\xe2\x80\xaa\xe2\x80\x8eB\n' >"$d/markctlmid.txt"
    # The head of the second of two paragraphs is a head again, so the mark
    # there names a direction too, and a paragraph of a mark alone is a
    # paragraph of nothing once the mark has named it.
    printf '\xe2\x80\x8eA\n\xe2\x80\x8eB\n' >"$d/marktwopara.txt"
    printf '\xe2\x80\x8e\n' >"$d/markonlyhead.txt"
    # markcr, markcrlf, markps and markls are the four ways a line can end and
    # a mark follow it.  A CR, an LF, a CRLF pair and a U+2029 each end a
    # paragraph and so put the next one at a head; a U+2028 is a break inside
    # the paragraph and does not, so the mark after it is text.
    printf 'A\r\xe2\x80\x8eB\n' >"$d/markcr.txt"
    printf 'A\r\n\xe2\x80\x8eB\n' >"$d/markcrlf.txt"
    printf 'A\xe2\x80\xa9\xe2\x80\x8eB\n' >"$d/markps.txt"
    printf 'A\xe2\x80\xa8\xe2\x80\x8eB\n' >"$d/markls.txt"
    # A CR pairs with the LF after it into one terminator, and what may lie
    # between the two halves of that pair is the question.  markinpair and
    # markinpairrlm are one mark, ctlpair and pairctl2 are controls, and
    # pairmarkctl is a mark and then a control, so a mark is taken at the head
    # of the line the CR begins and the controls are gone over after it.
    # pairmarkalm adds a mark that is text and so is in the way.  pairctlmark
    # puts a control before the mark, which stops the mark being at the head,
    # and pairmark2 puts a second mark there, which is the other thing that
    # does.  Neither of those two is a pair.
    printf 'A\r\xe2\x80\x8e\nB\n' >"$d/markinpair.txt"
    printf 'A\r\xe2\x80\x8f\nB\n' >"$d/markinpairrlm.txt"
    printf 'A\r\xe2\x80\xaa\nB\n' >"$d/ctlpair.txt"
    printf 'A\r\xe2\x80\xaa\xe2\x80\xac\nB\n' >"$d/pairctl2.txt"
    printf 'A\r\xe2\x80\x8e\xe2\x80\xaa\nB\n' >"$d/pairmarkctl.txt"
    printf 'A\r\xe2\x80\x8e\xe2\x98\x9c\nB\n' >"$d/pairmarkalm.txt"
    printf 'A\r\xe2\x80\xaa\xe2\x80\x8e\nB\n' >"$d/pairctlmark.txt"
    printf 'A\r\xe2\x80\x8e\xe2\x80\x8e\nB\n' >"$d/pairmark2.txt"
    printf 'one\r\ntwo\r\n' >"$d/crlf.txt"
    # A NUL and a run of blanks.  A NUL is not shown and it is not a blank,
    # but it leaves the blanks after it at the head of their line, so they are
    # counted as leading however much text came before it: nulrearm has one
    # blank either side of it and a word, nulrearmthree three blanks, nulrearmat
    # a NUL at the head of a line with blanks on both sides of them, and
    # nulrearmtail ends the line with blanks after the NUL.
    printf 'A\0 B\n' >"$d/nulrearm.txt"
    printf 'A\0   B\n' >"$d/nulrearmthree.txt"
    printf '  \0  \n' >"$d/nulrearmat.txt"
    printf '\0  \n' >"$d/nulrearmtail.txt"
    # A C0 control other than a NUL, and blanks after it.  It is not shown and
    # it is not a blank either, but the blanks after it are counted at the head
    # of their line where one space, and one space only, separates it from text
    # the reader is shown, and between two words otherwise.  ctlrearmword has
    # that one space with a word before it, ctlrearmtwo has two spaces,
    # ctlrearmindent has the one space at the head of the line where it is an
    # indent rather than a word gap, ctlrearmtabgap has a tab before the space,
    # ctlrearmlevel a level before it, ctlrearmtail blanks at the end of the
    # line, and ctlrearmbs is the first of these over a backspace.
    printf 'A \001 B\n' >"$d/ctlrearmword.txt"
    printf 'A  \001 B\n' >"$d/ctlrearmtwo.txt"
    printf ' \001 B\n' >"$d/ctlrearmindent.txt"
    printf 'A\t \001 B\n' >"$d/ctlrearmtabgap.txt"
    printf '\xe2\x80\xaa \001 B\n' >"$d/ctlrearmlevel.txt"
    printf 'A \001  \n' >"$d/ctlrearmtail.txt"
    printf 'A \010 B\n' >"$d/ctlrearmbs.txt"
    # A level that opens and one that closes inside a region of blanks.  The
    # blanks are counted over the whole of the region and the level the no-break
    # spaces were written under stays open past the end of it, so the spaces and
    # the text after them share one span rather than the span being closed and
    # opened again.  lvlrearmclosemark, lvlrearmclosenul and lvlrearmcloseemoji
    # are that shape ending in a mark, in a NUL and in a character that is a
    # block of its own, and lvlrearmcloseends is the same shape with nothing
    # after the region at all, where the only span left open is the one the
    # paragraph itself closes.
    printf '\xe2\x80\xae \xe2\x80\xab\xe2\x80\xac\xd8\x9c\n' >"$d/lvlrearmclosemark.txt"
    printf '\xe2\x80\xad\t\xe2\x80\xab\xe2\x80\xac\000\n' >"$d/lvlrearmclosenul.txt"
    printf '\xe2\x80\xab \xe2\x80\xae\xe2\x80\xac\xf0\x9f\x98\x80\n' >"$d/lvlrearmcloseemoji.txt"
    printf '\xe2\x80\xab \xe2\x80\xae\xe2\x80\xac\n' >"$d/lvlrearmcloseends.txt"
    # A lone carriage return between two lines, which is a paragraph mark of
    # its own, and the two Unicode line and paragraph separators.  U+2028
    # ends a line inside a paragraph and U+2029 ends the paragraph, so these
    # are the same bytes as crlf.txt with one separator changed and one with
    # each of the two kinds on its own.  A line feed followed by a carriage
    # return is two marks where a carriage return followed by a line feed is
    # one, so lfcr and crlf are the pair that tells the two apart.
    printf 'one\rtwo\n' >"$d/cronly.txt"
    printf 'a\n\rb\n' >"$d/lfcr.txt"
    printf 'a\r\nb\n' >"$d/crlfnl.txt"
    printf 'a\n\r' >"$d/lfcrend.txt"
    printf 'a\xe2\x80\xa8b\n' >"$d/ls.txt"
    printf 'a\xe2\x80\xa9b\n' >"$d/ps.txt"
    printf '\xe2\x80\xa8' >"$d/lsonly.txt"
    printf '\xe2\x80\xa9' >"$d/psonly.txt"
    printf 'a\xe2\x80\xa8\xe2\x80\xa8b\n' >"$d/ls2.txt"
    printf 'a\xe2\x80\xa9\xe2\x80\xa9b\n' >"$d/ps2.txt"
    # A tab on its own, and one at each end of a line, so that the empty text
    # either side of a tab is written rather than skipped.
    printf '\t\n' >"$d/tabonly.txt"
    printf 'a\t\n\tb\n' >"$d/tabends.txt"
    # A subdirectory, so that output naming can be checked for a path.
    mkdir -p "$d/sub"
    printf 'nested\n' >"$d/sub/n.txt"
    # A directory to hand to a tool, a second one with a space in its name, and
    # a symlink pointing at a directory.  A path that is a directory is a read
    # failure rather than an empty document.
    mkdir -p "$d/adir" "$d/sp ace" "$d/rodir"
    ln -s adir "$d/slink"
    # Readable but not writable, so that a destination inside it is refused.
    chmod 500 "$d/rodir"
    # A file whose name looks like an option, and a file with a space in it.
    printf 'dashname\n' >"$d/-weird.txt"
    printf 'spaced\n' >"$d/has space.txt"
    printf 'noext\n' >"$d/noext"
    printf 'hidden\n' >"$d/.hidden"
    # Multibyte first lines either side of the 30 character preview cut.  A
    # character is one character however many bytes it takes, so a CJK
    # character and a supplementary one each occupy one of the 30 slots while
    # taking 3 and 4 bytes respectively.  These are the cases that catch a
    # preview which counts lead bytes but then prints only those.
    rep "$(printf '\xe4\xb8\xad')" 31 >"$d/cjk31"
    printf '\n' >>"$d/cjk31"
    rep "$(printf '\xe4\xb8\xad')" 30 >"$d/cjk30"
    printf '\n' >>"$d/cjk30"
    rep "$(printf '\xf0\x9f\x98\x80')" 31 >"$d/emoji31"
    printf '\n' >>"$d/emoji31"
    rep "$(printf '\xf0\x9f\x98\x80')" 30 >"$d/emoji30"
    printf '\n' >>"$d/emoji30"
    printf '\xf0\x9f\x98\x80' >"$d/emojilead"
    rep y 28 >>"$d/emojilead"
    printf '\n' >>"$d/emojilead"
    printf '\xf0\x9f\x98\x80' >"$d/emojitail"
    rep y 29 >>"$d/emojitail"
    printf '\n' >>"$d/emojitail"
    # A multibyte first line that is not truncated, so the whole line is shown
    # with its newline rather than an ellipsis.
    printf 'caf\xc3\xa9 na\xc3\xafve\n' >"$d/accentline"
    # The 30 unit cut landing exactly on a character boundary (28 y plus one
    # emoji is 30 units, so all of it is shown) and one unit past it (29 y plus
    # one emoji is 31, so the cut falls inside the emoji).
    printf '\xf0\x9f\x98\x80' >"$d/units30"
    rep y 28 >>"$d/units30"
    printf '\n' >>"$d/units30"
    printf '\xf0\x9f\x98\x80' >"$d/units31"
    rep y 29 >>"$d/units31"
    printf '\n' >>"$d/units31"
    # The same split reached with BMP characters first, so the boundary does
    # not depend on how the 29 units were spent.
    rep "$(printf '\xe4\xb8\xad')" 29 >"$d/units32"
    printf '\xf0\x9f\x98\x80' >>"$d/units32"
    printf '\n' >>"$d/units32"
    rep "$(printf '\xe4\xb8\xad')" 28 >"$d/units33"
    printf '\xf0\x9f\x98\x80' >>"$d/units33"
    printf '\xf0\x9f\x98\x80' >>"$d/units33"
    printf '\n' >>"$d/units33"
    printf '\xf0\x9f\x98\x80' >"$d/units34"
    rep y 28 >>"$d/units34"
    printf '\xf0\x9f\x98\x80' >>"$d/units34"
    printf '\n' >>"$d/units34"
    printf '\xf0\x9f\x98\x80\ntail\n' >"$d/emojione"
}

# Run one case.  The label is only for reporting; everything after it is passed
# to both tools verbatim.  Each tool runs in a subdirectory of the same name, so
# that a diagnostic naming the working folder says the same thing for both.  A
# non-empty PREP is run inside each directory before the tool, for a fixture the
# shared setup cannot hold: an unreadable file has to be created per case,
# because the tree comparison below could not read one left in the shared setup.
# The files the reader cases read.  They are written once, by the reference
# tool, and then copied into each case tree, so that the two tools are reading
# the same bytes rather than each other's work.
mktpl() {
    local t="$ROOT/tpl"
    rm -rf "$t"
    mkdir -p "$t"
    ( cd "$t" && mkfix . )
    ( cd "$t" && "$ORACLE" -convert rtf -output one.rtf line2.txt )
    ( cd "$t" && "$ORACLE" -convert html -output one.html line2.txt )
    ( cd "$t" && "$ORACLE" -convert rtfd -output one.rtfd line2.txt )
    ( cd "$t" && "$ORACLE" -convert rtf -output meta.rtf \
        -title T -author A -editor E -company C -subject S -comment M \
        -creationtime 2024-01-02T03:04:05Z \
        -modificationtime 2024-01-02T03:04:05Z line2.txt )
    ( cd "$t" && "$ORACLE" -convert rtfd -output meta.rtfd \
        -title T -author A -editor E -company C -subject S -comment M \
        -creationtime 2024-01-02T03:04:05Z \
        -modificationtime 2024-01-02T03:04:05Z line2.txt )
    ( cd "$t" && "$ORACLE" -convert html -output meta.html \
        -author A -editor E -company C -subject S -comment M \
        -creationtime 2024-01-02T03:04:05Z \
        -modificationtime 2024-01-02T03:04:05Z line2.txt )
    # What an RTF file has to be before it is read at all.
    printf 'not rtf at all\n' >"$t/plain.rtf"
    printf '{\\rtf1 an unclosed group' >"$t/open.rtf"
    printf '{\\rtf1 a group}\\}' >"$t/esc.rtf"
    printf '{\\rtf1 a group}}' >"$t/extra.rtf"
    printf '{\\rtf1 {a nested group}}' >"$t/nest.rtf"
    # A bundle is a folder with a TXT.rtf in it, and each of these says
    # otherwise.
    mkdir -p "$t/empty.rtfd"
    mkdir -p "$t/blank.rtfd"; : >"$t/blank.rtfd/TXT.rtf"
    mkdir -p "$t/junk.rtfd"; printf 'junk\n' >"$t/junk.rtfd/TXT.rtf"
    mkdir -p "$t/trunc.rtfd"
    printf '{\\rtf1 an unclosed group' >"$t/trunc.rtfd/TXT.rtf"
    mkdir -p "$t/upper.RTFD"
    printf '{\\rtf1 the name in capitals}' >"$t/upper.RTFD/TXT.rtf"
    # A folder that is not a bundle, and a file named as one.
    mkdir -p "$t/plaindir.rtfd/x"
    printf '{\\rtf1 a file, not a folder}' >"$t/filer.rtfd"
    # A recognised name is read as what it says it is, whatever is in it.
    cp "$t/one.html" "$t/lying.rtf"
    cp "$t/one.rtf" "$t/lying.html"
    cp "$t/one.rtf" "$t/lying.txt"
    # A document with a doctype in it under a name that is not a format at all.
    # This is the one way a doctype makes something HTML, since a .doc or a
    # .txt or a .foo are all read as plain text whatever they hold.  The text
    # is inline rather than in a block, because a block paragraph is one of
    # the known divergences in src/textutil/NOTES.md and this is a case about
    # the name and not about the layout.
    printf '<!doctype html>a doctype' >"$t/docty"
    cp "$t/docty" "$t/docty.foo"
    cp "$t/docty" "$t/docty.txt"
    cp "$t/docty" "$t/docty.rtf"
    cp "$t/docty" "$t/docty.doc"
    # The same document, without the paragraphs, under a name that is not a
    # format.  These are about the name and about the doctype, and one.html
    # holds <p> elements, which is one of the known divergences in
    # src/textutil/NOTES.md and is read back elsewhere.
    printf '<html>a tag' >"$t/inl"
    cp "$t/inl" "$t/noext"
    cp "$t/inl" "$t/inl.htm"
    cp "$t/inl" "$t/inl.odt"
    # The names that are read rather than sniffed, and the ones that are not.
    # .htm is a second spelling of .html, and the Word container names are
    # believed where .rtfd and .webarchive are: a .doc holding RTF is read as
    # RTF, so its name decides nothing.  .htm gets inl and not one.html, for
    # the reason given above.
    cp "$t/one.rtf" "$t/htm.rtf"
    cp "$t/one.rtf" "$t/upper.HTM"
    cp "$t/one.rtf" "$t/lying.doc"
    cp "$t/one.rtf" "$t/lying.docx"
    cp "$t/one.rtf" "$t/lying.wordml"
    cp "$t/one.rtf" "$t/filer.rtfd"
    # Bytes that decide a file with no format in its name, and bytes that do
    # not.  The RTF mark is five bytes and no version digit, it is case
    # sensitive, and only a newline or a carriage return may come before it;
    # HTML is the opposite on both, but a doctype alone is not enough to be
    # one.  Passing the gate is not the last word either: the file is read
    # with the reader the gate chose, and a read that fails falls back to
    # plain text, which is what snr4, snr7, snr8 and snr11 are for.
    printf '{\\rtf1 a mark at the start}' >"$t/snr1"
    printf '{\\rtf9 any digit will do}' >"$t/snr2"
    printf '{ a brace with no mark}' >"$t/snr3"
    printf '{rtf and no brace}' >"$t/snr4"
    printf '{\\rtf and no digit}' >"$t/snr5"
    printf '{\\RTF1 in capitals}' >"$t/snr6"
    printf ' {\\rtf1 marked but late}' >"$t/snr7"
    printf '\n\t{\\rtf1 marked and indented}' >"$t/snr8"
    # A leading newline may come before the mark and a leading tab may not,
    # which is the reader's rule and not the gate's.  Of the eight above only
    # snr5 gets as far as the reader, and the reader takes it, so the six that
    # come back as plain text are the four that fail the gate (snr3 and snr4
    # lack the backslash, snr6 is in capitals, snr7 and snr8 are behind
    # something the mark does not allow) plus the two that do open a file and
    # then fail to read it, below.
    printf '\n{\\rtf1 a mark on the second line}' >"$t/snr9"
    printf '\t{\\rtf1 a mark after a tab}' >"$t/snr10"
    # An unclosed group, which is a mark and then a file that falls apart, and
    # the same group closed, which is the file the reader can open.
    printf '{\\rtf1 an unclosed group' >"$t/snr11"
    printf '{\\rtf1 \\bogus x}' >"$t/snr12"
    # A gate wants a byte past the spelling it matches, and the two gates want
    # different numbers of them: the mark alone is text and needs a sixth byte,
    # while "<html" needs two and every six byte document beginning with those
    # five is text, whether the sixth byte is a digit, a space or a bracket.
    printf '{\\rtf' >"$t/snr13"
    printf '{\\rtf}' >"$t/snr14"
    printf '{\\rtfa}' >"$t/snr15"
    printf '<html' >"$t/snh13"
    printf '<html5' >"$t/snh14"
    printf '<html5>' >"$t/snh15"
    printf '<html ' >"$t/snh16"
    printf '  <html>' >"$t/snh17"
    printf '<!doctype html' >"$t/snh18"
    printf '<!doctype html>' >"$t/snh19"
    printf '<html>a tag</html>' >"$t/snh1"
    printf '<HTML>a tag in capitals</HTML>' >"$t/snh2"
    printf '  <html>a tag indented</html>' >"$t/snh3"
    printf '<htmlfoo>a tag that only starts like one</htmlfoo>' >"$t/snh4"
    printf '<htm>a shorter tag</htm>' >"$t/snh5"
    printf '\n\n<html>a tag two lines down</html>' >"$t/snh11"
    printf '\t\t<html>a tag after two tabs</html>' >"$t/snh12"
    # An empty document, and one holding only a line break, which is the case
    # where -info drops its Contents field outright.
    : >"$t/empty"
    printf '\n' >"$t/brk"
    printf '\nafter a break\n' >"$t/afterbrk"
    # The text here is inline rather than in a block, because a block
    # paragraph is one of the known divergences in src/textutil/NOTES.md and
    # these cases are about which reader ran, not about how it lays out.
    printf '<!doctype html>a doctype' >"$t/snh6"
    printf '<!DOCTYPE HTML>a doctype in capitals' >"$t/snh7"
    printf '<!doctype x>a doctype that is not html' >"$t/snh8"
    printf '<!doctypehtml>no space is not html</!doctypehtml>' >"$t/snh9"
    printf 'x<html>not at the start</html>' >"$t/snh10"
    # An HTML entity is the tell that decides whether the HTML reader ran, and
    # a plain text file keeps it escaped.
    printf '&lt;b&gt;escaped&lt;/b&gt;\n' >"$t/ents"
    cp "$t/ents" "$t/ents.txt"
    cp "$t/ents" "$t/ents.html"
    cp "$t/ents" "$t/ents.htm"
    cp "$t/ents" "$t/ents.foo"
    cp "$t/one.rtf" "$t/upper.RTF"
    # A bundle and a plain folder, for the directory branch of the name test.
    mkdir -p "$t/realdir.rtfd"; printf '{\\rtf1 a real bundle}' >"$t/realdir.rtfd/TXT.rtf"
    mkdir -p "$t/realdir.HTM"
    # A name that is believed and a reader that is missing: the Type line is
    # the one the name gives, and the contents are read as text, which is what
    # the reference tool does to a webarchive that is not an archive.
    printf 'not an archive' >"$t/notarch.webarchive"
}

mkread() {
    cp -R "$ROOT/tpl"/. .
}

check() {
    local label="$1"
    shift
    local o="$ROOT/o" m="$ROOT/m"
    rm -rf "$o" "$m"
    mkdir -p "$o/run" "$m/run"
    mkfix "$o/run"
    mkfix "$m/run"
    if [ -n "$READFIX" ]; then
        ( cd "$o/run" && eval "$READFIX" )
        ( cd "$m/run" && eval "$READFIX" )
    fi
    if [ -n "$PREP" ]; then
        ( cd "$o/run" && eval "$PREP" )
        ( cd "$m/run" && eval "$PREP" )
    fi

    ( cd "$o/run" && "$ORACLE" "$@" >o.out 2>o.err; echo $? >o.rc )
    ( cd "$m/run" && "$MY" "$@" >o.out 2>o.err; echo $? >o.rc )

    if [ -n "$NORMALIZE" ]; then
        ( cd "$o/run" && eval "$NORMALIZE" )
        ( cd "$m/run" && eval "$NORMALIZE" )
    fi

    local why=""

    if ! cmp -s "$o/run/o.rc" "$m/run/o.rc"; then
        why="exit($(cat "$o/run/o.rc")/$(cat "$m/run/o.rc"))"
    fi
    if ! cmp -s "$o/run/o.out" "$m/run/o.out"; then
        why="$why stdout"
    fi
    if ! cmp -s "$o/run/o.err" "$m/run/o.err"; then
        why="$why stderr"
    fi

    # Compare the whole tree, so that a missing or extra output is caught as
    # well as differing bytes.  diff -r on directories compares members.  Two
    # fixtures are skipped, each for a reason that belongs to diff rather than
    # to the tools: diff cannot read noperm.txt, and the slink symlink points
    # at a sibling directory, which diff reports as a directory loop once per
    # root.  Both notes name the root, so they would differ when they are not
    # dropped.  The cases that use either fixture still compare its exit
    # status, its standard output and its standard error.
    local dl=""
    dl=$(diff -r -q "$o" "$m" 2>&1 |
        grep -v -e "o\.out" -e "o\.err" -e "o\.rc" -e "noperm\.txt" \
            -e "slink")
    if [ -n "$dl" ]; then
        why="$why tree"
    fi

    if [ -z "$why" ]; then
        PASS=$((PASS + 1))
    else
        FAIL=$((FAIL + 1))
        FAILED="$FAILED $label"
        echo "FAIL [$why] $label"
        if [ -n "$VERBOSE" ]; then
            echo "--- oracle stdout"; cat "$o/run/o.out"
            echo "--- mine stdout";   cat "$m/run/o.out"
            echo "--- oracle stderr"; cat "$o/run/o.err"
            echo "--- mine stderr";   cat "$m/run/o.err"
            [ -n "$dl" ] && echo "$dl"
        fi
    fi
}

# Run one case with something redirected onto standard input.  check_stdin
# exists because the redirect has to be applied to the subshell that runs each
# tool, and because a case that reads stdin must start from the same fixtures
# so that the named input is available by path.
check_stdin() {
    local label="$1" src="$2"
    shift 2
    local o="$ROOT/o" m="$ROOT/m"
    rm -rf "$o" "$m"
    mkdir -p "$o/run" "$m/run"
    mkfix "$o/run"
    mkfix "$m/run"

    ( cd "$o/run" && "$ORACLE" "$@" <"$src" >o.out 2>o.err; echo $? >o.rc )
    ( cd "$m/run" && "$MY" "$@" <"$src" >o.out 2>o.err; echo $? >o.rc )

    if [ -n "$NORMALIZE" ]; then
        ( cd "$o/run" && eval "$NORMALIZE" )
        ( cd "$m/run" && eval "$NORMALIZE" )
    fi

    local why=""

    cmp -s "$o/run/o.rc" "$m/run/o.rc" || why="exit($(cat "$o/run/o.rc")/$(cat "$m/run/o.rc"))"
    cmp -s "$o/run/o.out" "$m/run/o.out" || why="$why stdout"
    cmp -s "$o/run/o.err" "$m/run/o.err" || why="$why stderr"
    # The same two fixture exclusions as check, for the same reasons.
    local dl=""
    dl=$(diff -r -q "$o" "$m" 2>&1 |
        grep -v -e "o\.out" -e "o\.err" -e "o\.rc" -e "noperm\.txt" \
            -e "slink")
    [ -n "$dl" ] && why="$why tree"

    if [ -z "$why" ]; then
        PASS=$((PASS + 1))
    else
        FAIL=$((FAIL + 1))
        FAILED="$FAILED $label"
        echo "FAIL [$why] $label"
        if [ -n "$VERBOSE" ]; then
            echo "--- oracle stdout"; cat "$o/run/o.out"
            echo "--- mine stdout";   cat "$m/run/o.out"
            echo "--- oracle stderr"; cat "$o/run/o.err"
            echo "--- mine stderr";   cat "$m/run/o.err"
            [ -n "$dl" ] && echo "$dl"
        fi
    fi
}

# ---------------------------------------------------------------------------
# -stdin.  The conflict check is diagnosed ahead of the "no command" fallback,
# so a bare filename alongside -stdin is an error even with no command given.
# -info -stdin names the input "stdin" and omits Size, and a byte order mark on
# the redirected bytes is honoured just as it is in a file.
# ---------------------------------------------------------------------------
S=$ROOT/rd; rm -rf "$S"; mkdir -p "$S"
printf 'abc\ndef\n' >"$S/plain"
printf '\xef\xbb\xbfcaf\xc3\xa9\n' >"$S/bom8"
printf '' >"$S/empty"
: >"$S/zero"
# The files the reader cases read are written once here, by the reference
# tool, so that both sides of every case read the same bytes.
mktpl

check_stdin "stdin alone" "$S/zero" -stdin
check_stdin "stdin with a file" "$S/plain" -stdin line2.txt
check_stdin "stdin with a missing file" "$S/plain" -stdin nosuch
check_stdin "stdin no command with a file" "$S/plain" -convert txt -stdin
check_stdin "stdin and file ordered both ways" "$S/plain" line2.txt -stdin -convert txt
check_stdin "stdin and file with info" "$S/plain" -info -stdin line2.txt
check_stdin "stdin convert" "$S/plain" -convert txt -stdin
check_stdin "stdin convert stdout" "$S/plain" -convert txt -stdin -stdout
check_stdin "stdin convert output" "$S/plain" -convert txt -stdin -output named.txt
check_stdin "stdin convert extension" "$S/plain" -convert txt -stdin -extension zz
check_stdin "stdin cat" "$S/plain" -cat txt -stdin
check_stdin "stdin options first" "$S/plain" -stdin -stdout -convert txt
check_stdin "stdin rtf" "$S/plain" -convert rtf -stdin
check_stdin "stdin rtf font and size" "$S/plain" -convert rtf -stdin -font Courier -fontsize 9
check_stdin "stdin rtfd" "$S/plain" -convert rtfd -stdin
check_stdin "stdin html" "$S/plain" -convert html -stdin
check_stdin "stdin html title" "$S/plain" -convert html -stdin -title Hi
check_stdin "stdin webarchive" "$S/plain" -convert webarchive -stdin
check_stdin "stdin webarchive title" "$S/plain" -convert webarchive -stdin -title Hi
check_stdin "stdin info" "$S/plain" -info -stdin
check_stdin "stdin info two lines" "$S/plain" -info -stdin
check_stdin "stdin bom" "$S/bom8" -convert txt -stdin -stdout
check_stdin "stdin bom rtf" "$S/bom8" -convert rtf -stdin
check_stdin "stdin bom info" "$S/bom8" -info -stdin
check_stdin "stdin empty convert" "$S/empty" -convert txt -stdin -stdout
check_stdin "stdin empty rtf" "$S/empty" -convert rtf -stdin
check_stdin "stdin empty html" "$S/empty" -convert html -stdin
check_stdin "stdin empty info" "$S/empty" -info -stdin
printf 'one\r\ntwo\r\n' >"$S/crlf"
printf '\xf0\x9f\x98\x80\xe4\xb8\xad\n\xc3\xa9\xc3\xa8\n' >"$S/wide"
rep "$(printf '\xe4\xb8\xad')" 31 >"$S/cjk31"
printf '\n' >>"$S/cjk31"
rep "$(printf '\xf0\x9f\x98\x80')" 31 >"$S/emoji31"
printf '\n' >>"$S/emoji31"
printf '\xf0\x9f\x98\x80' >"$S/emojilead"
rep y 28 >>"$S/emojilead"
printf '\n' >>"$S/emojilead"
check_stdin "stdin crlf" "$S/crlf" -convert txt -stdin -stdout
check_stdin "stdin crlf rtf" "$S/crlf" -convert rtf -stdin
check_stdin "stdin crlf html" "$S/crlf" -convert html -stdin
check_stdin "stdin wide" "$S/wide" -convert rtf -stdin
check_stdin "stdin wide info" "$S/wide" -info -stdin
check_stdin "stdin wide html" "$S/wide" -convert html -stdin
check_stdin "stdin cjk31 info" "$S/cjk31" -info -stdin
check_stdin "stdin emoji31 info" "$S/emoji31" -info -stdin
check_stdin "stdin emojilead info" "$S/emojilead" -info -stdin
# The Mac OS Roman fallback, through -stdin, where the buffer arrives in a pipe
# rather than from a file and so is not known to be a whole one.
printf 'a\xc3\xa9\xe2b\n' >"$S/macutf8"
printf 'A\xa9A\n' >"$S/macquote"
i=128
: >"$S/macrun"
while [ "$i" -lt 256 ]; do
    printf "$(printf '\\%03o' "$i")" >>"$S/macrun"
    i=$((i + 1))
done
check_stdin "stdin macroman" "$S/macrun" -convert txt -stdin -stdout
check_stdin "stdin macroman rtf" "$S/macrun" -convert rtf -stdin
check_stdin "stdin macroman info" "$S/macrun" -info -stdin
check_stdin "stdin whole buffer fallback" "$S/macutf8" -convert txt -stdin -stdout
check_stdin "stdin quote run" "$S/macquote" -convert txt -stdin -stdout

# ---------------------------------------------------------------------------
# Usage, the default command, and argument errors.
# ---------------------------------------------------------------------------
check "no args"
check "help" -help
check "unknown option" -bogus line2.txt
check "unknown option alone" -bogus
check "double dash option" --format line2.txt
check "two commands" -info -convert txt line2.txt
check "two commands reversed" -convert txt -info line2.txt
check "same command twice" -info -info line2.txt
check "no files" -info
check "no files convert" -convert txt
check "no format cat" -cat
check "bad format" -convert nosuch line2.txt
check "bad format cat" -cat nosuch line2.txt
check "format looks like a file" -convert line2.txt
check "missing value output" -convert txt -output
check "missing value extension" -convert txt -extension
check "missing value encoding" -convert txt -encoding
check "missing value inputencoding" -convert txt -inputencoding
check "missing value format" -convert txt -format
check "missing value font" -convert txt -font
check "missing value fontsize" -convert txt -fontsize
check "missing value baseurl" -convert txt -baseurl
check "missing value timeout" -convert txt -timeout
check "missing value textsizemultiplier" -convert txt -textsizemultiplier
check "missing value excludedelements" -convert txt -excludedelements
check "missing value prefixspaces" -convert txt -prefixspaces
check "missing value title" -convert txt -title
check "missing value author" -convert txt -author
check "missing value subject" -convert txt -subject
check "missing value keywords" -convert txt -keywords
check "missing value comment" -convert txt -comment
check "missing value editor" -convert txt -editor
check "missing value company" -convert txt -company
check "missing value creationtime" -convert txt -creationtime
check "missing value modificationtime" -convert txt -modificationtime
check "missing file" -info nosuch.txt
check "missing file convert" -convert txt nosuch.txt
check "missing file amongst good" -convert txt line2.txt nosuch.txt nonl.txt
check "missing file info amongst good" -info line2.txt nosuch.txt nonl.txt
check "dash is a file name" -convert txt -
check "double dash ends options" -info -- -weird.txt
check "double dash then optionlike" -info -- -weird.txt --bogus

# ---------------------------------------------------------------------------
# Paths that are not readable files.  A directory is a read failure, not an
# empty document, and it is named by the last path component of the path as it
# was written, so ./sub/dir and sub/dir both quote "dir".  The three wordings
# are distinct: missing, no permission, and could not be opened.
# ---------------------------------------------------------------------------
check "directory is not a document" -convert txt -output d1.txt adir
check "directory named by component" -convert txt -output d2.txt ./sub/adir
check "directory with a space in its name" -convert txt -output d3.txt "sp ace"
check "symlink to a directory" -convert txt -output d4.txt slink
check "info on a directory" -info adir
check "info on a symlink to a directory" -info slink
check "directory amongst good files" -info line2.txt adir nonl.txt
check "cat on a directory" -cat txt -output c1.txt adir
check "directory amongst good files for cat" -cat txt -output c2.txt line2.txt adir
check "output into a directory path" -convert txt -output sub line2.txt

# ---------------------------------------------------------------------------
# Destinations that cannot be written.  A folder that is absent, a folder that
# may not be written to, a parent that is a file rather than a folder, and a
# destination that is already a directory each have their own wording.  The
# last quotes the destination's own name and the name of the folder holding it,
# which is why both tools run in a directory of the same name.
# ---------------------------------------------------------------------------
check "output folder does not exist" -convert txt -output nosuch/out.txt line2.txt
check "output parent is a file" -convert txt -output noext/out.txt line2.txt
check "output folder not writable" -convert txt -output rodir/out.txt line2.txt
check "html output folder not writable" -convert html -output rodir/out.html line2.txt
check "rtf output is a directory" -convert rtf -output adir line2.txt
check "html output is a directory" -convert html -output adir line2.txt
check "cat output is a directory" -cat txt -output adir line2.txt
# A bundle is a folder, so a folder already in the way is used as it stands, a
# missing parent is made, and a plain file in the way is replaced.
check "rtfd into an existing folder" -convert rtfd -output adir line2.txt
check "rtfd into a missing folder" -convert rtfd -output nosuch/bundle.rtfd line2.txt
check "rtfd onto a plain file" -convert rtfd -output noext line2.txt
# A file that exists but may not be opened needs its own fixture, since the
# tree comparison could not read one held in the shared setup.
PREP='printf "z\n" >noperm.txt; chmod 000 noperm.txt'
check "unreadable file convert" -convert txt -output n1.txt noperm.txt
check "unreadable file info" -info noperm.txt
check "unreadable file cat" -cat txt -output n2.txt noperm.txt
check "unreadable file amongst good" -info line2.txt noperm.txt nonl.txt
PREP=''

# ---------------------------------------------------------------------------
# -info, over every fixture shape that reaches the decoder.
# ---------------------------------------------------------------------------
for f in line2 nonl empty one eol1 eol2 two3 n29 n30 n31 n40 head30 amp \
         special blank wsonly indent tabs accent cjk bom8 bom16le bom16be \
         crlf noext macutf8 macmix macall macrun maca9 macquote macutf8a9 macseq macoverlong bommac boma9 ctl spcrun spcblank spctab; do
    check "info $f" -info "$f.txt"
done
check "info no extension" -info noext
# The 30 character cut seen through multibyte characters, where one character
# is 3 or 4 bytes.  A preview that counts lead bytes but prints only those
# passes every ASCII case above and fails all of these.
for f in cjk30 cjk31 emoji30 emoji31 emojilead emojitail accentline; do
    check "info $f" -info "$f"
done
# The cut is 30 UTF-16 units, so these bracket it: 28 y's plus an emoji is
# exactly 30 and is shown whole, while 29 y's plus an emoji is 31 and the cut
# lands between the halves of the surrogate pair.  For that shape the reference
# tool has no character to show and prints the literal text (null) for the
# whole preview, which is reproduced here rather than papered over.
check "info cut lands on a unit boundary" -info units30
check "info cut splits a surrogate pair" -info units31
check "info split pair from CJK" -info units32
check "info two emoji after 28 units" -info units33
check "info emoji alone" -info emojione
check "info emoji at the unit cut" -info units34
check "info several" -info line2.txt nonl.txt empty.txt
check "info space in name" -info "has space.txt"
check "info dash in name" -info -- -weird.txt
check "info subdirectory file" -info sub/n.txt
check "info rtfd" -convert rtfd -output b.rtfd line2.txt
check "info rtfd empty" -convert rtfd -output b.rtfd empty.txt
check "info rtf" -convert rtf -output b.rtf line2.txt
check "info html" -convert html -output b.html line2.txt
check "info txt" -convert txt -output b2.txt line2.txt

# ---------------------------------------------------------------------------
# The five writers, over every fixture shape.
# ---------------------------------------------------------------------------
for fmt in txt rtf html rtfd webarchive; do
    for f in line2 nonl empty one eol1 eol2 two3 n31 amp special blank \
             wsonly indent tabs accent cjk bom8 bom16le bom16be crlf \
             macutf8 macmix macall macrun maca9 macquote macutf8a9 macseq \
             macoverlong bommac boma9 ctl spcrun spcblank spctab; do
        check "convert $fmt $f" -convert "$fmt" "$f.txt"
    done
done

# Which way a line reads is a question the HTML writer answers for itself, and
# the HTML and webarchive writers are the only ones held to the reference's
# answer here -- the webarchive writer holds the very HTML the HTML writer
# writes, so the two share every one of these cases -- and the RTF and WordML
# writers say a line reads right to left only when the text named it with a
# mark, and a byte order mark is taken off the head of the text by the reader
# of the text rather than by each writer.  So the three shapes that settle all
# of this are checked against the HTML writer alone.
for f in bidi bidionly bidiend; do
    check "convert html $f" -convert html "$f.txt"
    check "convert webarchive $f" -convert webarchive "$f.txt"
done

# A mark at the head of a paragraph is how the RTF and WordML writers are told a
# paragraph reads right to left, so unlike the HTML writer they are held to what
# the reference does with one, and each writes the direction its own way:
# \rtlpar in the paragraph properties, and w:bidi.  The left-to-right mark is
# not the mirror of that -- it records having been told a direction, which
# leaves no word to write.  The three shapes above settle the HTML writer's
# side of this; these settle the other two.
for f in bidimark bidimarkhead bidimarktwice; do
    for fmt in rtf txt; do
        check "convert $fmt $f" -convert "$fmt" "$f.txt"
    done
    check "convert wordml $f" -convert wordml -output out "$f.txt"
done

# The embedding controls are held to all three writers at once, since it is the
# one thing they agree on how to write: the level each stretch of text is under,
# and where the text either side of a control that leaves the stack and comes
# back is one run and not two.  The plain text writer is held to them as well,
# for the other answer to the same question: a level is a direction given to the
# text it covers rather than characters in it, so it is not shown at all.
for f in embedone embedall embednest embedmerge embedtwo embedopen \
    embedpage embedpage2; do
    for fmt in html rtf txt; do
        check "convert $fmt $f" -convert "$fmt" "$f.txt"
    done
    check "convert wordml $f" -convert wordml -output out "$f.txt"
done

# Which paragraph a mark names a direction for, held to all four writers.  The
# three shapes above settle it for the three that record the direction, and
# these settle it for the fourth, which shows neither a level nor a mark at the
# head of a paragraph -- and for the three that record it, the case where a
# paragraph does not begin with a mark at all.
for f in markctlhead markctlmid marktwopara markonlyhead markcr markcrlf \
    markps markls; do
    for fmt in html rtf txt; do
        check "convert $fmt $f" -convert "$fmt" "$f.txt"
    done
    check "convert wordml $f" -convert wordml -output out "$f.txt"
done

# What may lie between the two halves of a CR and an LF that pair with each
# other.  All four writers are held to all eight shapes, since they are settled
# and they settled them the same way: the head of a line is taken first, and the
# controls are gone over after it.
for f in markinpair markinpairrlm ctlpair pairctl2 pairmarkctl pairmarkalm \
    pairctlmark pairmark2; do
    for fmt in html rtf txt; do
        check "convert $fmt $f" -convert "$fmt" "$f.txt"
    done
    check "convert wordml $f" -convert wordml -output out "$f.txt"
done

# A NUL among blanks, which all four writers are held to: it is not shown, and
# it leaves the blanks after it at the head of their line.
for f in nulrearm nulrearmthree nulrearmat nulrearmtail; do
    for fmt in html rtf txt; do
        check "convert $fmt $f" -convert "$fmt" "$f.txt"
    done
    check "convert wordml $f" -convert wordml -output out "$f.txt"
done

# A C0 control among blanks, and a level that opens and one that closes inside a
# region of blanks, which all four writers are held to as well: the control is
# not shown and the blanks after it lead their line only where a single word gap
# separates it from text the reader is shown, and the level the blanks were
# written under stays open past the end of the region.
for f in ctlrearmword ctlrearmtwo ctlrearmindent ctlrearmtabgap \
    ctlrearmlevel ctlrearmtail ctlrearmbs lvlrearmclosemark lvlrearmclosenul \
    lvlrearmcloseemoji lvlrearmcloseends; do
    for fmt in html rtf txt; do
        check "convert $fmt $f" -convert "$fmt" "$f.txt"
    done
    check "convert wordml $f" -convert wordml -output out "$f.txt"
done

# A paragraph separator is a line terminator in the HTML and RTF writers as
# well, and neither of them had it held to anything: the fixtures that cover it
# were written for the WordML writer and only ever checked against that, which
# is how the two writers got to disagree with the reference tool about what a
# U+2029 is.  U+2028 is here as well, since it is the same kind of question
# asked of the same two writers, one shape to one side of the boundary.
for f in ps psonly ps2 lsonly ls; do
    for fmt in html rtf txt; do
        check "convert $fmt $f" -convert "$fmt" "$f.txt"
    done
done

# ---------------------------------------------------------------------------
# Output naming.
# ---------------------------------------------------------------------------
check "convert names beside input" -convert rtf sub/n.txt
check "convert output names first" -convert txt -output first.txt line2.txt nonl.txt
check "convert output names first only" -convert rtf -output first.rtf line2.txt nonl.txt
check "convert extension" -convert txt -extension dat line2.txt nonl.txt
check "convert extension on rtf" -convert rtf -extension dat line2.txt
check "convert no extension on input" -convert rtf noext
check "convert dotfile input" -convert rtf .hidden
check "stdout txt" -convert txt -stdout line2.txt
check "stdout rtf" -convert rtf -stdout line2.txt
check "stdout html" -convert html -stdout line2.txt
check "stdout first of two" -convert txt -stdout line2.txt nonl.txt
check "cat default name txt" -cat txt line2.txt nonl.txt
check "cat default name rtf" -cat rtf line2.txt nonl.txt
check "cat default name html" -cat html line2.txt nonl.txt
check "cat one file" -cat txt line2.txt
check "cat empty file" -cat txt empty.txt
check "cat output" -cat txt -output all.txt line2.txt nonl.txt
check "cat extension" -cat txt -extension dat line2.txt nonl.txt
check "cat stdout" -cat txt -stdout line2.txt nonl.txt
check "cat three" -cat rtf line2.txt nonl.txt one.txt
check "cat missing file" -cat txt line2.txt nosuch.txt

# ---------------------------------------------------------------------------
# Options that are parsed and applied, so the bytes written are compared.
# -font names the face in the font table, -fontsize its size in half points,
# and -title the HTML <title>.
# ---------------------------------------------------------------------------
check "font" -convert rtf -font Courier line2.txt
check "font swiss" -convert rtf -font Arial line2.txt
check "font roman" -convert rtf -font "Times New Roman" line2.txt
check "font nil" -convert rtf -font Monaco line2.txt
check "font default" -convert rtf line2.txt
# A name is matched against the installed families without regard to case, a few
# names are aliases matched only as they are spelled, a PostScript name is an
# output rather than an input, and a name that resolves to nothing writes plain
# Helvetica -- which is not the Helvetica-Light that no -font at all writes.
check "font lower case" -convert rtf -font courier line2.txt
check "font upper case" -convert rtf -font COURIER line2.txt
check "font courier new" -convert rtf -font "Courier New" line2.txt
check "font alias helvetica-light" -convert rtf -font "Helvetica-Light" line2.txt
check "font alias helvetica bold" -convert rtf -font "Helvetica Bold" line2.txt
check "font alias palatino roman" -convert rtf -font "Palatino-Roman" line2.txt
check "font alias not case insensitive" -convert rtf -font "helvetica-light" line2.txt
check "font postscript name arialmt" -convert rtf -font ArialMT line2.txt
check "font postscript name couriernew" -convert rtf -font CourierNewPSMT line2.txt
check "font postscript name times roman" -convert rtf -font Times-Roman line2.txt
check "font unknown falls back" -convert rtf -font "No Such Face" line2.txt
check "font trailing space is unknown" -convert rtf -font "Courier " line2.txt
check "font palatino bold" -convert rtf -font "Palatino Bold" line2.txt
check "font arial bold" -convert rtf -font "Arial Bold" line2.txt
check "font arial bold italic" -convert rtf -font "Arial Bold Italic" line2.txt
check "font helvetica oblique" -convert rtf -font "Helvetica Oblique" line2.txt
check "font helvetica bold oblique" -convert rtf -font "Helvetica Bold Oblique" line2.txt
check "font palatino italic" -convert rtf -font "Palatino Italic" line2.txt
check "font and fontsize bold" -convert rtf -font "Arial Bold" -fontsize 30 line2.txt
# An empty value is refused for most of the options that take one, and each
# gives its own diagnostic rather than the one for a missing argument.  A title,
# a base URL and an input encoding are the exceptions, where empty is a value.
check "empty font" -convert rtf -font "" line2.txt
check "empty font no output" -convert rtf -font "" -output out line2.txt
check "empty fontsize" -convert rtf -fontsize "" line2.txt
check "empty extension" -convert rtf -extension "" line2.txt
check "empty output" -convert rtf -output "" line2.txt
check "empty prefixspaces" -convert rtf -prefixspaces "" line2.txt
check "empty textsizemultiplier" -convert rtf -textsizemultiplier "" line2.txt
check "empty title is allowed" -convert rtf -title "" line2.txt
check "empty baseurl is allowed" -convert rtf -baseurl "" line2.txt
check "empty inputencoding is allowed" -convert rtf -inputencoding "" line2.txt
check "empty author is allowed" -convert rtf -author "" line2.txt
check "fontsize" -convert rtf -fontsize 18 line2.txt
check "font and fontsize" -convert rtf -font Courier -fontsize 9 line2.txt
check "title" -convert html -title Hello line2.txt

# ---------------------------------------------------------------------------
# Options accepted but not acted on by this port.  They must still parse, and
# must not change the bytes written.
# ---------------------------------------------------------------------------
for opt in -strip -noload -nostore; do
    check "accepted $opt" -convert txt "$opt" line2.txt
done
check "accepted encoding" -convert txt -encoding UTF-8 line2.txt
check "accepted inputencoding" -convert txt -inputencoding UTF-8 line2.txt
check "accepted format" -convert txt -format txt line2.txt
check "accepted creationtime" -convert txt -creationtime 2026-01-02T03:04:05Z line2.txt
check "accepted modificationtime" -convert txt -modificationtime 2026-01-02T03:04:05Z line2.txt
check "accepted keywords" -convert txt -keywords "(a, b)" line2.txt
check "accepted prefixspaces" -convert html -prefixspaces 2 line2.txt
check "accepted baseurl" -convert html -baseurl file:///tmp line2.txt
check "accepted timeout" -convert html -timeout 5 line2.txt
check "accepted author" -convert txt -author Someone line2.txt
check "accepted subject" -convert txt -subject Something line2.txt
check "accepted comment" -convert txt -comment Note line2.txt
check "accepted editor" -convert txt -editor Someone line2.txt
check "accepted company" -convert txt -company Somewhere line2.txt
check "accepted textsizemultiplier" -convert html -textsizemultiplier 1.5 line2.txt

# ---------------------------------------------------------------------------
# Format names the parser must recognise even though this port cannot write
# them.  Naming one with no input files fails on the missing files before any
# writing is attempted, so these cases pin down that the name is accepted as a
# format rather than rejected as invalid.  What happens when a file is actually
# present is a known divergence, recorded in src/textutil/NOTES.md.
# ---------------------------------------------------------------------------
for fmt in txt rtf html rtfd doc docx odt wordml webarchive; do
    check "recognised $fmt" -convert "$fmt"
    check "recognised $fmt cat" -cat "$fmt"
done
# -excludedelements is recognised and its value read, but this port does not
# implement the second HTML serialisation that supplying it selects, so only
# the parse is pinned down here; see src/textutil/NOTES.md.
check "recognised excludedelements" -convert html -excludedelements "(a, b)"
check "still rejects unknown" -convert wibble

# ---------------------------------------------------------------------------
# Format names are matched without regard to case, and a name that is not one of
# the nine is refused as it is read rather than later, so ahead even of -help.
# The -convert half of this covers only the formats this port writes: the
# reference tool really does write the office containers, and a case that
# exercised one would be testing a writer rather than the parser.  The -format
# half leaves out html and webarchive, for the same reason read rather than
# written.  Both are recorded in src/textutil/NOTES.md.
# ---------------------------------------------------------------------------
for fmt in TXT Txt rtf RTF Rtfd HTML wordml WordML; do
    check "case insensitive -convert $fmt" -convert "$fmt" line2.txt
done
for fmt in TXT Txt rtf RTF Rtfd doc DOCX odt ODT wordml WordML; do
    check "case insensitive -format $fmt" -format "$fmt" -info line2.txt
done
for bad in "" " " wibble "rtfd " "public.rtf" "text/plain" "-txt" "Text"; do
    check "rejected -format '$bad'" -format "$bad" -info line2.txt
    check "rejected -format '$bad' help" -format "$bad" -help
done

# ---------------------------------------------------------------------------
# A forced -format overrides what the contents say.  Plain text reads anything,
# so forcing it on a file that would otherwise be taken as rich text reports it
# as plain text.  The other readers this port does not have are refused in the
# words the reference tool's own reader would use, which differ between the two
# families: RTF and RTFD say the file could not be opened, the office
# containers say it is not in the correct format.  A forced read failure is
# still not a failure of the command, so the status stays 0.
# ---------------------------------------------------------------------------
PREP='cp emojione real.rtf'
check "forced txt over rtf content" -format txt -info real.rtf
check "forced txt over rtf content, convert" -format txt -convert rtf -output out real.rtf
for f in rtf rtfd; do
    check "forced $f on plain text" -format "$f" -info line2.txt
    check "forced $f on plain text, convert" -format "$f" -convert txt -output out line2.txt
done
for f in doc docx odt wordml; do
    check "forced $f on plain text" -format "$f" -info line2.txt
    check "forced $f on plain text, cat" -cat txt -format "$f" -output out line2.txt
done
PREP=''

# ---------------------------------------------------------------------------
# -encoding is applied to the text, HTML and webarchive writers, and to
# nothing else: RTF and RTFD output is 7-bit by construction, and -info writes
# no output.  Each encoding is checked over input that has both a two byte and
# a four byte character, so that the surrogate pair a supplementary character
# becomes in UTF-16 is compared rather than assumed.
# ---------------------------------------------------------------------------
for e in utf8 UTF-8 utf-16 UTF-16 utf-16le utf-16be utf-32 utf-32LE utf-32be \
         UtF-16 4 4x; do
    check "-encoding $e txt" -convert txt -encoding "$e" -output out cjk.txt
    check "-encoding $e txt stdout" -convert txt -encoding "$e" -stdout cjk.txt
    check "-encoding $e cat" -cat txt -encoding "$e" -output out line2.txt cjk.txt
    check "-encoding $e html" -convert html -encoding "$e" -output out cjk.txt
    check "-encoding $e webarchive" -convert webarchive -encoding "$e" \
        -output out cjk.txt
    check "-encoding $e rtf" -convert rtf -encoding "$e" -output out cjk.txt
    check "-encoding $e rtfd" -convert rtfd -encoding "$e" -output out rtfd cjk.txt
    check "-encoding $e info" -encoding "$e" -info cjk.txt
done
# The charset attribute names the encoding that was used, and not the spelling
# that was asked for: "utf8" and the number 4 both come out as "utf-8".
for e in utf8 utf-16 utf-16le utf-16be utf-32 utf-32le utf-32be 4; do
    check "-encoding $e html charset" -convert html -encoding "$e" -output out line2.txt
    check "-encoding $e webarchive name" -convert webarchive -encoding "$e" \
        -output out line2.txt
done
# A name that names no encoding at all is refused up front, ahead even of
# -help, rather than answered with UTF-8 in its place.
for bad in nope wibble "utf-9" "" " " 4.5; do
    check "rejected -encoding '$bad'" -convert txt -encoding "$bad" -output out line2.txt
    check "rejected -encoding '$bad' help" -encoding "$bad" -help
done
# A number is an NSStringEncoding the reference tool has a table for, so it is
# accepted, and only 4 is one this port writes.  The parse is pinned down here;
# what the reference then writes for the others, and what this port says
# instead, is a known divergence recorded in src/textutil/NOTES.md.
for n in 9 30 1033 4x; do
    check "accepted -encoding number $n help" -encoding "$n" -help
    check "accepted -encoding number $n info" -encoding "$n" -info line2.txt
done

# ---------------------------------------------------------------------------
# The wordml writer.  The document is one long line with no newline at the end
# of it, so every byte of it is compared, including the two processing
# instructions, the namespace list and the fact that the body begins on the
# same line as the opening tag.
# ---------------------------------------------------------------------------
for f in one nonl line2 empty eol1 eol2 two3 blank wsonly indent tabs \
         tabonly tabends special amp accent cjk crlf cronly crlfnl lfcr \
         lfcrend ls ps lsonly psonly ls2 ps2 macutf8 macmix macall macrun \
         maca9 macquote macutf8a9 macseq macoverlong bommac boma9 ctl spcrun spcblank spctab; do
    check "wordml $f" -convert wordml -output out "$f.txt"
done
# The output name the format asks for, with and without an -output, and to
# standard output.
check "wordml default name" -convert wordml one.txt
check "wordml stdout" -convert wordml -stdout one.txt
check "wordml cat" -cat wordml -output out line2.txt
check "wordml extension" -convert wordml -extension xml one.txt
check "wordml into a subdirectory" -convert wordml -output sub/out one.txt
# The three characters that mean something to the markup, in a document, in a
# title and in a name, and a quote and an apostrophe beside them, which are
# written as themselves in the text of a document.
check "wordml escapes" -convert wordml -output out amp.txt
check "wordml escape title" -convert wordml -title 'a<b&c>d"e' -output out one.txt
check "wordml quote and apostrophe" -convert wordml -output out -title "a'b\"c" one.txt
# The font the run names is the one -font resolves to, and the size is in half
# points, so both a default and an odd size are checked.
for fn in Helvetica "Arial Bold" Avenir "Times New Roman" "Courier New" \
          wibble; do
    check "wordml font $fn" -convert wordml -font "$fn" -output out one.txt
done
for s in 1 7 12 18 72; do
    check "wordml fontsize $s" -convert wordml -fontsize "$s" -output out one.txt
    check "wordml font and size $s" -convert wordml -font Avenir -fontsize "$s" \
        -output out one.txt
done
# The document is written in UTF-8 whatever the text was decoded from, so
# nothing is refused for an encoding and the characters are written as bytes.
for e in utf8 utf-16 utf-16le utf-16be utf-32; do
    check "wordml -encoding $e" -convert wordml -encoding "$e" -output out cjk.txt
done
check "wordml -encoding refused name" -convert wordml -encoding nope \
    -output out one.txt
# A document is a run of paragraphs, and the marks that end one are a carriage
# return, a line feed, the two together, and U+2029.  A mark at the end of the
# document does not begin another paragraph, so these are the shapes the loop
# over them has to get right.
for f in eol1 eol2 two3 crlf cronly crlfnl lfcr lfcrend ls2 ps2 empty; do
    check "wordml paragraphs $f" -convert wordml -output out "$f.txt"
    check "wordml paragraphs $f stdout" -convert wordml -stdout "$f.txt"
done
# A destination that cannot be written, and a document that cannot be read, are
# refused in the words the other writers use.
PREP='chmod 500 rodir'
check "wordml into a read only directory" -convert wordml -output rodir/out one.txt
PREP=''
check "wordml from a missing file" -convert wordml -output out nosuchfile
check "wordml from a directory" -convert wordml -output out adir
# A document read as plain text has no page setup and no default tab stop, which
# is what the empty w:sectPr and the empty w:docPr are.  A document read as RTF
# or as HTML gets the page setup, the default tab stop, the paragraph spacing
# and the resolved default font of the reader that read it, none of which this
# port's readers record: the same gap shows up in rtf to rtf conversion.  So
# only the plain text cases above are compared here, for the same reason the
# office containers are not; see src/textutil/NOTES.md.
PREP=''

# ---------------------------------------------------------------------------
# The two office containers, docx and odt.  Good plain text is read
# identically, so the fixtures that settled the wordml writer are held to both
# of them here, and so are everything across the boundary the wordml cases
# leave alone: a line's direction in a bidi run, the embedding controls, the
# mark that names a paragraph's direction, the pair shapes, and the NUL and
# control runs among blanks.  The paragraph model is the wordml model -- same
# marks, same pairing, same opening mark taken out of the head -- so those
# fixtures pin where the two writers agree against the reference tool, and the
# direction fixtures pin where they are each held to the reference's own
# answer.  The comparison is check_zip for the reasons given beside zipfix
# above; the DOS times are the only thing not compared.
# ---------------------------------------------------------------------------
if [ "$HAVE_PY" -eq 1 ]; then
    for fmt in docx odt; do
        for f in one nonl line2 empty eol1 eol2 two3 blank wsonly indent tabs \
                 tabonly tabends special amp accent cjk crlf cronly crlfnl lfcr \
                 lfcrend ls ps lsonly psonly ls2 ps2 macutf8 macmix macall macrun \
                 maca9 macquote macutf8a9 macseq macoverlong bommac boma9 ctl \
                 spcrun spcblank spctab; do
            check_zip "convert $fmt $f" -convert "$fmt" "$f.txt"
        done
        for f in bidi bidionly bidiend; do
            check_zip "convert $fmt $f" -convert "$fmt" "$f.txt"
        done
        for f in bidimark bidimarkhead bidimarktwice; do
            check_zip "convert $fmt $f" -convert "$fmt" "$f.txt"
        done
        for f in embedone embedall embednest embedmerge embedtwo embedopen \
                 embedpage embedpage2; do
            check_zip "convert $fmt $f" -convert "$fmt" "$f.txt"
        done
        for f in markctlhead markctlmid marktwopara markonlyhead markcr \
                 markcrlf markps markls; do
            check_zip "convert $fmt $f" -convert "$fmt" "$f.txt"
        done
        for f in markinpair markinpairrlm ctlpair pairctl2 pairmarkctl \
                 pairmarkalm pairctlmark pairmark2; do
            check_zip "convert $fmt $f" -convert "$fmt" "$f.txt"
        done
        for f in nulrearm nulrearmthree nulrearmat nulrearmtail; do
            check_zip "convert $fmt $f" -convert "$fmt" "$f.txt"
        done
        for f in ctlrearmword ctlrearmtwo ctlrearmindent ctlrearmtabgap \
                 ctlrearmlevel ctlrearmtail ctlrearmbs lvlrearmclosemark \
                 lvlrearmclosenul lvlrearmcloseemoji lvlrearmcloseends; do
            check_zip "convert $fmt $f" -convert "$fmt" "$f.txt"
        done
    done
    # The output the format names, and the characters escaped in the document.
    check_zip "docx default name" -convert docx one.txt
    check_zip "odt default name" -convert odt one.txt
    check_zip "docx escapes" -convert docx -output out amp.txt
    check_zip "odt escapes" -convert odt -output out amp.txt
    # The -title and -author groups, which docx and odt each carry in their own
    # member, and the docx run properties from -font and -fontsize, in the
    # saved case to keep the whole archive against the loop above.
    check_zip "docx title and author" -convert docx -output out -title T \
        -author A one.txt
    check_zip "odt title and author" -convert odt -output out -title T \
        -author A one.txt
    check_zip "docx font and size" -convert docx -output out -font Arial \
        -fontsize 30 one.txt
    # The empty document, which leaves the container's styles out: the case
    # where the two writers are held to the difference between an empty text
    # and a paragraph of nothing.
    check_zip "docx empty" -convert docx -output out empty.txt
    check_zip "odt empty" -convert odt -output out empty.txt
else
    echo "note: python3 not found; skipping docx and odt writer cases"
fi

# The metadata options.  Each one opens the info group between the envelope and
# the paragraph, and the group is written for an empty value too: an empty
# -title is an empty title, not no title.
for f in title author subject comment editor company; do
    check "-$f" -convert rtf -"$f" "value" -output out line2.txt
    check "-$f empty" -convert rtf -"$f" "" -output out line2.txt
    check "-$f rtfd" -convert rtfd -"$f" "value" -output out line2.txt
    check "-$f html" -convert html -"$f" "value" -output out line2.txt
    check "-$f webarchive" -convert webarchive -"$f" "value" -output out line2.txt
    check "-$f txt" -convert txt -"$f" "value" -output out line2.txt
    check "-$f wordml" -convert wordml -"$f" "value" -output out line2.txt
    check "-$f wordml empty" -convert wordml -"$f" "" -output out line2.txt
done
# The fields are written in one order whatever order they are given in, and
# that order is not the order of the usage message.
check "info group order" -convert rtf -company C -editor E -comment M \
    -subject S -author A -title T -output out line2.txt
check "info group order wordml" -convert wordml -company C -editor E \
    -comment M -subject S -author A -title T -output out line2.txt
check "info group order webarchive" -convert webarchive -company C -editor E \
    -comment M -subject S -author A -title T -output out line2.txt
check "info group repeated" -convert rtf -title one -title two -output out line2.txt
check "info group with empty document" -convert rtf -title T -output out zero
check "info group with empty document wordml" -convert wordml -title T \
    -output out empty.txt
check "info group without a document" -convert rtf -title T -output out nosuchfile
check "info group without a document wordml" -convert wordml -title T \
    -output out nosuchfile
check "info group wordml times" -convert wordml -creationtime 2024-01-02T03:04:05Z \
    -modificationtime 2024-01-02T03:04:05Z -output out line2.txt
check "info group webarchive times" -convert webarchive \
    -creationtime 2024-01-02T03:04:05Z -modificationtime 2024-01-02T03:04:05Z \
    -output out line2.txt

# A value is escaped for RTF: the three characters with a meaning of their own,
# and then everything above ASCII.  The code page 1252 characters are written as
# bytes and the rest as UTF-16, and \uc0 goes in front of the first \u of the
# value and not again, whether the character before it needed an escape or not.
check "info escape braces and backslash" -convert rtf -title 'a{b}c\d' -output out line2.txt
check "info escape quote" -convert rtf -title 'a"b' -output out line2.txt
check "info escape leading" -convert rtf -title '{' -output out line2.txt
check "info escape latin1" -convert rtf -title 'café' -output out line2.txt
check "info escape cp1252 high" -convert rtf -title '–—‘’“”†‡•…‰‹›€™' -output out line2.txt
check "info escape cp1252 in 0x80 range" -convert rtf -title 'ŒœŠšŸŽžƒˆ˜†‡‰‹›' -output out line2.txt
check "info escape latin1 range" -convert rtf -title ' ­ÿ' -output out line2.txt
check "info escape c1 controls" -convert rtf -title "$(printf '\200\237')" -output out line2.txt
check "info escape two byte" -convert rtf -title '中' -output out line2.txt
check "info escape four byte" -convert rtf -title '😀' -output out line2.txt
check "info escape c1 then two byte" -convert rtf -title "$(printf '\200')中" -output out line2.txt
check "info escape two byte then c1" -convert rtf -title "中$(printf '\200')" -output out line2.txt
check "info escape two byte run" -convert rtf -title '中中中' -output out line2.txt
check "info escape four byte run" -convert rtf -title '😀😀' -output out line2.txt
check "info escape mixed run" -convert rtf -title '中—中😀中' -output out line2.txt
check "info escape mixed run reversed" -convert rtf -title '😀中—中' -output out line2.txt
check "info escape ascii between" -convert rtf -title '中a中' -output out line2.txt
check "info escape control between" -convert rtf -title "$(printf '中\001中')" -output out line2.txt
check "info escape space between" -convert rtf -title '中 中' -output out line2.txt
check "info escape utf8 truncation" -convert rtf -title "$(printf '\303')" -output out line2.txt
check "info escape bad continuation" -convert rtf -title "$(printf '\303\041')" -output out line2.txt
check "info escape long" -convert rtf -title "$(rtk python3 -c 'print("a"*200, end="")')" -output out line2.txt

# The two times.  The argument is twenty characters of YYYY-MM-DDTHH:MM:SSZ and
# only that many are looked at, so anything may follow and nothing is checked
# for range; a month of 13 is a time the reference tool accepts.
for t in 2024-01-02T03:04:05Z 1913-01-01T00:00:00Z 2038-01-19T03:14:07Z \
         2069-01-19T03:14:06Z 2069-01-19T03:14:07Z 9998-01-01T00:00:00Z \
         2024-13-45T25:61:61Z 2024-02-30T00:00:00Z 2024-01-02T03:04:05ZZZ \
         1969-12-31T23:59:59Z 1970-01-01T00:00:00Z; do
    check "creation time $t" -convert rtf -creationtime "$t" -output out line2.txt
    check "modification time $t" -convert rtf -modificationtime "$t" -output out line2.txt
done
check "both times" -convert rtf -creationtime 2024-01-02T03:04:05Z \
    -modificationtime 2025-06-07T08:09:10Z -output out line2.txt
check "both times reversed" -convert rtf -modificationtime 2025-06-07T08:09:10Z \
    -creationtime 2024-01-02T03:04:05Z -output out line2.txt
check "both times same" -convert rtf -creationtime 2024-01-02T03:04:05Z \
    -modificationtime 2024-01-02T03:04:05Z -output out line2.txt
check "times rtfd" -convert rtfd -creationtime 2024-01-02T03:04:05Z -output out line2.txt
check "times html" -convert html -creationtime 2024-01-02T03:04:05Z -output out line2.txt
check "times txt" -convert txt -creationtime 2024-01-02T03:04:05Z -output out line2.txt
check "times and title" -convert rtf -title T -creationtime 2024-01-02T03:04:05Z \
    -output out line2.txt
check "times and company" -convert rtf -company C -creationtime 2024-01-02T03:04:05Z \
    -output out line2.txt
# What is not the twenty characters asked for is refused, and the word names
# which of the two it was.
for t in 2024-01-02T03:04:05 2024-01-02 2024-01-02 03:04:05Z \
         2024-01-02t03:04:05z 2024-1-2T3:4:5Z 2024-01-02T03:04:05.5Z \
         2024-01-02T03:04:05+00:00 20240102T030405Z 2024-01-02X03:04:05Z \
         2024/01/02T03:04:05Z now never "" 12345; do
    check "rejected -creationtime '$t'" -convert rtf -creationtime "$t" -output out line2.txt
    check "rejected -modificationtime '$t'" -convert rtf -modificationtime "$t" -output out line2.txt
done
check "rejected creation time help" -creationtime now -help
check "rejected creation time info" -creationtime now -info line2.txt
# -keywords wants a list and the reference tool mishandles every form of it, so
# it is taken and not acted on.  Only a single word is compared here; the rest
# of its behaviour is a known divergence in src/textutil/NOTES.md.
check "keywords one word" -convert rtf -keywords one -output out line2.txt

# The readers.  -info is the only command that reads a rich file back, and it
# reports the text and the metadata of RTF, of the RTF inside a bundle, and of
# HTML.  Each file is written by the reference tool into a template that every
# case copies, so that neither side is reading the other's work.
check_read "read rtf back" -info one.rtf
check_read "read rtfd back" -info one.rtfd
check_read "read html back" -info one.html
check_read "read rtf metadata back" -info meta.rtf
check_read "read rtfd metadata back" -info meta.rtfd
check_read "read html metadata back" -info meta.html

# What an RTF file has to be before it is read at all.
check_read "rtf without a header" -info plain.rtf
check_read "rtf with an unclosed group" -info open.rtf
check_read "rtf with a bad escape" -info esc.rtf
check_read "rtf with an extra brace" -info extra.rtf
check_read "rtf with a nested group" -info nest.rtf

# A bundle is a folder with a TXT.rtf in it, and each of these says otherwise.
check_read "rtfd with nothing in it" -info empty.rtfd
check_read "rtfd with an empty TXT.rtf" -info blank.rtfd
check_read "rtfd whose TXT.rtf is not rtf" -info junk.rtfd
check_read "rtfd whose TXT.rtf is malformed" -info trunc.rtfd
check_read "rtfd with the name in capitals" -info upper.RTFD
check_read "folder that is not a bundle" -info plaindir.rtfd
check_read "file named as a bundle" -info filer.rtfd

# A recognised name is read as what it says it is, whatever is in it.
check_read "html named as rtf" -info lying.rtf
check_read "rtf named as html" -info lying.html
check_read "rtf named as txt" -info lying.txt
check_read "html with no name" -info inl

# The same question asked of -convert, which is where a reader chosen by name
# rather than by bytes is visible in the text that comes back: a .rtf holding
# HTML fails to open instead of being read as HTML, and a file named for a
# format it does not hold is read as what it holds.
check_read "convert html named as rtf" -convert txt -stdout lying.rtf
check_read "convert rtf named as html" -convert txt -stdout lying.html
check_read "convert rtf named as txt" -convert txt -stdout lying.txt
check_read "convert html with no name" -convert txt -stdout inl
check_read "convert rtf named as doc" -convert txt -stdout lying.doc
check_read "convert rtf named as docx" -convert txt -stdout lying.docx
check_read "convert html named as odt" -convert txt -stdout inl.odt
check_read "convert rtf named as wordml" -convert txt -stdout lying.wordml
check_read "convert rtf named as rtfd" -convert txt -stdout filer.rtfd
check_read "convert doctype named as foo" -convert txt -stdout docty.foo
check_read "convert doctype named as txt" -convert txt -stdout docty.txt
check_read "convert doctype named as rtf" -convert txt -stdout docty.rtf
check_read "convert doctype named as doc" -convert txt -stdout docty.doc
check_read "info doctype named as foo" -info docty.foo
check_read "convert htm named as rtf" -convert txt -stdout htm.rtf
check_read "convert html named as htm" -convert txt -stdout inl.htm
check_read "convert htm in capitals" -convert txt -stdout upper.HTM
check_read "convert rtf in capitals" -convert txt -stdout upper.RTF
check_read "convert a real bundle" -convert txt -stdout realdir.rtfd
check_read "convert a folder named htm" -convert txt -stdout realdir.HTM
check_read "info rtf in capitals" -info upper.RTF
check_read "info a real bundle" -info realdir.rtfd
check_read "info a webarchive that is not one" -info notarch.webarchive
check_read "convert a webarchive that is not one" -convert txt -stdout notarch.webarchive

# The bytes that decide a file with no format in its name.  Each of these is a
# case where being one byte or one case different changes the reader, and where
# the answer is visible in the text rather than only in a name.
for f in snr1 snr2 snr3 snr4 snr5 snr6 snr7 snr8 snr9 snr10 snr11 snr12 \
         snr13 snr14 snr15; do
    check_read "sniff rtf $f" -info "$f"
    check_read "convert sniff rtf $f" -convert txt -stdout "$f"
done
for f in snh1 snh2 snh3 snh4 snh5 snh6 snh7 snh8 snh9 snh10 snh11 snh12 \
         snh13 snh14 snh15 snh16 snh17 snh18 snh19; do
    check_read "sniff html $f" -info "$f"
    check_read "convert sniff html $f" -convert txt -stdout "$f"
done
# A document that begins with a line break has an empty first line, and
# -info shows no Contents field for it at all.  One that begins with a space
# is not empty and does get one.
for f in empty brk afterbrk; do
    check_read "empty first line $f" -info "$f"
    check_read "convert empty first line $f" -convert txt -stdout "$f"
done

# An entity is how the HTML reader proves it ran, and each of these names the
# same bytes differently.
for f in ents ents.txt ents.html ents.htm ents.foo; do
    check_read "entity $f" -info "$f"
    check_read "convert entity $f" -convert txt -stdout "$f"
done

echo
echo "PASS=$PASS FAIL=$FAIL"
if [ "$FAIL" != 0 ]; then
    echo "failed:$FAILED"
    exit 1
fi
