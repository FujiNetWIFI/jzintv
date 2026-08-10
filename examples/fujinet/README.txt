FUJINET MAILBOX TEST

fujitest.bas / fujitest.rom demonstrate jzIntv's --fujinet peripheral, which
emulates the RP2040 side of the FujiNet-for-Intellivision bridge from
fujinet-firmware/pico/intellivision/ -- see src/fujinet/fujinet.h for the
implementation.  It presents the same "mailbox" register window at
$9C00-$9FFF that real RP2040 hardware does, but instead of relaying
transactions to a FujiNet over USB CDC, it speaks the same FujiBus wire
protocol over a TCP socket to fujinet-firmware's "Bus Over IP" (BoIP)
feature.

This ROM sends a GET_ADAPTERCONFIG_EXTENDED request (device 0x70, command
0xC4) through the mailbox and prints the SSID, firmware version, and local
IP address from the reply.  It's copied, with one change (see below), from
fujinet-firmware/pico/intellivision/intv/fujitest.bas, which is the same
test used against real RP2040 hardware -- keep the two in sync by hand if
the mailbox layout in fuji_mailbox.h ever changes.

RUNNING IT

1. Start a fujinet-pc build of fujinet-firmware (an RS232 build, not
   ESP32) with [BOIP] enabled in fnconfig.ini.  The default BoIP port for
   an RS232 build is 1985.

2. Run:

       jzintv --fujinet=<fujinet-host>[:<port>] fujitest.rom

   Omit the host to default to "localhost:1985".  Add --fujinet-debug to
   trace mailbox transactions and FujiBus frames to stdout.

   NOTE:  jzIntv also embeds the FujiNet config ROM (fujinet-config/intv --
   WiFi setup, host slots, directory browsing, boot-a-ROM).  Omitting the
   ROM argument entirely (just "jzintv --fujinet=<host>[:<port>]") boots
   that instead of looking for game.rom, the same way a real FujiNet
   cartridge would on power-up.  Passing fujitest.rom explicitly, as above,
   always overrides it.

3. The ROM should print the FujiNet's SSID, firmware version, and IP
   address on screen within a few seconds.  "NO CARTRIDGE MAILBOX" means
   jzIntv wasn't started with --fujinet at all; a FujiBus error code from
   FUJI_MB_ERR (1=no link, 2=timeout, 3=bad frame, 4=too big) means jzIntv
   connected to something at that host:port but the far end isn't a BoIP
   server speaking FujiBus, or fujinet-firmware isn't running/reachable.

JZINTV-SPECIFIC CHANGE FROM THE UPSTREAM ROM

The upstream ROM declares the full $8000-$9FFF as RAM via
"ASM MEMATTR $8000, $9FFF, "+RWN"", because on real hardware the RP2040
maps that entire window as physical RAM (the mailbox cells are just
polled cells within it).  jzIntv instead models $9C00-$9FFF as its own
FujiNet peripheral -- registered separately from, and behaving just like,
the cartridge's RAM for that range -- so this copy declares RAM only for
$8000-$9BFF and leaves $9C00-$9FFF to --fujinet.  Declaring RAM over both
ranges would register two peripherals over the same addresses; jzIntv's
periph_read() ANDs together every reader in a bin, so the FujiNet
peripheral's magic-byte and ACKSEQ cells would get corrupted by the cart
RAM's zero-initialized contents.  (This is the same constraint already
documented for JLP's savegame RAM window in src/cfg/cfg.c -- don't declare
RAM over a range a peripheral owns.)

REBUILDING

    intybasic --title "FujiNet Test" fujitest.bas fujitest.asm <path-to-intybasic-prologue-epilogue>
    as1600 fujitest.asm -o fujitest.bin -l fujitest.lst
    bin2rom fujitest
