/*
 * ============================================================================
 *  Title:    FujiNet mailbox peripheral
 * ============================================================================
 *  This module emulates the RP2040 side of the FujiNet-for-Intellivision
 *  bridge described in fujinet-firmware/pico/intellivision/.  On real
 *  hardware, an RP2040 cart exposes a "mailbox" register window in
 *  Intellivision RAM at $9C00-$9F3F, and relays each transaction to a
 *  FujiNet (ESP32-S3) over USB CDC as a SLIP-framed FujiBus packet (see
 *  fujibus.h).  This peripheral presents the identical mailbox window to
 *  the emulated Intellivision, but instead of USB CDC it speaks the same
 *  FujiBus frames over a TCP socket -- specifically, fujinet-firmware's
 *  "Bus Over IP" (BoIP) feature, which is a transparent byte stream with
 *  no framing of its own, so the bytes on the wire are identical to what
 *  the RP2040 would send over USB.  fujinet-firmware is the TCP server;
 *  we are the client.
 *
 *  See fuji_mailbox.h for the register layout (copied verbatim from
 *  fujinet-firmware/pico/intellivision/firmware/fuji_mailbox.h -- that
 *  file remains the single source of truth for the layout; keep this in
 *  sync with it by hand if it ever changes upstream).
 * ============================================================================
 */
#ifndef FUJINET_H_
#define FUJINET_H_

#include <stdio.h>

#include "fujinet/fuji_mailbox.h"
#include "fujinet/fn_sock.h"

/* ======================================================================== */
/*  FUJINET_STATE_T  -- Connection / transaction state machine states       */
/* ======================================================================== */
typedef enum
{
    FUJINET_ST_DISCONNECTED = 0,   /* No socket; waiting to (re)connect.    */
    FUJINET_ST_CONNECTING,         /* connect() issued, awaiting completion */
    FUJINET_ST_IDLE,               /* Connected, no transaction in flight.  */
    FUJINET_ST_SENDING,            /* Draining the request frame to the fd. */
    FUJINET_ST_RXWAIT              /* Awaiting the SLIP-framed reply.       */
} fujinet_state_t;

/* Size of the emulated mailbox window. Must be a power of 2 that evenly
 * divides FUJI_MB_ADDR_LO ($9C00 = 0x400 * 39), since fujinet_read()/
 * fujinet_write() recover the ram[] index with addr & (WINDOW_SIZE - 1) --
 * 0x400 is the largest such power of 2 that still covers the mailbox's own
 * footprint (0x340 words, $9C00-$9F3F); the remaining $9F40-$9FFF is
 * unused slack, same as the old $9800-based window had past $9F3F.
 */
#define FUJINET_WINDOW_SIZE  0x0400

/* ======================================================================== */
/*  FUJINET_T        -- FujiNet mailbox peripheral object                   */
/* ======================================================================== */
typedef struct fujinet_t
{
    periph_t        periph;                    /* Yes, it's a peripheral.  */

    uint8_t         ram[FUJINET_WINDOW_SIZE];   /* $9C00-$9FFF, byte-wide. */

    char           *host;                       /* fujinet-firmware host.  */
    int             port;                       /* BoIP TCP port.          */
    int             debug;                      /* Trace frames/states?    */

    fn_sock_t       sock;                       /* TCP connection.         */
    fujinet_state_t state;
    uint64_t        deadline;                   /* periph.now cycle count. */
    uint64_t        reconnect_at;                /* periph.now cycle count. */

    uint8_t         seq_in_flight;

    uint8_t         txbuf[FUJI_MB_TX_MAX + 128]; /* Encoded SLIP request.  */
    size_t          txlen;
    size_t          txsent;

    uint8_t         rxbuf[FUJI_MB_RX_MAX + 128]; /* Encoded SLIP reply.    */
    size_t          rxlen;
    unsigned        seen_end;

    /*  DBC (FUJI_DEVICEID_DBC) inbound-frame handling: the real           */
    /*  fujinet-firmware bridge pushes a ROM (and optionally a .cfg        */
    /*  sibling) to this device mid-MOUNT_IMAGE-transaction, exactly the   */
    /*  same as it does to the RP2040 -- see dbc_inbound_handler() in      */
    /*  fujinet-firmware/pico/intellivision/firmware/inty_cart.c. Unlike   */
    /*  the RP2040, this emulator CAN reload the running cart: the pushed  */
    /*  bytes are decoded into the existing icart_t (fujinet_apply_rom()   */
    /*  in fujinet.c) and the console is reset, same as resetCart() does   */
    /*  on real hardware. With --fujinet-bootdump=PREFIX it also writes    */
    /*  what it receives to PREFIX.rom / PREFIX.cfg, which can be diffed   */
    /*  against the source files as an end-to-end test of the ESP32-side   */
    /*  media-type push, independent of the reload.                        */
    char           *bootdump_prefix;             /* NULL: don't dump.     */
    FILE           *bootdump_fh;                  /* Currently-open file.  */
    int             bootdump_stream;               /* -1, or 0=rom/1=cfg. */
    uint32_t        bootdump_rom_bytes;            /* For the progress bar.*/

    /*  Cart-reload target, set once by fujinet_init() from cfg_init()'s   */
    /*  own icart/cp1600/bus objects -- see fujinet_apply_rom() in         */
    /*  fujinet.c.                                                        */
    void             *icart;      /* Actually icart_t*; void* here so this  */
    void             *cpu;        /* header doesn't need icart.h/cp1600.h   */
                                   /* -- see fujinet_apply_rom() in          */
                                   /* fujinet.c for the cast back.           */
    periph_bus_t     *bus;
    uint32_t          cache_flags;

    uint8_t  *rom_buf;       /* Growable heap buffer for the in-flight ROM.*/
    size_t    rom_len;
    size_t    rom_cap;

    uint8_t   cfg_buf[2048]; /* .cfg sibling text; mirrors inty_cart.c's   */
    size_t    cfg_len;       /* fixed CFG_BUF_MAX -- a .cfg is well under  */
    int       have_cfg;      /* 1KB in practice.                          */
} fujinet_t;

/* ======================================================================== */
/*  FUJINET_INIT     -- Sets up the FujiNet mailbox peripheral.             */
/* ======================================================================== */
int fujinet_init
(
    fujinet_t  *const fujinet,
    const char *const host,        /*  fujinet-firmware hostname/IP.       */
    const int          port,        /*  BoIP TCP port.                     */
    const int          debug,       /*  Nonzero to enable frame tracing.   */
    const char *const  bootdump_prefix, /* NULL, or dump DBC pushes here.  */
    void             *const icart,      /*  Actually icart_t*.             */
    void             *const cpu,        /*  Actually cp1600_t*.            */
    periph_bus_t     *const bus,        /*  Bus, for icart_register()/     */
                                         /*  periph_reset().                */
    const uint32_t     cache_flags      /*  Same cache flags cfg_init()    */
                                         /*  passed to the startup          */
                                         /*  icart_register() call.         */
);

#endif /* FUJINET_H_ */
/* ======================================================================== */
/*  This program is free software; you can redistribute it and/or modify    */
/*  it under the terms of the GNU General Public License as published by    */
/*  the Free Software Foundation; either version 2 of the License, or       */
/*  (at your option) any later version.                                     */
/*                                                                          */
/*  This program is distributed in the hope that it will be useful,         */
/*  but WITHOUT ANY WARRANTY; without even the implied warranty of          */
/*  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU       */
/*  General Public License for more details.                                */
/*                                                                          */
/*  You should have received a copy of the GNU General Public License along */
/*  with this program; if not, write to the Free Software Foundation, Inc., */
/*  51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA.             */
/* ======================================================================== */
