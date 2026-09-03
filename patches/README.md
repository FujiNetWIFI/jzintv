# Community patch series

A patch series by deck <jenergy@tiscali.it>, authored against vanilla
upstream jzIntv 20200822 and kept here for provenance. The FujiNet
mailbox peripheral is forward-ported on top of the applied ones.

## Applied

| Patch | What it does |
|---|---|
| `0001` | Upstream best-tree fixes across 12 files, including the stray `#elif` in `src/config.h` |
| `0002` | TutorVision EXEC/GROM auto-selection; moves the EXEC/GROM load later in `cfg_init()` |
| `0003` | `--aspect-4-3`: force a 4:3 pillarboxed picture in fullscreen |
| `0004` | Adds `build.sh` and `package-release.sh` |
| `0005` | Adds the `sprint` target and `src/Makefile.sprint` (Intellivision Sprint, armhf) |

Two things had to be reconciled by hand, both because the series was
authored against a differently-configured tree:

* **Line endings.** Upstream jzIntv ships 365 source files with CRLF
  endings; the series was authored against an LF-normalised tree, so no
  hunk touching a C source matched. Each patch was retargeted to the
  repository's actual line endings before applying, and applied with
  `git am --keep-cr` (`git mailinfo` strips CR from the patch body
  otherwise). Content was unaffected.
* **`.gitignore`.** `0004` and `0005` expect a `.gitignore` in which
  `bin/*.dll` and `bin/README` are tracked, which is not how this fork
  is set up. Their new rules were merged by hand instead: this repo's
  existing `bin/*` rule already covers `/bin/linux/`, `/bin/windows/`
  and `/bin/sprint/`, so only `/build-support/` and `*.zip` were added.

## Not applied

`0006` and `0007` are present but **cannot be applied**, because the
series is incomplete. The `Subject:` lines run 1/7, 2/7, 3/7, 5/7, 6/7
and then a separate 1/2, 2/2 — patches **4/7 and 7/7 are missing**.

7/7 is the blocker. It added the Intellivision Sprint in-game popup menu
— `src/jzintv.h`, `popup_draw_string()` and its callers in
`src/gfx/gfx_sdl2.c`, and the menu hooks in `src/event/` — and set
`DEF_FLAGS = -DJZINTV_SPRINT` in `src/Makefile.sprint`. `0006` only
*gates* that code behind `JZINTV_SPRINT` and `ADD_JZINTV_MENU`, so with
7/7 absent it has nothing to wrap: all six of its source hunks fail, and
the menu implementation is not recoverable from the diff. `0007` then
fails in turn, because one of its three `gfx_sdl2.c` hunks needs the
`#ifdef` block that `0006` introduces.

Both patches are Sprint-only and everything they touch is behind
`JZINTV_SPRINT`, which no build in this repository defines — the CI
armv7 job builds the plain `linux-armhf` target — so nothing else is
affected by their absence.

To finish the series, drop `0004-*.patch` (4/7) and the real 7/7 in here
and apply `0006`/`0007` on top; note that the existing file numbering
does not match the `Subject:` numbering.
