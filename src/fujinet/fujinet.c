/*
 * ============================================================================
 *  Title:    FujiNet mailbox peripheral
 * ============================================================================
 *  See fujinet.h for the overview.  This file implements the state machine
 *  that plays the RP2040's role from fuji_mailbox_service() (see
 *  fujinet-firmware/pico/intellivision/firmware/inty_cart.c) against a
 *  non-blocking TCP socket instead of a USB CDC link.
 *
 *  Because jzIntv ticks peripherals cooperatively and must never block, the
 *  RP2040's straight-line "wait for link, build request, send, wait for
 *  reply" sequence becomes an explicit state machine driven a little bit
 *  further every tick:
 *
 *      DISCONNECTED -> CONNECTING -> IDLE <-> SENDING -> RXWAIT -> IDLE
 *           ^                                                |
 *           +------------------------------------------------+
 *                       (socket error at any point)
 *
 *  IDLE is where we notice the Intellivision has bumped FUJI_MB_SEQ (i.e.
 *  ram[SEQ] != ram[ACKSEQ]) and start a transaction; every other state is
 *  just pushing that one transaction to completion.  Every path out of a
 *  transaction -- success, timeout, bad frame, or link loss -- publishes
 *  ram[ACKSEQ] = seq exactly once, written last, so the guest never hangs
 *  waiting on a mailbox reply that will never come.
 * ============================================================================
 */

#include "config.h"
#include "lzoe/lzoe.h"
#include "periph/periph.h"
#include "cp1600/cp1600.h"
#include "mem/mem.h"
#include "icart/icart.h"
#include "metadata/metadata.h"
#include "file/file.h"
#include "bincfg/bincfg.h"
#include "bincfg/legacy.h"
#include "jlp/jlp.h"
#include "misc/jzprint.h"
#include "fujinet/fujibus.h"
#include "fujinet/fujinet.h"

#if defined(_WIN32) || defined(WIN32)
#  include <direct.h>
#else
#  include <sys/stat.h>
#  include <sys/types.h>
#endif

/* Approximate CP-1610 clock rate that periph->now counts in (see
 * PERIPH_HZ() in periph/periph.h -- NTSC main clock / 4).  Used only to
 * convert the millisecond timeouts/backoffs from fuji_mailbox_service() and
 * fujibus_transact() into a cycle-count budget; real-hardware parity to the
 * millisecond doesn't matter; staying in the right ballpark relative to
 * fujitest.bas's own frame-count timeouts does.
 */
#define FUJINET_CLK_HZ    (3579545 / 4)
#define FUJINET_MS(ms)    (((uint64_t)(ms) * FUJINET_CLK_HZ) / 1000)

#define FUJINET_CONNECT_TIMEOUT_MS   2000   /* Matches BOIP_CONNECT_TMOUT.  */
#define FUJINET_RECONNECT_BACKOFF_MS 2000
#define FUJINET_TXN_TIMEOUT_MS       5000   /* Matches fuji_mailbox_service.*/
#define FUJINET_MOUNT_TIMEOUT_MS     60000  /* Matches fuji_mailbox_service's
                                              * MOUNT_IMAGE override -- a ROM
                                              * (+.cfg) push rides the same
                                              * link mid-transaction and can
                                              * run well past the ordinary
                                              * 5s budget. */

/* Device-relative offset of a FUJI_MB_* constant (which is defined as an
 * absolute RAM[] index, i.e. Intellivision-address minus $8000).  Our
 * ram[] is indexed from $9C00, so subtract FUJI_MB_BASE ($1C00) again.
 */
#define OFS(x) ((x) - FUJI_MB_BASE)

/* ======================================================================== */
/*  FUJINET_PAINT_IDENT -- (Re)paints the constant magic/version cells.     */
/* ======================================================================== */
LOCAL void fujinet_paint_ident(fujinet_t *const fn)
{
    fn->ram[OFS(FUJI_MB_MAGIC0)]    = 'F';
    fn->ram[OFS(FUJI_MB_MAGIC1)]    = 'N';
    fn->ram[OFS(FUJI_MB_PROTO_VER)] = 1;
}

/* ======================================================================== */
/*  FUJINET_RESET     -- periph.reset: repaints the RP2040-owned cells.     */
/*                       Does NOT touch the TCP connection -- a console     */
/*                       reset on real hardware doesn't reboot the RP2040,  */
/*                       and (per fuji_mailbox.h) the Inty must always      */
/*                       derive its next SEQ from our ACKSEQ, never from a  */
/*                       local counter, so leaving ACKSEQ alone is correct */
/*                       and required for that contract to hold across a   */
/*                       reset.                                             */
/* ======================================================================== */
LOCAL void fujinet_reset(periph_t *const per)
{
    fujinet_t *const fn = PERIPH_AS(fujinet_t, per);

    fujinet_paint_ident(fn);
    fn->ram[OFS(FUJI_MB_STATUS)]    = FUJI_MB_STATUS_IDLE;
    fn->ram[OFS(FUJI_MB_ERR)]       = FB_OK;
    fn->ram[OFS(FUJI_MB_RXLEN_LO)]  = 0;
    fn->ram[OFS(FUJI_MB_RXLEN_HI)]  = 0;
    fn->ram[OFS(FUJI_MB_REPLY_CMD)] = 0;
    fn->ram[OFS(FUJI_MB_LINK)]      = fn_sock_connected(&fn->sock) &&
                                       fn->state != FUJINET_ST_CONNECTING;
}

/* ======================================================================== */
/*  FUJINET_READ / PEEK  -- Reads are pure; peek aliases read.              */
/* ======================================================================== */
LOCAL uint32_t fujinet_read(periph_t *const per, periph_t *req,
                             uint32_t addr, uint32_t data)
{
    fujinet_t *const fn = PERIPH_AS(fujinet_t, per);
    UNUSED(req);
    UNUSED(data);

    /* Mailbox disabled for this session: ~0 is the AND-identity on
     * jzIntv's shared bus, so the game's own ROM here reads through. */
    if (!fn->mailbox_active)
        return ~0U;

    return fn->ram[addr & (FUJINET_WINDOW_SIZE - 1)];
}

/* ======================================================================== */
/*  FUJINET_WRITE / POKE -- Writes are hardware-truncated to 8 bits, same   */
/*                       as the DWS handler in inty_cart.c's core1_main().  */
/*                       No side effects fire here -- fujinet_tick() is     */
/*                       what notices SEQ changed, exactly mirroring the    */
/*                       real device where the RP2040 polls RAM rather than */
/*                       being interrupted by the write.                    */
/* ======================================================================== */
LOCAL void fujinet_write(periph_t *const per, periph_t *req,
                          uint32_t addr, uint32_t data)
{
    fujinet_t *const fn = PERIPH_AS(fujinet_t, per);
    UNUSED(req);

    if (!fn->mailbox_active)
        return;

    fn->ram[addr & (FUJINET_WINDOW_SIZE - 1)] = (uint8_t)(data & 0xFF);
}

/* ======================================================================== */
/*  FUJINET_FINISH_TXN -- Publishes a transaction result.  `st` is the      */
/*                       fb_status_t for FUJI_MB_ERR; `reply` is NULL on    */
/*                       any failure.  Always ends by writing ACKSEQ last.  */
/* ======================================================================== */
LOCAL void fujinet_finish_txn(fujinet_t *const fn, fb_status_t st,
                               const fb_reply_t *const reply)
{
    fn->ram[OFS(FUJI_MB_ERR)] = (uint8_t)st;

    if (st == FB_OK && reply)
    {
        uint16_t rxlen = reply->data_len;
        uint16_t i;

        if (rxlen > FUJI_MB_RX_MAX)
            rxlen = FUJI_MB_RX_MAX;   /* Defensive; shouldn't happen. */

        for (i = 0; i < rxlen; i++)
            fn->ram[OFS(FUJI_MB_RX) + i] = reply->data[i];

        fn->ram[OFS(FUJI_MB_RXLEN_LO)]  = (uint8_t)(rxlen & 0xFF);
        fn->ram[OFS(FUJI_MB_RXLEN_HI)]  = (uint8_t)((rxlen >> 8) & 0xFF);
        fn->ram[OFS(FUJI_MB_REPLY_CMD)] = reply->command;
        fn->ram[OFS(FUJI_MB_STATUS)]    =
            (reply->command == FUJICMD_ACK) ? FUJI_MB_STATUS_OK
                                             : FUJI_MB_STATUS_ERR;

        if (fn->debug)
            jzp_printf("FUJINET: txn seq=%d OK cmd=%.2X rxlen=%d\n",
                       fn->seq_in_flight, reply->command, rxlen);
    } else
    {
        fn->ram[OFS(FUJI_MB_RXLEN_LO)]  = 0;
        fn->ram[OFS(FUJI_MB_RXLEN_HI)]  = 0;
        fn->ram[OFS(FUJI_MB_REPLY_CMD)] = 0;
        fn->ram[OFS(FUJI_MB_STATUS)]    = FUJI_MB_STATUS_ERR;

        if (fn->debug)
            jzp_printf("FUJINET: txn seq=%d FAILED status=%d\n",
                       fn->seq_in_flight, (int)st);
    }

    /*  Single publishing store, written last -- see fuji_mailbox.h.        */
    fn->ram[OFS(FUJI_MB_ACKSEQ)] = fn->seq_in_flight;
    fn->state = FUJINET_ST_IDLE;
}

/* ======================================================================== */
/*  FUJINET_FAIL_LINK -- A socket-level failure: drop the connection and,   */
/*                       if a transaction was in flight, fail it too.       */
/* ======================================================================== */
LOCAL void fujinet_fail_link(fujinet_t *const fn, const periph_t *const per)
{
    const int had_txn = (fn->state == FUJINET_ST_SENDING ||
                          fn->state == FUJINET_ST_RXWAIT);

    if (fn->debug)
        jzp_printf("FUJINET: link lost, reconnecting\n");

    fn_sock_close(&fn->sock);
    fn->ram[OFS(FUJI_MB_LINK)] = 0;
    fn->reconnect_at = per->now + FUJINET_MS(FUJINET_RECONNECT_BACKOFF_MS);
    fn->state = FUJINET_ST_DISCONNECTED;

    if (had_txn)
        fujinet_finish_txn(fn, FB_ENOLINK, NULL);
}

/* ======================================================================== */
/*  FUJINET_START_TXN -- IDLE noticed SEQ != ACKSEQ: marshal the request    */
/*                       exactly as fuji_mailbox_service() does and hand it */
/*                       to the codec.                                      */
/* ======================================================================== */
LOCAL void fujinet_start_txn(fujinet_t *const fn, const periph_t *const per)
{
    uint8_t     seq    = fn->ram[OFS(FUJI_MB_SEQ)];
    uint8_t     device = fn->ram[OFS(FUJI_MB_DEVICE)];
    uint8_t     cmd    = fn->ram[OFS(FUJI_MB_CMD)];
    unsigned    nparam = fn->ram[OFS(FUJI_MB_NPARAM)];
    fb_param_t  params[8];
    uint16_t    txlen;
    static uint8_t txpayload[FUJI_MB_TX_MAX];
    unsigned    i;

    if (nparam > 8)
        nparam = 8;

    for (i = 0; i < nparam; i++)
    {
        uint8_t  size = fn->ram[OFS(FUJI_MB_PARAM_SIZE) + i];
        uint32_t val  = 0;
        uint8_t  b;

        for (b = 0; b < size && b < 4; b++)
            val |= ((uint32_t)fn->ram[OFS(FUJI_MB_PARAM_VAL) + i * 4 + b])
                   << (8 * b);

        params[i].size  = size;
        params[i].value = val;
    }

    txlen = (uint16_t)(fn->ram[OFS(FUJI_MB_TXLEN_LO)] |
                       (fn->ram[OFS(FUJI_MB_TXLEN_HI)] << 8));
    if (txlen > FUJI_MB_TX_MAX)
        txlen = FUJI_MB_TX_MAX;

    for (i = 0; i < txlen; i++)
        txpayload[i] = fn->ram[OFS(FUJI_MB_TX) + i];

    /*  See fn->last_boot_path: this is the only place a name for a        */
    /*  network-pushed image ever crosses the mailbox.  Snapshot it        */
    /*  regardless of whether the transaction below succeeds -- the        */
    /*  config ROM sent it, which is all we need it for.  Same placement   */
    /*  as the RP2040's fuji_mailbox_service().                            */
    if (device == FUJI_DEVICEID_FUJINET && cmd == FUJICMD_SET_DEVICE_FULLPATH)
    {
        unsigned n = txlen < sizeof(fn->last_boot_path) - 1
                   ? txlen : sizeof(fn->last_boot_path) - 1;
        memcpy(fn->last_boot_path, txpayload, n);
        fn->last_boot_path[n] = 0;
        if (fn->debug)
            jzp_printf("FUJINET: boot path = \"%s\"\n", fn->last_boot_path);
    }

    fn->seq_in_flight = seq;
    fn->ram[OFS(FUJI_MB_STATUS)] = FUJI_MB_STATUS_BUSY;

    if (fn->debug)
        jzp_printf("FUJINET: txn seq=%d dev=%.2X cmd=%.2X nparam=%d "
                   "txlen=%d\n", seq, device, cmd, nparam, txlen);

    /*  Not connected right now?  Fail immediately -- there's no USB-       */
    /*  enumeration race to wait out the way there is on real hardware.     */
    if (fn->state != FUJINET_ST_IDLE || !fn_sock_connected(&fn->sock))
    {
        fujinet_finish_txn(fn, FB_ENOLINK, NULL);
        return;
    }

    fn->txlen = fujibus_build_request(device, cmd,
                                       nparam ? params : NULL, nparam,
                                       txlen ? txpayload : NULL, txlen,
                                       fn->txbuf, sizeof(fn->txbuf));
    if (!fn->txlen)
    {
        fujinet_finish_txn(fn, FB_ETOOBIG, NULL);
        return;
    }

    /*  Anti-desync: same rationale as fujibus_usb.c's transaction start -- */
    /*  drop anything already sitting in the receive path before we send.  */
    {
        uint8_t junk[256];
        int     eof;
        while (fn_sock_recv(&fn->sock, junk, sizeof(junk), &eof) > 0)
            /* keep draining */ ;
    }

    fn->txsent   = 0;
    fn->rxlen    = 0;
    fn->seen_end = 0;
    {
        uint32_t timeout_ms = FUJINET_TXN_TIMEOUT_MS;
        if (device == FUJI_DEVICEID_FUJINET && cmd == FUJICMD_MOUNT_IMAGE)
            timeout_ms = FUJINET_MOUNT_TIMEOUT_MS;
        fn->deadline = per->now + FUJINET_MS(timeout_ms);
    }
    fn->state    = FUJINET_ST_SENDING;
}

/* ======================================================================== */
/*  FUJINET_DBC_SEND_ACK -- Sends a bare ACK reply for one DBC command,     */
/*                       over the same socket fujinet_start_txn() sends     */
/*                       requests on. The real fujinet-firmware bridge's    */
/*                       SYSTEM_BUS.sendCommand() blocks waiting for this   */
/*                       exact reply before sending its next OPEN/WRITE/    */
/*                       CLOSE frame, so this has to go out synchronously   */
/*                       -- DBC ACK frames are ~8 bytes, small enough that   */
/*                       a bounded retry loop is fine even on a non-        */
/*                       blocking socket (this mirrors the RP2040's own     */
/*                       dbc_send_frame(), which has the same shape for the */
/*                       same reason).                                      */
/* ======================================================================== */
LOCAL void fujinet_dbc_send_ack(fujinet_t *const fn)
{
    uint8_t buf[16];
    size_t  len = fujibus_build_request(FUJI_DEVICEID_DBC, FUJICMD_ACK,
                                         NULL, 0, NULL, 0, buf, sizeof(buf));
    size_t  sent = 0;
    int     tries;

    if (!len)
        return;

    for (tries = 0; sent < len && tries < 1000; tries++)
    {
        const int n = fn_sock_send(&fn->sock, buf + sent, len - sent);
        if (n < 0)
            return;   /* Link's going down anyway; fujinet_fail_link() below. */
        if (n > 0)
            sent += (size_t)n;
    }
}

/* ======================================================================== */
/*  FUJINET_DBC_STREAM_OPEN/WRITE/CLOSE -- --fujinet-bootdump=PREFIX file   */
/*                       plumbing. See fujinet.h's bootdump_* fields.       */
/* ======================================================================== */
LOCAL void fujinet_dbc_stream_open(fujinet_t *const fn, int stream,
                                    uint32_t expected)
{
    fn->bootdump_stream = stream;
    if (stream == 0)
    {
        fn->bootdump_rom_bytes = 0;
        fn->rom_len = 0;
        fn->rom_truncated = 0;
        fn->rom_expected = expected;

        /*  The sender told us the exact size, so take the whole buffer in   */
        /*  one go rather than letting fujinet_rom_buf_append() double its   */
        /*  way there (six reallocs of a growing 232KB image for             */
        /*  Pacmanthology).  Purely an optimization: a failure here leaves   */
        /*  rom_cap alone and the append path grows as it always did.        */
        if (expected && expected > fn->rom_cap)
        {
            uint8_t *grown = (uint8_t *)realloc(fn->rom_buf, expected);
            if (grown)
            {
                fn->rom_buf = grown;
                fn->rom_cap = expected;
            }
        }
    }
    else
    {
        fn->cfg_len       = 0;
        fn->have_cfg      = 0;
        fn->cfg_truncated = 0;
    }

    if (!fn->bootdump_prefix)
        return;

    {
        char path[512];
        snprintf(path, sizeof(path), "%s.%s", fn->bootdump_prefix,
                  stream == 1 ? "cfg" : "rom");
        fn->bootdump_fh = fopen(path, "wb");
        if (fn->debug)
            jzp_printf("FUJINET: bootdump stream %d -> %s (%s)\n", stream,
                       path, fn->bootdump_fh ? "ok" : "FAILED TO OPEN");
    }
}

/* Growable append into fn->rom_buf, doubling capacity as needed. Normally
 * this never has to grow at all: fujinet_dbc_stream_open() reserves the
 * exact size the OPEN header carried. It still matters against a sender
 * that doesn't send one, and as the backstop if that reservation failed. */
LOCAL void fujinet_rom_buf_append(fujinet_t *const fn,
                                   const uint8_t *data, uint16_t len)
{
    if (!data || !len)
        return;

    if (fn->rom_truncated)
        return;   /*  Already gave up on this push; don't keep trying.     */

    if (fn->rom_len + len > fn->rom_cap)
    {
        size_t newcap = fn->rom_cap ? fn->rom_cap * 2 : 4096;
        uint8_t *grown;

        while (newcap < fn->rom_len + len)
            newcap *= 2;

        /*  Assigning realloc()'s result straight back would drop the only  */
        /*  pointer to the existing image on failure, then memcpy into      */
        /*  NULL below. Keep what we have and fail the boot instead.        */
        grown = (uint8_t *)realloc(fn->rom_buf, newcap);
        if (!grown)
        {
            fn->rom_truncated = 1;
            return;
        }
        fn->rom_buf = grown;
        fn->rom_cap = newcap;
    }
    memcpy(fn->rom_buf + fn->rom_len, data, len);
    fn->rom_len += len;
}

LOCAL void fujinet_dbc_stream_write(fujinet_t *const fn,
                                     const uint8_t *data, uint16_t len)
{
    if (fn->bootdump_fh && data && len)
        fwrite(data, 1, len, fn->bootdump_fh);

    if (fn->bootdump_stream == 0)
    {
        fujinet_rom_buf_append(fn, data, len);
    }
    else if (fn->bootdump_stream == 1 && data && len)
    {
        size_t room = sizeof(fn->cfg_buf) - fn->cfg_len;
        size_t n = len < room ? len : room;
        if (len > room)
            fn->cfg_truncated = 1; /* cfg didn't fit -- fail TRUNCATED later */
        memcpy(fn->cfg_buf + fn->cfg_len, data, n);
        fn->cfg_len += n;
    }
}

LOCAL void fujinet_dbc_stream_close(fujinet_t *const fn)
{
    if (fn->bootdump_fh)
    {
        fclose(fn->bootdump_fh);
        fn->bootdump_fh = NULL;
    }
    if (fn->bootdump_stream == 1)
        fn->have_cfg = (fn->cfg_len > 0);
    fn->bootdump_stream = -1;
}

/* ======================================================================== */
/*  FUJINET_MAILBOX_OVERLAP -- [addr, addr+len-1] vs the $9C00-$9F3F        */
/*                       mailbox window, mirroring fujinet.c's              */
/*                       mailbox_overlap() (jlp_pending=false case; this    */
/*                       peripheral doesn't model the Minty JLP window).    */
/*                       An overlap no longer rejects the boot -- the game  */
/*                       boots with the mailbox disabled for the session,   */
/*                       same as the RP2040 firmware.                       */
/* ======================================================================== */
LOCAL int fujinet_mailbox_overlap(uint32_t addr, uint32_t len)
{
    uint32_t to = addr + len - 1;
    return !(to < FUJI_MB_ADDR_LO || addr > FUJI_MB_ADDR_HI);
}

/* ======================================================================== */
/*  FUJINET_MKDIR     -- Creates one directory; "already there" is success. */
/* ======================================================================== */
LOCAL void fujinet_mkdir(const char *path)
{
#if defined(_WIN32) || defined(WIN32)
    _mkdir(path);
#else
    mkdir(path, 0777);
#endif
}

/* ======================================================================== */
/*  FUJINET_WRITE_FILE -- Dumps a buffer to a path.  Returns 0 on success.  */
/* ======================================================================== */
LOCAL int fujinet_write_file(const char *path, const void *data, size_t len)
{
    FILE *f = fopen(path, "wb");
    size_t wrote;

    if (!f)
        return -1;

    wrote = len ? fwrite(data, 1, len, f) : 0;

    /*  fclose() is where a full filesystem usually surfaces, so its result  */
    /*  matters as much as fwrite()'s -- a short stage would look exactly    */
    /*  like a truncated push to everything downstream.                      */
    if (fclose(f) != 0 || wrote != len)
    {
        remove(path);
        return -1;
    }

    return 0;
}

/* ======================================================================== */
/*  FUJINET_SAVEGAME_PATH -- Builds the JLP save-file path for the pushed   */
/*                       title, from the FUJICMD_SET_DEVICE_FULLPATH the    */
/*                       config ROM sent just before MOUNT_IMAGE.  Mirrors  */
/*                       config_jlp() on the RP2040, which does the same    */
/*                       thing with its own last_boot_path: take the        */
/*                       basename, swap the extension, and keep it beside   */
/*                       the boot staging area so the save survives the     */
/*                       session.  Falls back to "network" when nothing     */
/*                       was staged, same as the firmware's "network.rom".  */
/* ======================================================================== */
LOCAL void fujinet_savegame_path(fujinet_t *const fn, char *out, size_t out_sz)
{
    const char *base = fn->last_boot_path;
    char        stem[128];
    const char *slash, *dot;
    size_t      n, i;

    /*  Basename: the path is a *remote* one (TNFS/SD host slot), so accept  */
    /*  either separator regardless of what this platform uses locally.      */
    for (slash = base; *slash; slash++)
        if (*slash == '/' || *slash == '\\')
            base = slash + 1;

    if (!*base)
        base = "network";

    dot = strrchr(base, '.');
    n   = dot && dot != base ? (size_t)(dot - base) : strlen(base);
    if (n > sizeof(stem) - 1)
        n = sizeof(stem) - 1;
    memcpy(stem, base, n);
    stem[n] = 0;

    /*  Whatever came off the wire is now going to be part of a local path.  */
    /*  Keep it to characters that can't walk out of the directory or upset  */
    /*  a shell-quoting-free fopen() on any of the platforms this builds on. */
    for (i = 0; i < n; i++)
    {
        unsigned char c = (unsigned char)stem[i];
        if (!((c >= '0' && c <= '9') || (c >= 'A' && c <= 'Z') ||
              (c >= 'a' && c <= 'z') || c == '-' || c == '_'))
            stem[i] = '_';
    }
    if (!stem[0])
        strcpy(stem, "network");

    snprintf(out, out_sz, "%s%cjlpsave%c%s.jlp",
             fn->bootstage_dir, PATH_SEP, PATH_SEP, stem);
}

/* ======================================================================== */
/*  FUJINET_RECACHEABLE_CONSOLE -- Re-marks the cacheable ranges that       */
/*                       belong to the console rather than to any cart.     */
/*                                                                          */
/*  cp1600_uncacheable_all() is deliberately blunt -- it has no way to tell */
/*  the outgoing cart's marks from anyone else's -- so the ranges cfg.c     */
/*  sets up once at machine build have to be put back afterwards.  Keep     */
/*  this list in step with cfg_init()'s own, right after its                */
/*  legacy_register()/icart_register() call: system RAM (snooped, since     */
/*  it's writable), the EXEC ROM, and GROM.  The FujiNet window itself is   */
/*  re-marked here too, for the same reason cfg.c marks it: not cacheable,  */
/*  because its cells change underneath the CPU.                            */
/* ======================================================================== */
LOCAL void fujinet_recacheable_console(fujinet_t *const fn)
{
    cp1600_t *const cpu = (cp1600_t *)fn->cpu;

    cp1600_cacheable(cpu, 0x0200, 0x035F, 1);
    cp1600_cacheable(cpu, 0x1000, 0x1FFF, 0);
    cp1600_cacheable(cpu, 0x3000, 0x37FF, 0);
    cp1600_cacheable(cpu, 0x9C00, 0x9FFF, 0);
}

/* ======================================================================== */
/*  FUJINET_RETIRE_CART -- Unbinds whatever cart currently owns the bus.    */
/*                                                                          */
/*  periph_register() only ever ORs a device into the address decode, so    */
/*  without this the outgoing map keeps answering next to the incoming one  */
/*  and the wire-AND bus hands the CPU the AND of the two.  Note the        */
/*  asymmetry with free(): periph_unregister() deliberately leaves the      */
/*  device on the bus' linked list (see periph.h), because that list still  */
/*  owns its reset/serialize/dtor duties.  So a retired legacy_t is parked  */
/*  on fn->retired[] and freed by nobody until periph_delete() runs its     */
/*  dtor at teardown.  A session that pushes many carts therefore grows by  */
/*  the size of each one's decoded image; a handful of pushes is what this  */
/*  is actually used for.                                                   */
/* ======================================================================== */
LOCAL void fujinet_retire_cart(fujinet_t *const fn)
{
    legacy_t *const old = (legacy_t *)fn->legacy;
    int i;

    if (!old)
    {
        /*  Still the startup cart (the config ROM, or whatever ROM jzIntv   */
        /*  was given on the command line).                                  */
        icart_unregister((icart_t *)fn->icart, fn->bus);
        return;
    }

    periph_unregister(fn->bus, &old->periph);
    for (i = 0; i < old->npg_rom; i++)
        periph_unregister(fn->bus, &old->pg_rom[i].periph);

    if (fn->n_retired == fn->retired_cap)
    {
        int    newcap = fn->retired_cap ? fn->retired_cap * 2 : 4;
        void **grown  = (void **)realloc(fn->retired, newcap * sizeof(void *));

        /*  Can't park it: leave it out of the list rather than dropping the */
        /*  pointer.  It stays unbound and reachable through the bus, which  */
        /*  is all the list is for anyway.                                   */
        if (grown)
        {
            fn->retired     = grown;
            fn->retired_cap = newcap;
        }
    }
    if (fn->n_retired < fn->retired_cap)
        fn->retired[fn->n_retired++] = old;

    fn->legacy = NULL;
}

/* ======================================================================== */
/*  FUJINET_APPLY_JLP -- Brings up JLP for a pushed title that asked for it */
/*                       in its .cfg [vars], the same way cfg.c does for a  */
/*                       command-line --jlp.                                */
/*                                                                          */
/*  JLP claims $8000-$9FFF whole, which swallows the mailbox window -- so   */
/*  the caller disables the mailbox for the session, exactly as it does for */
/*  a cart whose own segments cover it.  The RP2040 rejects this            */
/*  combination outright (FUJI_BOOT_ERR_MAILBOX) because it has one RAM     */
/*  window to hand out; jzIntv can simply stop answering there and let JLP  */
/*  have it.                                                                */
/* ======================================================================== */
LOCAL int fujinet_apply_jlp(fujinet_t *const fn, int jlp_accel, int jlp_flash)
{
    jlp_t *jlp = (jlp_t *)fn->jlp;
    char   sgpath[512];

    if (!jlp)
    {
        jlp = CALLOC(jlp_t, 1);
        if (!jlp)
            return -1;
        fn->jlp = jlp;
    } else if (jlp->periph.dtor)
    {
        /*  Second JLP push this session.  jlp_init() left its own dtor      */
        /*  here; running it closes the previous save file and frees the RAM */
        /*  and flash images before we allocate replacements.  It leaves     */
        /*  jlp->sg_file dangling, though, and jlp_init() only reassigns     */
        /*  that when it manages to open the new one -- so clear the data    */
        /*  fields wholesale rather than naming them and hoping the list     */
        /*  stays right.                                                     */
        /*                                                                   */
        /*  The periph_t has to survive that memset verbatim: this jlp_t is  */
        /*  already on the bus' linked list, and periph.next is what links   */
        /*  the *rest* of that list.  Zeroing it drops every peripheral      */
        /*  behind this one -- no reset, no dtor, and periph.bus NULL, so    */
        /*  periph_register() below would happily "re-add" it a second time. */
        const periph_t saved = jlp->periph;
        jlp->periph.dtor(&jlp->periph);
        memset(jlp, 0, sizeof(*jlp));
        jlp->periph = saved;
    }

    fujinet_savegame_path(fn, sgpath, sizeof(sgpath));
    {
        char dir[512];
        snprintf(dir, sizeof(dir), "%s%cjlpsave", fn->bootstage_dir, PATH_SEP);
        fujinet_mkdir(dir);
    }

    if (jlp_init(jlp, sgpath, &((cp1600_t *)fn->cpu)->xr[0],
                 jlp_accel, jlp_flash, 0))
        return -1;

    if (!fn->jlp_registered)
    {
        periph_register(fn->bus, &jlp->periph, 0x8000, 0x9FFF, "JLP Support");
        fn->jlp_registered = 1;
    }

    if (fn->debug)
        jzp_printf("FUJINET: JLP accel=%d flash=%d savegame=%s\n",
                   jlp_accel, jlp_flash, sgpath);

    return 0;
}

/* ======================================================================== */
/*  FUJINET_APPLY_BINCFG -- Loads a pushed .bin + .cfg pair through         */
/*                       jzIntv's own legacy BIN+CFG loader and hands it    */
/*                       the bus.                                           */
/*                                                                          */
/*  This replaces a hand-rolled port of the RP2040's parse_pushed_cfg(),    */
/*  which topped out at 16 [mapping] lines, silently dropped the "PAGE n"   */
/*  suffix, and never looked at [vars] at all.  Every large homebrew is     */
/*  past all three limits at once: Cloudfire has 24 mapping lines of which  */
/*  13 are pages of $A000 plus jlp=3, Pacmanthology has 30 of which 27 are  */
/*  paged plus ROM at $8000-$9FFF under the JLP window.  None of that is    */
/*  expressible in the icartrom_t the old path built -- it has one flat 64K */
/*  image[] and no paged storage, and icart.c's own loader warns that a JLP */
/*  title with readable Intellicart segments in $80-$9F cannot work.        */
/*                                                                          */
/*  legacy_t is the object that can: mem/mem.c paged ROMs for "PAGE n",     */
/*  and legacy_read()/legacy_write()'s jlp_accel_on check for the JLP       */
/*  window flip.  Nothing here is new emulation -- it is the same code path */
/*  "jzintv game.bin" has always taken, which is also what makes a pushed   */
/*  cart and a locally loaded one behave identically.                       */
/*                                                                          */
/*  The RP2040's *local-storage* loader (load_cfg() in intellicart.c) does  */
/*  parse "PAGE n" and [vars] jlp; only its pushed-cfg path is narrower,    */
/*  and only because an MCU has to stage into fixed tables.  So this is     */
/*  catching up to the firmware, not diverging from it -- with the one      */
/*  consequence that jzIntv now boots pushed carts the firmware's pushed    */
/*  path still rejects, and that its 0x2800-word cart RAM budget            */
/*  (FUJI_BOOT_ERR_RAM) no longer applies here.                             */
/*                                                                          */
/*  Returns nonzero on success, having already registered the new cart.     */
/*  On failure FUJI_MB_BOOT_ERR is set and the outgoing cart is untouched.  */
/* ======================================================================== */
LOCAL int fujinet_apply_bincfg(fujinet_t *const fn, int *const disable_mb)
{
    char      binpath[512], cfgpath[512];
    legacy_t *l;
    int       jlp_accel = 0, jlp_flash = 0;

    snprintf(binpath, sizeof(binpath), "%s%cjzintv-fnboot.bin",
             fn->bootstage_dir, PATH_SEP);
    snprintf(cfgpath, sizeof(cfgpath), "%s%cjzintv-fnboot.cfg",
             fn->bootstage_dir, PATH_SEP);

    /*  bc_parse_cfg()/bc_read_data() read from files, so the push has to    */
    /*  land on disk before the loader can see it.  A staging failure is a   */
    /*  short image from everything downstream's point of view, so report    */
    /*  it as one.                                                           */
    if (fujinet_write_file(binpath, fn->rom_buf, fn->rom_len) ||
        fujinet_write_file(cfgpath, fn->cfg_buf, fn->cfg_len))
    {
        fprintf(stderr, "FUJINET: could not stage the pushed cart under "
                        "'%s'\n", fn->bootstage_dir);
        fn->ram[OFS(FUJI_MB_BOOT_ERR)] = FUJI_BOOT_ERR_TRUNCATED;
        return 0;
    }

    l = CALLOC(legacy_t, 1);
    if (!l)
    {
        fn->ram[OFS(FUJI_MB_BOOT_ERR)] = FUJI_BOOT_ERR_TRUNCATED;
        return 0;
    }

    /*  -1/-1 for the JLP flags is what cfg.c passes when the user gave no  */
    /*  --jlp options: the .cfg's own [vars] decide.  rand_mem 0 keeps a    */
    /*  pushed boot reproducible.                                           */
    if (legacy_read_bincfg(binpath, cfgpath, l, fn->cpu, -1, -1, 0))
    {
        free(l);
        fn->ram[OFS(FUJI_MB_BOOT_ERR)] = FUJI_BOOT_ERR_CFGBAD;
        return 0;
    }

    /* -------------------------------------------------------------------- */
    /*  Everything that needs the parsed config has to happen here:          */
    /*  legacy_register() frees l->bc on its way out.                        */
    /* -------------------------------------------------------------------- */
    jlp_accel = l->jlp_accel;
    if (l->bc && l->bc->metadata)
        jlp_flash = l->bc->metadata->jlp_flash;

    if (l->bc)
    {
        const bc_memspan_t *span;
        for (span = l->bc->span; span; span = (const bc_memspan_t *)span->l.next)
            if (fujinet_mailbox_overlap(span->s_addr,
                                        span->e_addr - span->s_addr + 1))
                *disable_mb = 1;
    }

    /*  JLP's window is $8000-$9FFF, which contains the mailbox outright.    */
    if (jlp_accel > 0)
        *disable_mb = 1;

    /* -------------------------------------------------------------------- */
    /*  Commit.  Past this point the old cart is gone, so nothing below may  */
    /*  fail in a way that leaves the console with no cart at all.           */
    /* -------------------------------------------------------------------- */
    fujinet_retire_cart(fn);
    cp1600_uncacheable_all((cp1600_t *)fn->cpu);

    legacy_init_periph(l);
    legacy_register(l, fn->bus, (cp1600_t *)fn->cpu);
    fujinet_recacheable_console(fn);
    fn->legacy = l;

    if (jlp_accel > 0 && fujinet_apply_jlp(fn, jlp_accel, jlp_flash))
        fprintf(stderr, "FUJINET: JLP setup failed; the cart will run "
                        "without it\n");

    return 1;
}

/* ======================================================================== */
/*  FUJINET_APPLY_ROM -- Turns the buffered ROM push (fn->rom_buf) into a   */
/*                       registered cartridge, with the same three-way      */
/*                       format precedence inty_cart.c's                    */
/*                       apply_boot_mapping() uses:                         */
/*                                                                          */
/*                         1. An Intellicart/CC3 self-describing header,    */
/*                            through jzIntv's own icartrom_decode() --     */
/*                            bankswitching, CRCs, metadata and all -- into */
/*                            the live icart_t.                             */
/*                         2. A .cfg sibling, through jzIntv's own legacy   */
/*                            BIN+CFG loader.  See fujinet_apply_bincfg()   */
/*                            for why that and not the icart_t -- this is   */
/*                            the branch that differs most from the RP2040. */
/*                         3. A size-guess table for a bare flat .bin, also */
/*                            into the icart_t.                             */
/*                                                                          */
/*                       Returns nonzero on success, with the new cart      */
/*                       already registered on the bus and the old one      */
/*                       unbound; the caller still owes it the CPU decode-  */
/*                       cache flush and the console reset.                 */
/* ======================================================================== */
LOCAL int fujinet_apply_rom(fujinet_t *const fn)
{
    if (!fn->icart || !fn->cpu || !fn->bus)
        return 0;

    icart_t *const ic = (icart_t *)fn->icart;
    int disable_mb = 0;

    /*  Ran out of memory growing rom_buf mid-push: what we hold is a       */
    /*  short image, and booting it would be worse than saying so.          */
    if (fn->rom_truncated)
    {
        fn->ram[OFS(FUJI_MB_BOOT_ERR)] = FUJI_BOOT_ERR_TRUNCATED;
        return 0;
    }

    /* Decode/validate the incoming push into a scratch icartrom_t, NOT
     * ic->rom directly. ic->rom is still the live, currently-registered
     * cartridge image -- on a hot-load, that's the config ROM's own boot
     * menu, which may still be mid-execution of the very polling loop that
     * triggered this call (periph_reset() below only takes effect once the
     * bus goes idle, and even a synchronous CPU reset only stops *further*
     * fetches, it doesn't undo damage already done). Mutating ic->rom in
     * place and then discovering the push was bad (short/empty transfer,
     * unparseable .cfg, bad header, ...) would leave the boot ROM's own
     * code permanently zeroed with nothing to fall back to -- exactly what
     * happened here: an empty ROM push wiped the config ROM's own $D000+
     * segment via icartrom_init(), the mapping attempt then legitimately
     * failed and left ic->rom empty, and the still-running boot menu
     * immediately faulted on its own (now-zeroed) code the instant it
     * fetched past the failed transaction. Only commit into ic->rom once
     * we know we have a fully valid replacement. */
    icartrom_t *const scratch = (icartrom_t *)calloc(1, sizeof(icartrom_t));
    if (!scratch)
        return 0;

    int committed = 0;

    /* Try the Intellicart/CC3 self-describing header first -- same format
     * inty_cart.c's ROMFMT_HDR path decodes, and jzIntv already has a full
     * decoder for it (bankswitching, CRCs, metadata and all), so prefer it
     * over hand-rolling. */
    if (fn->rom_len >= 53 &&
        icartrom_decode(scratch, fn->rom_buf, (int)fn->rom_len, 0, 0) >= 0)
    {
        committed = 1;
        /* Scan the decoded 256-word-page attribute bitmaps for anything
         * live over the mailbox window (pages $9C-$9F). */
        for (uint32_t pg = FUJI_MB_ADDR_LO >> 8; pg <= FUJI_MB_ADDR_HI >> 8; pg++)
            if (((scratch->readable[pg >> 5] | scratch->writable[pg >> 5]) >>
                 (pg & 31)) & 1)
                disable_mb = 1;
    } else if (fn->have_cfg && fn->rom_len)
    {
        /* ------------------------------------------------------------------ */
        /*  Not a header image, but a .cfg sibling came with it: that's a      */
        /*  BIN+CFG pair, and jzIntv has a loader for those already.  It       */
        /*  registers the cart itself (a legacy_t's paged ROMs are separate    */
        /*  peripherals), so unwind the scratch icartrom_t and hand off.       */
        /*                                                                     */
        /*  The header probe still goes first, matching both inty_cart.c's     */
        /*  precedence and legacy_bincfg()'s own .rom-before-.bin ordering:    */
        /*  a .rom that happens to sit beside a same-basename .cfg gets both   */
        /*  pushed, and decoding it as a flat .bin would be silent garbage.    */
        /*  icartrom_decode() validates the segment table and CRCs, so it      */
        /*  won't claim a real .bin by accident.                               */
        /* ------------------------------------------------------------------ */
        if (scratch->metadata)
            free(scratch->metadata);
        free(scratch);

        if (!fujinet_apply_bincfg(fn, &disable_mb))
            return 0;

        if (disable_mb)
        {
            fn->mailbox_active = 0;
            if (fn->debug)
                jzp_printf("FUJINET: mapping covers the mailbox window -- "
                           "mailbox disabled for this session\n");
        }
        return 1;
    } else
    {
        icartrom_init(scratch); /* A failed decode may have partially
                                  * populated scratch. */

        uint32_t word_count = (uint32_t)(fn->rom_len / 2);
        uint16_t *words = word_count
                         ? (uint16_t *)malloc(word_count * sizeof(uint16_t))
                         : NULL;

        if (word_count && words)
        {
            for (uint32_t i = 0; i < word_count; i++)
                words[i] = ((uint16_t)fn->rom_buf[2*i] << 8) |
                            fn->rom_buf[2*i + 1];

            {
                /* Size-guess table, same as inty_cart.c's
                 * apply_boot_mapping() default branch.  Only reachable with
                 * no .cfg at all: a pushed pair went to the legacy loader
                 * above, and refusing to guess is the point -- a wrong map
                 * is worse than an honest FUJI_BOOT_ERR_NOMAP. */
                uint32_t nseg = 0, from[3], to[3], rom_addr[3];
                switch (fn->rom_len)
                {
                case 4096:  nseg=1; from[0]=0; to[0]=0x07FF; rom_addr[0]=0x5000; break;
                case 8192:  nseg=1; from[0]=0; to[0]=0x0FFF; rom_addr[0]=0x5000; break;
                case 12288: nseg=1; from[0]=0; to[0]=0x17FF; rom_addr[0]=0x5000; break;
                case 16384: nseg=1; from[0]=0; to[0]=0x1FFF; rom_addr[0]=0x5000; break;
                case 24576:
                    nseg=2;
                    from[0]=0;      to[0]=0x1FFF; rom_addr[0]=0x5000;
                    from[1]=0x2000; to[1]=0x2FFF; rom_addr[1]=0xD000;
                    break;
                case 32768:
                    nseg=3;
                    from[0]=0;      to[0]=0x1FFF; rom_addr[0]=0x5000;
                    from[1]=0x2000; to[1]=0x2FFF; rom_addr[1]=0xD000;
                    from[2]=0x3000; to[2]=0x3FFF; rom_addr[2]=0xF000;
                    break;
                case 40960: /* 20K words: 8K at $5000, 12K contiguous at $D000 */
                    nseg=2;
                    from[0]=0;      to[0]=0x1FFF; rom_addr[0]=0x5000;
                    from[1]=0x2000; to[1]=0x4FFF; rom_addr[1]=0xD000;
                    break;
                case 49152: /* 24K words: the common INTV shape, 8K/12K/4K */
                    nseg=3;
                    from[0]=0;      to[0]=0x1FFF; rom_addr[0]=0x5000;
                    from[1]=0x2000; to[1]=0x4FFF; rom_addr[1]=0x9000;
                    from[2]=0x5000; to[2]=0x5FFF; rom_addr[2]=0xD000;
                    break;
                default:
                    nseg = 0;
                    break;
                }

                if (nseg > 0)
                {
                    committed = 1;
                    for (uint32_t i = 0; i < nseg; i++)
                    {
                        if (fujinet_mailbox_overlap(rom_addr[i], to[i] - from[i] + 1))
                            disable_mb = 1;
                        icartrom_addseg(scratch, words + from[i], rom_addr[i],
                                        to[i] - from[i] + 1, ICARTROM_READ, 0);
                    }
                }
            }
        }

        free(words);
    }

    if (committed)
    {
        icartrom_t *const rom = &ic->rom;
        if (rom->metadata)
            free(rom->metadata);
        *rom = *scratch;
        memset(ic->bs_tbl, 0, sizeof(ic->bs_tbl));
        if (disable_mb)
        {
            fn->mailbox_active = 0;
            if (fn->debug)
                jzp_printf("FUJINET: mapping covers the mailbox window -- "
                           "mailbox disabled for this session\n");
        }

        /* Drop the outgoing cart's address decode bindings first.
         * periph_register() only ever adds, so without this the config ROM's
         * own map keeps answering next to the new game's -- and on the
         * Intellivision's wire-AND bus the CPU gets the AND of the two. The
         * config ROM declares 8-bit RAM over $8000-$9BFF, so every pushed
         * game with a segment there (the 12K-at-$9000 shape most 32K/48K
         * carts use) came back with its high byte silently masked off. */
        fujinet_retire_cart(fn);
        cp1600_uncacheable_all((cp1600_t *)fn->cpu);
        icart_register(ic, fn->bus, (cp1600_t *)fn->cpu, fn->cache_flags);
        fujinet_recacheable_console(fn);
    } else if (scratch->metadata)
    {
        free(scratch->metadata);
    }

    free(scratch);
    return committed;
}

/* ======================================================================== */
/*  FUJINET_HANDLE_DBC_FRAME -- One inbound FUJI_DEVICEID_DBC frame, mid-   */
/*                       MOUNT_IMAGE-transaction. Mirrors the RP2040's own  */
/*                       dbc_inbound_handler() (inty_cart.c) in shape:      */
/*                       decodes the pushed ROM (+ optional .cfg sibling)   */
/*                       into the live icart_t via fujinet_apply_rom() and  */
/*                       resets the console into it -- see that function    */
/*                       and periph_reset()/cp1600_reset() for why this is  */
/*                       safe to do from inside a peripheral's own tick.    */
/*                       With --fujinet-bootdump=PREFIX it also writes what */
/*                       it receives to PREFIX.rom/.cfg, independent of the */
/*                       reload, so they can be diffed against the source   */
/*                       files as an end-to-end test of the ESP32-side      */
/*                       push.                                              */
/* ======================================================================== */
LOCAL void fujinet_handle_dbc_frame(fujinet_t *const fn, const fb_reply_t *req)
{
    /* Abort-CLOSE (payload 0x01): the bridge detected a short/failed
     * transfer -- discard the stream without treating the partial data as
     * boot-ready. Bare CLOSE = commit, so old peers stay compatible. */
    int aborted = (req->command == NETCMD_CLOSE &&
                   req->data_len > 0 && req->data[0] == 0x01);

    if (req->command == NETCMD_OPEN)
    {
        int stream = (req->data_len > 0) ? req->data[0] : 0;

        /* OPEN payload is the stream id followed by that stream's total
         * size, four little-endian bytes -- see push_stream()'s open_hdr in
         * fujinet-firmware's lib/media/rs232/diskTypeROM.cpp, and the
         * identical decode in the RP2040's own dbc_inbound_handler(). The
         * length guard is the compatibility hinge in the other direction:
         * a sender predating the header sends the stream id alone, and we
         * fall back to the fixed-size guess below, exactly as before. */
        uint32_t total = 0;
        if (req->data_len >= 5)
            total = (uint32_t)req->data[1]        |
                    ((uint32_t)req->data[2] << 8)  |
                    ((uint32_t)req->data[3] << 16) |
                    ((uint32_t)req->data[4] << 24);

        fujinet_dbc_stream_open(fn, stream, total);
        fn->ram[OFS(FUJI_MB_BOOT_STATE)] = (stream == 1) ? FUJI_BOOT_OPENING
                                                          : FUJI_BOOT_XFER;
        if (stream == 0)
        {
            fn->ram[OFS(FUJI_MB_BOOT_PCT)] = 0;
            fn->ram[OFS(FUJI_MB_BOOT_ERR)] = 0;
            if (fn->debug)
                jzp_printf("FUJINET: ROM push, %u bytes%s\n",
                           (unsigned)total,
                           total ? "" : " (size not sent; progress estimated)");
        }
    }
    else if (req->command == NETCMD_WRITE)
    {
        fujinet_dbc_stream_write(fn, req->data, req->data_len);
        if (fn->bootdump_stream == 0)
        {
            /* Mirrors bootmap_pct() in the RP2040 firmware's bootmap.c,
             * including its fallback denominator: without a size from the
             * sender there is nothing to divide by, and a fixed 32KB at
             * least makes the bar move. The clamp at 99 is deliberate on
             * both sides -- CLOSE publishes 100, once the cart is actually
             * mapped, so the bar never claims a boot that hasn't happened. */
            uint32_t total = fn->rom_expected ? fn->rom_expected : 32768;
            uint32_t pct;
            uint8_t  was = fn->ram[OFS(FUJI_MB_BOOT_PCT)];
            fn->bootdump_rom_bytes += req->data_len;
            pct = (fn->bootdump_rom_bytes * 100) / total;
            fn->ram[OFS(FUJI_MB_BOOT_PCT)] = (uint8_t)(pct > 99 ? 99 : pct);

            /*  Only on a change, so this is ~100 lines across a whole push  */
            /*  rather than one per 512-byte frame -- enough to see the bar  */
            /*  advance (or notice it wedged) without burying the trace.     */
            if (fn->debug && fn->ram[OFS(FUJI_MB_BOOT_PCT)] != was)
                jzp_printf("FUJINET: boot %u%% (%u/%u bytes)\n",
                           (unsigned)fn->ram[OFS(FUJI_MB_BOOT_PCT)],
                           (unsigned)fn->bootdump_rom_bytes,
                           (unsigned)total);
        }
    }
    else if (req->command == NETCMD_CLOSE)
    {
        int was_rom = (fn->bootdump_stream == 0);
        fujinet_dbc_stream_close(fn);
        if (aborted && !was_rom)
        {
            fn->have_cfg = 0;
            fn->cfg_len  = 0;
        }
        else if (was_rom && aborted)
        {
            fn->ram[OFS(FUJI_MB_BOOT_STATE)] = FUJI_BOOT_FAILED;
            fn->ram[OFS(FUJI_MB_BOOT_ERR)]   = FUJI_BOOT_ERR_TRUNCATED;
            fn->have_cfg = 0;
            fn->cfg_len  = 0;
        }
        else if (was_rom)
        {
            fn->ram[OFS(FUJI_MB_BOOT_STATE)] = FUJI_BOOT_MAPPING;
            int boot_ok = fujinet_apply_rom(fn);
            /* consume the cfg so the next push starts clean */
            fn->have_cfg = 0;
            fn->cfg_len  = 0;
            if (boot_ok)
            {
                fn->ram[OFS(FUJI_MB_BOOT_PCT)] = 100;
                /* fujinet_apply_rom() has already unbound the outgoing cart
                 * and registered the new one (which flavor depends on the
                 * push format -- see its header). What it has NOT done, and
                 * cannot, is tell the CP-1610 about instructions it already
                 * decoded and cached at those addresses from whatever
                 * occupied them before (the config ROM's boot menu, or a
                 * previously-loaded game). Left alone, the CPU would keep
                 * executing those stale decodes -- including a cached HLT
                 * from a spot that used to be unmapped -- right over the top
                 * of the freshly-pushed ROM's real bytes. Flush the whole
                 * decode cache so every fetch after reset re-decodes from
                 * the new memory contents. */
                cp1600_invalidate((cp1600_t *)fn->cpu, 0x0000, 0xFFFF);
                /* On the icart path, fujinet_apply_rom() just memset-zeroed
                 * and repopulated fn->icart's *live* rom.image[] in place --
                 * the very same icart_t that's still registered on the bus
                 * and backing whatever the CPU is executing right now (the
                 * config ROM's own mailbox-polling loop, mid-"DO NOT POWER
                 * OFF"). Any
                 * address the old boot ROM used that the new game's segment
                 * map doesn't cover (e.g. the boot menu's own code, off in
                 * $D000+) is now permanently zero, i.e. HLT.
                 *
                 * periph_reset(fn->bus) below is necessarily deferred when
                 * called mid-tick (the common case, since we're called from
                 * fujinet_tick() itself): it just latches bus->pend_reset
                 * and actually runs at the end of periph_run()'s current
                 * batch -- and every other due peripheral, CP-1610 included,
                 * still gets ticked (potentially executing many more
                 * instructions against the now-zeroed ROM) before that
                 * batch ends. That's exactly the HLT this hot-swap can hit.
                 *
                 * cp1600_reset() sets cp1600->pend_reset directly, which
                 * cp1600_run() checks before fetching *every* instruction --
                 * calling it here, synchronously, stops the CPU from
                 * fetching anything else from the torn-down ROM before the
                 * bus-level reset below gets around to the rest of the
                 * peripherals (STIC, PSG, etc). */
                cp1600_reset(&((cp1600_t *)fn->cpu)->periph);
                /* Matches resetCart() on real hardware: the *real*
                 * MOUNT_IMAGE ACK/NAK from the fujinet-firmware bridge
                 * (device FUJI_DEVICEID_FUJINET) still arrives separately
                 * and completes the transaction normally through
                 * fujinet_finish_txn() below, same as it always did -- the
                 * console reset doesn't need to wait for it. */
                periph_reset(fn->bus);
            }
            else
            {
                fn->ram[OFS(FUJI_MB_BOOT_STATE)] = FUJI_BOOT_FAILED;
                if (!fn->ram[OFS(FUJI_MB_BOOT_ERR)])
                    fn->ram[OFS(FUJI_MB_BOOT_ERR)] = FUJI_BOOT_ERR_NOMAP;
            }
        }
    }

    fujinet_dbc_send_ack(fn);
}

/* ======================================================================== */
/*  FUJINET_RX_RESYNC -- Discard a partial or unparseable frame and go      */
/*                       looking for the next delimiter.                    */
/*                                                                          */
/*  The RP2040 reference (fujibus_usb.c's fujibus_transact()) keeps rxlen   */
/*  and seen_end as *locals* inside one deadline loop, so an over-long or   */
/*  malformed frame just times the transaction out and the next one starts  */
/*  clean. Here they live in fujinet_t and persist across ticks, because    */
/*  a frame can straddle any number of them. That means nothing clears      */
/*  them on the failure paths: once rxlen reaches sizeof(rxbuf) without     */
/*  two ENDs, the accumulate loop's own `rxlen < sizeof(rxbuf)` guard is    */
/*  false on every subsequent tick -- not one more byte is ever drained,    */
/*  no frame is ever parsed, no DBC ACK is ever sent, and a ROM push wedges */
/*  forever with FUJI_MB_BOOT_PCT frozen at whatever it last reached (the   */
/*  console sits on "BOOTING / DO NOT POWER OFF" with a stuck bar). Same    */
/*  for a parse failure, which additionally leaves the byte stream desynced */
/*  mid-frame so every following parse fails too.                           */
/* ======================================================================== */
LOCAL void fujinet_rx_resync(fujinet_t *const fn)
{
    fn->rxlen    = 0;
    fn->seen_end = 0;
}

/* ======================================================================== */
/*  FUJINET_TICK      -- periph.tick: drives the state machine.             */
/* ======================================================================== */
LOCAL uint32_t fujinet_tick(periph_t *const per, uint32_t len)
{
    fujinet_t *const fn = PERIPH_AS(fujinet_t, per);

    switch (fn->state)
    {
        case FUJINET_ST_DISCONNECTED:
        {
            if (per->now < fn->reconnect_at)
                break;

            if (fn->debug)
                jzp_printf("FUJINET: connecting to %s:%d\n",
                           fn->host, fn->port);

            if (fn_sock_connect_begin(&fn->sock, fn->host, fn->port))
            {
                /*  Couldn't even start (DNS failure, etc).  Back off and   */
                /*  try again later.                                       */
                fn->reconnect_at =
                    per->now + FUJINET_MS(FUJINET_RECONNECT_BACKOFF_MS);
                break;
            }

            fn->deadline = per->now + FUJINET_MS(FUJINET_CONNECT_TIMEOUT_MS);
            fn->state    = FUJINET_ST_CONNECTING;
            break;
        }

        case FUJINET_ST_CONNECTING:
        {
            const int r = fn_sock_connect_poll(&fn->sock);

            if (r > 0)
            {
                if (fn->debug)
                    jzp_printf("FUJINET: connected\n");
                fn->ram[OFS(FUJI_MB_LINK)] = 1;
                fn->state = FUJINET_ST_IDLE;
                break;
            }
            if (r < 0 || per->now >= fn->deadline)
            {
                fujinet_fail_link(fn, per);
                break;
            }
            break;   /*  Still pending.                                    */
        }

        case FUJINET_ST_IDLE:
        {
            if (fn->ram[OFS(FUJI_MB_SEQ)] != fn->ram[OFS(FUJI_MB_ACKSEQ)])
                fujinet_start_txn(fn, per);
            break;
        }

        case FUJINET_ST_SENDING:
        {
            if (per->now >= fn->deadline)
            {
                fujinet_finish_txn(fn, FB_ETIMEOUT, NULL);
                break;
            }

            while (fn->txsent < fn->txlen)
            {
                const int n = fn_sock_send(&fn->sock, fn->txbuf + fn->txsent,
                                           fn->txlen - fn->txsent);
                if (n < 0)
                {
                    fujinet_fail_link(fn, per);
                    return len;
                }
                if (n == 0)
                    break;   /*  Would block; try again next tick.          */
                fn->txsent += (size_t)n;
            }

            if (fn->txsent >= fn->txlen)
                fn->state = FUJINET_ST_RXWAIT;
            break;
        }

        case FUJINET_ST_RXWAIT:
        {
            fb_reply_t reply;

            if (per->now >= fn->deadline)
            {
                fujinet_finish_txn(fn, FB_ETIMEOUT, NULL);
                break;
            }

            while (fn->seen_end < 2 && fn->rxlen < sizeof(fn->rxbuf))
            {
                uint8_t b;
                int     eof = 0;
                const int n = fn_sock_recv(&fn->sock, &b, 1, &eof);

                if (n < 0 || eof)
                {
                    fujinet_fail_link(fn, per);
                    return len;
                }
                if (n == 0)
                    break;   /*  Would block; try again next tick.          */

                /*  A leading END, or a run of them: the encoder brackets   */
                /*  every frame with one on each side, so back-to-back      */
                /*  frames put two 0xC0 in a row on the wire. Collapsing    */
                /*  them here (rather than counting the second as this      */
                /*  frame's trailing END and handing the parser an empty    */
                /*  "C0 C0") is what standard SLIP does with empty frames,  */
                /*  and it's also how a resync lands back on a boundary     */
                /*  without caring which of the two delimiters it found.    */
                if (b == 0xC0 && fn->rxlen <= 1 && fn->seen_end <= 1)
                {
                    fn->rxbuf[0] = b;
                    fn->rxlen    = 1;
                    fn->seen_end = 1;
                    continue;
                }

                fn->rxbuf[fn->rxlen++] = b;
                if (b == 0xC0 && ++fn->seen_end == 2)
                    break;
            }

            /*  Buffer filled without ever closing the frame. Drop it and   */
            /*  resync instead of returning with the loop guard permanently */
            /*  false -- see fujinet_rx_resync(). The bridge is still       */
            /*  blocking on an ACK we can't send for a frame we don't have, */
            /*  so it will abort the push on its own timeout and we report  */
            /*  that through the normal CLOSE path.                         */
            if (fn->seen_end < 2 && fn->rxlen >= sizeof(fn->rxbuf))
            {
                if (fn->debug)
                    jzp_printf("FUJINET: oversized frame (%u bytes, no END) "
                               "-- discarding and resyncing\n",
                               (unsigned)fn->rxlen);
                fujinet_rx_resync(fn);
                break;
            }

            if (fn->seen_end < 2)
                break;   /*  Not a full frame yet.                         */

            if (!fujibus_parse_reply(fn->rxbuf, fn->rxlen, &reply))
            {
                fujinet_rx_resync(fn);
                fujinet_finish_txn(fn, FB_EBADFRAME, NULL);
                break;
            }

            /*  A DBC-addressed frame is an inbound request from the       */
            /*  fujinet-firmware bridge (pushing a ROM/.cfg mid-           */
            /*  MOUNT_IMAGE-transaction), not the reply this transaction   */
            /*  is actually waiting for -- consume it, ACK it, and keep    */
            /*  waiting, with a fresh deadline (same reasoning as the      */
            /*  RP2040's own demux in fujibus_usb.c: a frame arriving at   */
            /*  all proves the link is alive and busy, not stalled).       */
            if (reply.device == FUJI_DEVICEID_DBC)
            {
                fujinet_handle_dbc_frame(fn, &reply);
                fn->rxlen    = 0;
                fn->seen_end = 0;
                fn->deadline = per->now + FUJINET_MS(FUJINET_MOUNT_TIMEOUT_MS);
                break;
            }

            fujinet_finish_txn(fn, FB_OK, &reply);
            break;
        }

        default:
            break;
    }

    return len;
}

/* ======================================================================== */
/*  FUJINET_DTOR      -- periph.dtor: closes the socket, frees strdup'd     */
/*                       state.                                             */
/* ======================================================================== */
LOCAL void fujinet_dtor(periph_t *const per)
{
    fujinet_t *const fn = PERIPH_AS(fujinet_t, per);

    fn_sock_close(&fn->sock);
    fujinet_dbc_stream_close(fn);
    CONDFREE(fn->host);
    CONDFREE(fn->bootdump_prefix);
    CONDFREE(fn->bootstage_dir);
    CONDFREE(fn->rom_buf);

    /*  fn->legacy / fn->retired[] / fn->jlp are peripherals in their own    */
    /*  right and sit on the bus' list, which is what runs their dtors --    */
    /*  and may already have, since periph_delete() walks that list in an    */
    /*  order we don't control.  Freeing the containers here could be a      */
    /*  double free of everything inside them.  jzIntv doesn't free any of   */
    /*  its other peripherals either (they're members of cfg_t); these just  */
    /*  came off the heap instead.  Only the index array is ours.            */
    CONDFREE(fn->retired);
}

/* ======================================================================== */
/*  FUJINET_PICK_BOOTSTAGE_DIR -- Falls back to a system scratch directory  */
/*                       when --fujinet-bootdir wasn't given.               */
/*                                                                          */
/*  The hosts that embed this emulator (fujinet-go-intv and its desktop     */
/*  sibling) always pass their own writable data directory, which is the    */
/*  only thing that works on Android, where an app process has no TMPDIR    */
/*  and no writable cwd.  This is for standalone `jzintv --fujinet` runs.   */
/* ======================================================================== */
LOCAL char *fujinet_pick_bootstage_dir(void)
{
    static const char *const vars[] = { "TMPDIR", "TEMP", "TMP" };
    unsigned i;

    for (i = 0; i < sizeof(vars) / sizeof(vars[0]); i++)
    {
        const char *v = getenv(vars[i]);
        if (v && *v)
            return strdup(v);
    }

#if defined(_WIN32) || defined(WIN32)
    return strdup(".");
#else
    return strdup("/tmp");
#endif
}

int fujinet_init(fujinet_t *const fujinet, const char *const host,
                  const int port, const int debug,
                  const char *const bootdump_prefix,
                  const char *const bootstage_dir,
                  void *const icart, void *const cpu,
                  periph_bus_t *const bus, const uint32_t cache_flags)
{
    if (!fujibus_selftest())
    {
        fprintf(stderr, "ERROR:  FujiNet codec self-test failed -- this "
                        "build's fujibus.c does not match "
                        "fujinet-firmware's FujiBusPacket wire format.\n");
        return -1;
    }

    memset(fujinet, 0, sizeof(*fujinet));

    fujinet->host  = strdup(host ? host : "localhost");
    fujinet->port  = port > 0 ? port : 1985;
    fujinet->debug = debug;

    fujinet->bootdump_prefix = bootdump_prefix ? strdup(bootdump_prefix) : NULL;
    fujinet->bootdump_fh     = NULL;
    fujinet->bootdump_stream = -1;

    fujinet->bootstage_dir = bootstage_dir && *bootstage_dir
                            ? strdup(bootstage_dir)
                            : fujinet_pick_bootstage_dir();
    if (!fujinet->bootstage_dir)
    {
        fprintf(stderr, "ERROR:  FujiNet: out of memory\n");
        return -1;
    }
    fujinet_mkdir(fujinet->bootstage_dir);

    fujinet->icart       = icart;
    fujinet->cpu         = cpu;
    fujinet->bus         = bus;
    fujinet->cache_flags = cache_flags;

    fn_sock_init(&fujinet->sock);
    fujinet->state          = FUJINET_ST_DISCONNECTED;
    fujinet->reconnect_at   = 0; /*  Try to connect immediately.            */
    fujinet->mailbox_active = 1; /*  Session flag; see fujinet.h.           */

    fujinet_paint_ident(fujinet);

    fujinet->periph.read      = fujinet_read;
    fujinet->periph.write     = fujinet_write;
    fujinet->periph.peek      = fujinet_read;
    fujinet->periph.poke      = fujinet_write;
    fujinet->periph.reset     = fujinet_reset;
    fujinet->periph.dtor      = fujinet_dtor;
    fujinet->periph.tick      = fujinet_tick;
    fujinet->periph.min_tick  = PERIPH_HZ(1000);
    fujinet->periph.max_tick  = PERIPH_HZ(200);
    fujinet->periph.addr_base = 0x9C00;
    fujinet->periph.addr_mask = FUJINET_WINDOW_SIZE - 1;
    fujinet->periph.ser_init  = NULL;

    if (debug)
        jzp_printf("FUJINET: enabled, target %s:%d\n",
                   fujinet->host, fujinet->port);

    return 0;
}
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
