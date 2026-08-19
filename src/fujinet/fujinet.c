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
#include "icart/icart.h"
#include "misc/jzprint.h"
#include "fujinet/fujibus.h"
#include "fujinet/fujinet.h"

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
LOCAL void fujinet_dbc_stream_open(fujinet_t *const fn, int stream)
{
    fn->bootdump_stream = stream;
    if (stream == 0)
    {
        fn->bootdump_rom_bytes = 0;
        fn->rom_len = 0;
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

/* Growable append into fn->rom_buf, doubling capacity as needed -- a real
 * game tops out well under 128KB, so this converges in a handful of
 * reallocs. */
LOCAL void fujinet_rom_buf_append(fujinet_t *const fn,
                                   const uint8_t *data, uint16_t len)
{
    if (!data || !len)
        return;

    if (fn->rom_len + len > fn->rom_cap)
    {
        size_t newcap = fn->rom_cap ? fn->rom_cap * 2 : 4096;
        while (newcap < fn->rom_len + len)
            newcap *= 2;
        fn->rom_buf = (uint8_t *)realloc(fn->rom_buf, newcap);
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
/*  FUJINET_PARSE_CFG_MAPPING -- Port of the pico's parse_pushed_cfg() +    */
/*                       the cfg half of apply_boot_mapping().              */
/* ======================================================================== */
/* Return: >0 = segments committed to `rom`; 0 = no [mapping] lines at all
 * (caller falls back to the size table); -1 = the cfg WAS the mapping
 * source but is unusable -- FUJI_MB_BOOT_ERR already set, don't fall back.
 * *disable_mb is set when the mapping covers the mailbox window: the boot
 * proceeds with the mailbox disabled for the session (the RP2040 does the
 * same). Mirrors the pico's parse_pushed_cfg() + apply_boot_mapping():
 * [mapping]/[memattr] sections, clamp-at-EOF (bin2rom semantics -- known
 * cfg collections describe each title's largest layout), nothing below
 * $4000, cfg RAM total capped at the hardware's 0x2800-word budget so a
 * cart that can't run on real hardware fails identically here. */
LOCAL int fujinet_parse_cfg_mapping_into(icartrom_t *const rom, fujinet_t *const fn,
                                          const uint16_t *const words,
                                          uint32_t word_count, int *disable_mb)
{
    enum { MAX_CFG_SEGS = 16, MAX_CFG_RAM = 8 };
    uint32_t seg_from[MAX_CFG_SEGS], seg_to[MAX_CFG_SEGS], seg_rom[MAX_CFG_SEGS];
    uint32_t ram_from[MAX_CFG_RAM], ram_to[MAX_CFG_RAM];
    uint8_t  ram_width[MAX_CFG_RAM];
    int nseg = 0, nram = 0, truncated = fn->cfg_truncated;
    uint32_t total_ram = 0;
    enum { SEC_NONE, SEC_MAPPING, SEC_MEMATTR } section = SEC_NONE;

    const char *p   = (const char *)fn->cfg_buf;
    const char *end = (const char *)fn->cfg_buf + fn->cfg_len;

    while (p < end)
    {
        const char *nl = memchr(p, '\n', (size_t)(end - p));
        const char *line_end = nl ? nl : end;
        size_t linelen = (size_t)(line_end - p);

        char tmp[96];
        size_t copylen = linelen < sizeof(tmp) - 1 ? linelen : sizeof(tmp) - 1;
        memcpy(tmp, p, copylen);
        tmp[copylen] = 0;

        if (strstr(tmp, "[mapping]"))
            section = SEC_MAPPING;
        else if (strstr(tmp, "[memattr]"))
            section = SEC_MEMATTR;
        else if (linelen > 0 && tmp[0] == '[')
            section = SEC_NONE;
        else if (section == SEC_MAPPING && linelen > 0 && tmp[0] == '$')
        {
            unsigned int from = 0, to = 0, dst = 0;
            if (sscanf(tmp, " $%x - $%x = $%x", &from, &to, &dst) == 3 &&
                to >= from)
            {
                if (nseg < MAX_CFG_SEGS)
                {
                    seg_from[nseg] = from;
                    seg_to[nseg]   = to;
                    seg_rom[nseg]  = dst;
                    nseg++;
                }
                else
                    truncated = 1;
            }
        }
        else if (section == SEC_MEMATTR && linelen > 0 && tmp[0] == '$')
        {
            unsigned int from = 0, to = 0, width = 0;
            char type[4];
            if (sscanf(tmp, " $%x - $%x = %3s %u", &from, &to, type, &width) == 4 &&
                (width == 8 || width == 16) && to >= from)
            {
                if (nram < MAX_CFG_RAM)
                {
                    ram_from[nram]  = from;
                    ram_to[nram]    = to;
                    ram_width[nram] = (uint8_t)width;
                    nram++;
                }
                else
                    truncated = 1;
            }
        }

        p = nl ? nl + 1 : end;
    }

    if (nseg == 0)
        return 0;

    if (truncated)
    {
        fn->ram[OFS(FUJI_MB_BOOT_ERR)] = FUJI_BOOT_ERR_TRUNCATED;
        return -1;
    }

    /* Clamp-at-EOF: drop segments wholly past the file, trim ones that
     * straddle it. */
    {
        int kept = 0;
        for (int i = 0; i < nseg; i++)
        {
            if (seg_from[i] >= word_count)
                continue;
            if (seg_to[i] >= word_count)
                seg_to[i] = word_count - 1;
            seg_from[kept] = seg_from[i];
            seg_to[kept]   = seg_to[i];
            seg_rom[kept]  = seg_rom[i];
            kept++;
        }
        nseg = kept;
    }
    if (nseg == 0)
    {
        fn->ram[OFS(FUJI_MB_BOOT_ERR)] = FUJI_BOOT_ERR_CFGBAD;
        return -1;
    }

    /* RAM sanitation: nothing over console space, total within the
     * hardware budget. */
    {
        int kept = 0;
        for (int i = 0; i < nram; i++)
        {
            if (ram_to[i] < 0x4000)
                continue;
            if (ram_from[i] < 0x4000)
                ram_from[i] = 0x4000;
            total_ram += ram_to[i] - ram_from[i] + 1;
            ram_from[kept]  = ram_from[i];
            ram_to[kept]    = ram_to[i];
            ram_width[kept] = ram_width[i];
            kept++;
        }
        nram = kept;
    }
    if (total_ram > 0x2800)
    {
        fn->ram[OFS(FUJI_MB_BOOT_ERR)] = FUJI_BOOT_ERR_RAM;
        return -1;
    }

    for (int i = 0; i < nseg; i++)
        if (fujinet_mailbox_overlap(seg_rom[i], seg_to[i] - seg_from[i] + 1))
            *disable_mb = 1;
    for (int i = 0; i < nram; i++)
        if (fujinet_mailbox_overlap(ram_from[i], ram_to[i] - ram_from[i] + 1))
            *disable_mb = 1;

    for (int i = 0; i < nseg; i++)
        icartrom_addseg(rom, (uint16_t *)(words + seg_from[i]), seg_rom[i],
                        seg_to[i] - seg_from[i] + 1, ICARTROM_READ, 0);
    for (int i = 0; i < nram; i++)
        icartrom_addseg(rom, NULL, ram_from[i], ram_to[i] - ram_from[i] + 1,
                        (uint8_t)(ICARTROM_READ | ICARTROM_WRITE |
                                  (ram_width[i] == 8 ? ICARTROM_NARROW : 0)),
                        0);

    return nseg;
}

/* ======================================================================== */
/*  FUJINET_APPLY_ROM -- Decodes the buffered ROM push (fn->rom_buf) into   */
/*                       the live icart_t, same three-way format precedence */
/*                       inty_cart.c's apply_boot_mapping() uses: an       */
/*                       Intellicart/CC3 self-describing header first (we   */
/*                       reuse jzIntv's own icartrom_decode() rather than   */
/*                       hand-rolling it), then a .cfg sibling's "$from -   */
/*                       $to = $rom" lines, then a size-guess table for a   */
/*                       bare flat .bin. Returns nonzero on success, with   */
/*                       fn->icart->rom populated and ready for            */
/*                       icart_register()+periph_reset() by the caller.    */
/* ======================================================================== */
LOCAL int fujinet_apply_rom(fujinet_t *const fn)
{
    if (!fn->icart || !fn->cpu || !fn->bus)
        return 0;

    icart_t *const ic = (icart_t *)fn->icart;

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
    int disable_mb = 0;

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

            int cfg_rc = fn->have_cfg
                       ? fujinet_parse_cfg_mapping_into(scratch, fn, words,
                                                        word_count, &disable_mb)
                       : 0;
            if (cfg_rc > 0)
            {
                committed = 1;
            } else if (cfg_rc == 0)
            {
                /* Size-guess table, same as inty_cart.c's
                 * apply_boot_mapping() default branch. */
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
            /* cfg_rc < 0: the cfg was the mapping source but unusable --
             * BOOT_ERR is already set, don't guess. */
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
        fujinet_dbc_stream_open(fn, stream);
        fn->ram[OFS(FUJI_MB_BOOT_STATE)] = (stream == 1) ? FUJI_BOOT_OPENING
                                                          : FUJI_BOOT_XFER;
        if (stream == 0)
        {
            fn->ram[OFS(FUJI_MB_BOOT_PCT)] = 0;
            fn->ram[OFS(FUJI_MB_BOOT_ERR)] = 0;
        }
    }
    else if (req->command == NETCMD_WRITE)
    {
        fujinet_dbc_stream_write(fn, req->data, req->data_len);
        if (fn->bootdump_stream == 0)
        {
            /* Total size isn't known in advance, so this saturates near
             * 100% rather than reaching it exactly until CLOSE -- same
             * approximation the RP2040 side uses, good enough to see the
             * bar move. */
            uint32_t pct;
            fn->bootdump_rom_bytes += req->data_len;
            pct = (fn->bootdump_rom_bytes * 100) / 32768;
            fn->ram[OFS(FUJI_MB_BOOT_PCT)] = (uint8_t)(pct > 99 ? 99 : pct);
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
                /* Drop the outgoing cart's address decode bindings first.
                 * periph_register() only ever adds, so without this the
                 * config ROM's own map keeps answering next to the new
                 * game's -- and on the Intellivision's wire-AND bus the CPU
                 * gets the AND of the two. The config ROM declares 8-bit
                 * RAM over $8000-$9BFF, so every pushed game with a segment
                 * there (the 12K-at-$9000 shape most 32K/48K carts use)
                 * came back with its high byte silently masked off. */
                icart_unregister((icart_t *)fn->icart, fn->bus);
                icart_register((icart_t *)fn->icart, fn->bus,
                               (cp1600_t *)fn->cpu, fn->cache_flags);
                /* icart_register() just repointed the ROM address decoder
                 * (and re-marked ranges cacheable) -- it does NOT know
                 * about, let alone clear, any instructions the CP-1610
                 * already decoded and cached at those addresses from
                 * whatever occupied them before (the config ROM's boot
                 * menu, or a previously-loaded game). Left alone, the CPU
                 * would keep executing those stale decodes -- including a
                 * cached HLT from a spot that used to be unmapped -- right
                 * over the top of the freshly-pushed ROM's real bytes.
                 * Flush the whole decode cache so every fetch after reset
                 * re-decodes from the new memory contents. */
                cp1600_invalidate((cp1600_t *)fn->cpu, 0x0000, 0xFFFF);
                /* fujinet_apply_rom() just memset-zeroed and repopulated
                 * fn->icart's *live* rom.image[] in place -- the very same
                 * icart_t that's still registered on the bus and backing
                 * whatever the CPU is executing right now (the config ROM's
                 * own mailbox-polling loop, mid-"DO NOT POWER OFF"). Any
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

                fn->rxbuf[fn->rxlen++] = b;
                if (b == 0xC0 && ++fn->seen_end == 2)
                    break;
            }

            if (fn->seen_end < 2)
                break;   /*  Not a full frame yet.                         */

            if (!fujibus_parse_reply(fn->rxbuf, fn->rxlen, &reply))
            {
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
    CONDFREE(fn->rom_buf);
}

/* ======================================================================== */
/*  FUJINET_INIT      -- Sets up the FujiNet mailbox peripheral.            */
/* ======================================================================== */
int fujinet_init(fujinet_t *const fujinet, const char *const host,
                  const int port, const int debug,
                  const char *const bootdump_prefix,
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
