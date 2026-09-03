# jzIntv + FujiNet

This is a fork of Joe Zbiciak's [jzIntv](http://spatula-city.org/~im14u2c/intv/)
-- a Mattel Intellivision(TM) emulator for Linux, Mac, and Windows -- with
added support for the [FujiNet](https://fujinet.online/) network-attached
storage/multi-peripheral for the Intellivision.

It is based on the upstream 2020-08-22 (SVN r2141) source release; see
`ReleaseNotes_20200822.txt` and the earlier `ReleaseNotes_*.txt` files for
that release's history, and `ReleaseNotes_FujiNet.txt` for what this fork
adds on top of it.

The fork also carries a community patch series by deck
<jenergy@tiscali.it>, kept in `patches/` -- upstream bug fixes,
TutorVision EXEC/GROM auto-selection, an `--aspect-4-3` option, the
`build.sh` build front-end, and an Intellivision Sprint cross-build
target.  See `patches/README.md`.

## What's added: FujiNet support

Real FujiNet-for-Intellivision hardware is an RP2040 cart that exposes a
"mailbox" register window in Intellivision RAM and relays each transaction
to a FujiNet (ESP32-S3) over USB CDC, using SLIP-framed FujiBus packets.
This fork emulates that mailbox peripheral (`src/fujinet/`) and speaks the
same FujiBus frames over a TCP socket instead of USB CDC, using
fujinet-firmware's "Bus Over IP" (BoIP) feature -- a transparent byte
stream with no framing of its own, so the bytes on the wire are identical
to what the RP2040 would send. jzIntv is the BoIP client; a running
fujinet-firmware instance is the server.

In short: run a fujinet-firmware instance (real hardware, or an emulated
build) and point jzIntv at it, and Intellivision software using the
FujiNet mailbox protocol -- WiFi setup, network-mounted disk images,
directory browsing, etc. -- works inside the emulator.

### Command-line options

```
--fujinet[=host[:port]]
                Enable the FujiNet mailbox peripheral at $9C00-$9FFF,
                connecting over BoIP to a fujinet-firmware instance.
                Defaults to localhost:1985. If no ROM is given on the
                command line, boots the built-in FujiNet config ROM
                (WiFi setup / host slots / directory browser) instead
                of looking for game.rom; an explicit ROM argument
                always overrides this.

--fujinet-debug
                Trace FujiNet mailbox transactions and FujiBus frames
                to stdout.

--fujinet-bootdump=PREFIX
                The emulator can't reload the cart the way real RP2040
                firmware does, so it can't complete a MOUNT_IMAGE boot
                -- but it can still receive the same DBC-addressed
                ROM/.cfg push a real board would. With this set, it
                writes what it received to PREFIX.rom and, if a .cfg
                sibling was pushed, PREFIX.cfg.
```

> **Note:** the FujiNet mailbox window ($9C00-$9FFF) overlaps the tail of
> the JLP RAM window, so `--jlp` and `--fujinet` should not be combined in
> the same session.

### Quick start

1. Start a `fujinet-firmware` instance (or point at one already running)
   with its BoIP server enabled -- default port `1985`.
2. Run jzIntv with `--fujinet`:

   ```sh
   bin/jzintv --fujinet
   ```

   With no ROM argument, this boots the built-in FujiNet config ROM so you
   can configure WiFi / host slots from within the emulator. To connect to
   a non-default host/port:

   ```sh
   bin/jzintv --fujinet=192.168.1.50:1985 my_game.rom
   ```
3. A minimal FujiNet mailbox test program is included in
   `examples/fujinet/` (`fujitest.bas` / `fujitest.rom`).

## Building

```sh
./build.sh                # everything your toolchain can manage
./build.sh linux          # native build        -> bin/linux/jzintv
./build.sh linux-armhf    # armv7 cross-build   -> bin/linux-armhf/jzintv
./build.sh windows        # MinGW-w64 cross     -> bin/windows/jzintv.exe
./build.sh sprint         # Intellivision Sprint -> bin/sprint/update.zip
```

`build.sh linux` needs a C/C++ toolchain and the SDL2 development package
(`libsdl2-dev` on Debian/Ubuntu, `SDL2-devel` on Fedora, `sdl2` on Arch).
`linux-armhf` additionally needs `crossbuild-essential-armhf` and
`libsdl2-dev:armhf`.  Run `./build.sh --help` for the environment
variables it honours.

The upstream per-platform Makefiles still work if you prefer them:

```sh
cd src
make -f Makefile.<platform>     # e.g. Makefile.linux_sdl2, Makefile.osx_sdl2
```

See `doc/` for the full upstream build and usage documentation, and run
`jzintv --help` for the complete list of command-line options.

### Prebuilt binaries

GitHub Actions builds Linux x86-64, ARM64 and armv7 on every push; the
binaries are attached to each workflow run as artifacts, and tagged
releases carry `.tar.gz` bundles for all three.  They link against the
system SDL2, so install your distribution's SDL2 runtime
(`libsdl2-2.0-0` on Debian/Ubuntu) before running one.

## Game Binaries / BIOS Images

jzIntv needs an EXEC image, a GROM image, and a game ROM to run anything.
You can use the stock `exec.bin`, `grom.bin`, and game ROM images that
come with the Intellivision Lives! CD or Intellivision Rocks! CD --
see http://www.intellivisionlives.com/. These are copyrighted and are not
included in this repository (see `.gitignore`); place them in `rom/`.

## Documentation

The `doc/` directory contains the full upstream jzIntv documentation.

## Credits

* jzIntv is written and maintained by Joe Zbiciak (intvnut AT gmail.com).
  See the [jzIntv homepage](http://spatula-city.org/~im14u2c/intv/) for
  full credits.
* FujiNet support added for the FujiNet project (https://fujinet.online/).

## Administrivia

Intellivision (TM) is a trademark of Intellivision Productions. Joe
Zbiciak, jzIntv, and this fork are not affiliated with Intellivision
Productions. See `COPYING.txt` for license terms (GPL v2 or later).
