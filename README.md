# AKCmds

Clean-room reimplementations of the AppKit command-line tools that ship in
macOS, written to match the shipped binaries byte for byte.

| command | written in | what it is |
| --- | --- | --- |
| `textutil` | C | reads plain text, writes `txt`, `rtf`, `rtfd`, `html`, `wordml` |
| `tiffutil` | C | reads and rewrites TIFF, including the `-cat` family |
| `tops` | Objective-C | in-place textual substitution driven by a `find`/`replace` rule grammar |
| `open` | Objective-C | the LaunchServices front end |
| `tiff2icns` | Objective-C | builds an `.icns` from one image file |
| `pbcopy` / `pbpaste` | Objective-C | the general pasteboard, one binary told apart by `argv[0]` |

Apple ships `pbcopy` and `pbpaste` as two binaries built from one source and
dispatching on `argv[0]`; this does the same, linking two copies of one object.

`textutil` is deliberately partial: it accepts the other four format names —
`doc`, `docx`, `odt`, `webarchive` — so that the option surface matches, but it
does not write them, and it has no readers but plain text. The divergences that
follow are listed together at the end of `src/textutil/NOTES.md`. `tops` is
partial in the same spirit: it implements the textual rule grammar, not a
PostScript interpreter.

## What "byte for byte" means here, and what it does not

Every tool here is checked against the real one still installed in
`/usr/bin`, by running both over the same input and comparing exit status,
both streams, and every byte of every file produced. As of this writing that is
**2201 cases across six harnesses, none failing**:

| harness | cases | surface |
| --- | --- | --- |
| `tests/tiffutil-parity.sh` | 942 | the TIFF reader, the writer, and the `-cat` family |
| `tests/textutil-parity.sh` | 933 | the option parser, `-info`, and five writers |
| `tests/parity.sh` (`tops`) | 184 | every mode, including the `-script` grammar and the TTY cases |
| `tests/tiff2icns-parity.sh` | 65 | representation selection across the six icon sizes |
| `tests/open-parity.sh` | 62 | argument handling, diagnostics, exit status |
| `tests/pbcopy-parity.sh` | 15 | the `ruler`, `find` and `font` pasteboards |

The number to quote is the total, but the *scope* line matters more than the
count, and each harness states its own in its header. A harness agrees about
what it exercises and nothing else. `tiff2icns`'s 65 cases are about choosing a
representation for the 16, 32, 48, 128, 256 and 512 sizes, from multi-page
uncompressed single-strip TIFFs whose pages differ in size; compressed, tiled,
multi-strip, 16-bit and palette/CMYK sources are never exercised, so agreement
there says nothing about how either side reads them.

The narrower the surface, the more precisely it needs saying. `open`'s 62 cases
are all ones both tools reject or resolve to a plain file, so they are evidence
about argument handling and exit status and are explicitly **not** evidence
that either binary launches an application correctly. `pbcopy`'s 15 cases never
touch the general pasteboard, because `-pboard` accepts only
`general|ruler|find|font` and any other value lands on general: a harness that
overwrote a developer's real clipboard would be worse than no harness.

Agreement is a statement about the reference tool as it was observed, not a
guarantee that it was observed everywhere.

## How the work is done

Each tool states its own provenance in its source header, and the two methods in
play are not the same. `tops`, `open`, `tiff2icns` and `pbcopy`/`pbpaste` were
recovered by disassembly of the shipped binary combined with black-box probing.
`textutil` and `tiffutil` were **not**: both are written in C, and both say so
plainly — their `NOTES.md` records that every finding came from probing the
installed tool, and that nothing was disassembled or read out of the binary.

That distinction is why those two carry a `NOTES.md` at all. A finding you
reached by disassembly is easy to re-derive; a finding you reached by guessing
what the output would be is worthless unless you wrote down what you tried. So
the notes record the probe, not just the conclusion:

- `src/textutil/NOTES.md` — the option surface, the `-info` model's counting in
  UTF-16 units, and the reference's own quirks in how it lays out runs of
  spaces and tabs.
- `src/tiffutil/NOTES.md` — that the writer is CoreGraphics rather than
  libtiff, and that its injected ICC profile is a per-colour-space constant,
  which is what makes byte parity tractable at all.

Where the shipped behaviour contradicts a published standard, the shipped
behaviour wins and the divergence is recorded. `textutil` is the clearest case:
it decides which way a line reads by treating a code point Unicode leaves
unassigned as strong left-to-right, so `src/textutil/bidi.c` carries a table of
direction ranges read off the reference tool one code point at a time rather
than taken from the Unicode data files.

## Building

```sh
make            # release, artifacts in build/release
make CONFIG=debug
make test       # all six parity harnesses against /usr/bin
make clean
```

A single harness can be run on its own, and the tool it exercises built first:

```sh
make textutil-parity
./tests/textutil-parity.sh          # MY=... and ORACLE=... both override
```

`ORACLE` exists so a harness can be pointed at a reference build other than the
one installed. `pbcopy-parity.sh` additionally refuses to run if an invocation
would reach the general pasteboard.

The makefile is portable across GNU make and bmake: no pattern rules, no
`ifeq`/`.if` conditionals and no `$(shell)`, with per-config flags in
`make/release.mk` and `make/debug.mk`. For the same reason nothing in the build
resolves the project root at build time — each harness locates it itself.

## Layout

```
src/<tool>/        the implementation, plus NOTES.md where a tool needed them
man/man1/          manual pages
tests/             one differential harness per tool
make/              per-config flags
```

BSD 3-Clause, © 2026 LibreDarwin.
