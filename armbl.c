/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 MaIII Themd */

#include "armbl.h"

#include <string.h>

/* ------------------------------------------------------------ */
/* States                                                       */
/* ------------------------------------------------------------ */

enum {
    ST_INIT       = 0,
    ST_ASK_VER,
    ST_WAIT_VER,
    ST_DECIDE,
    ST_ERASE,
    ST_ASK_LINE,
    ST_WAIT_LINE,
    ST_MARK_DONE,
    ST_BOOT,
    ST_TIMEOUT
};

static const char *state_label(int s)
{
    switch (s) {
        case ST_INIT:      return "INIT";
        case ST_ASK_VER:   return "ASK_VER";
        case ST_WAIT_VER:  return "WAIT_VER";
        case ST_DECIDE:    return "DECIDE";
        case ST_ERASE:     return "ERASE";
        case ST_ASK_LINE:  return "ASK_LINE";
        case ST_WAIT_LINE: return "WAIT_LINE";
        case ST_MARK_DONE: return "MARK_DONE";
        case ST_BOOT:      return "BOOT";
        case ST_TIMEOUT:   return "TIMEOUT";
        default:           return "?";
    }
}

/* ------------------------------------------------------------ */
/* Helpers                                                      */
/* ------------------------------------------------------------ */

static void log_state(armbl_ctx *ctx, int next)
{
    if (ctx->cfg->hooks.log) {
        ctx->cfg->hooks.log(state_label(next));
    }
    ctx->state = next;
}

static void send_line(armbl_ctx *ctx, const char *line, size_t len)
{
    ctx->cfg->hooks.write_line(line, len);
}

static void send_str(armbl_ctx *ctx, const char *s)
{
    send_line(ctx, s, strlen(s));
}

/* Decimal print of an unsigned 16-bit value into `buf`. Returns
 * length written. */
static size_t u16_to_dec(uint16_t v, char *buf)
{
    char tmp[6];
    size_t n = 0;
    if (v == 0) { buf[0] = '0'; return 1; }
    while (v > 0) { tmp[n++] = (char)('0' + (v % 10)); v /= 10; }
    for (size_t i = 0; i < n; i++) buf[i] = tmp[n - 1 - i];
    return n;
}

static int hex_nibble(char c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    return -1;
}

static int hex_byte(char hi, char lo, uint8_t *out)
{
    int h = hex_nibble(hi), l = hex_nibble(lo);
    if (h < 0 || l < 0) return -1;
    *out = (uint8_t)((h << 4) | l);
    return 0;
}

/* Pack 1..4 little-endian bytes into a 32-bit word. Missing
 * positions are filled with 0xFF so the result, when written into
 * an erased flash cell, leaves untouched bits at one. */
static uint32_t le_word(const uint8_t *b, size_t n)
{
    uint8_t buf[4] = { 0xFF, 0xFF, 0xFF, 0xFF };
    for (size_t i = 0; i < n && i < 4; i++) buf[i] = b[i];
    return ((uint32_t)buf[0])
         | ((uint32_t)buf[1] << 8)
         | ((uint32_t)buf[2] << 16)
         | ((uint32_t)buf[3] << 24);
}

/* ------------------------------------------------------------ */
/* S-record parser                                              */
/* ------------------------------------------------------------ */

/* parse_s3 walks one S3 record stored in ctx->rx_buf and programs
 * its data bytes into flash. On any error returns the short reason
 * token and the engine sends ERR <reason>. The terminator records
 * S7 / S8 / S9 are handled by the caller. */
static const char *parse_s3(armbl_ctx *ctx)
{
    const uint8_t *line = ctx->rx_buf;
    uint16_t       len  = ctx->rx_len;

    /* Strip trailing '\n' if still present. */
    while (len > 0 && (line[len - 1] == '\n' || line[len - 1] == '\r')) {
        len--;
    }

    if (len < 2 + 2 + 8 + 2) return "len";  /* S3 + count + addr + sum */
    if (line[0] != 'S' || line[1] != '3') return "addr"; /* not data */

    uint8_t count;
    if (hex_byte((char)line[2], (char)line[3], &count) < 0) return "hex";

    /* The line on the wire after "S3CC" has 2 hex chars per byte
     * counted in `count`. So expected printable length =
     * 4 (header) + 2 * count. */
    if ((uint16_t)(4 + 2 * count) != len) return "len";
    if (count < 5) return "len";  /* must contain 4-byte addr + sum */

    /* Decode the whole payload into a small scratch buffer. */
    uint8_t payload[ARMBL_LINE_MAX / 2];
    if (count > sizeof(payload)) return "len";
    for (uint8_t i = 0; i < count; i++) {
        if (hex_byte((char)line[4 + 2 * i],
                     (char)line[5 + 2 * i],
                     &payload[i]) < 0) return "hex";
    }

    /* Checksum = ones-complement of (count + sum(payload[0..n-2])). */
    uint8_t sum = count;
    for (uint8_t i = 0; i < (uint8_t)(count - 1); i++) sum += payload[i];
    if ((uint8_t)(~sum) != payload[count - 1]) return "csum";

    uint32_t addr = ((uint32_t)payload[0] << 24)
                  | ((uint32_t)payload[1] << 16)
                  | ((uint32_t)payload[2] <<  8)
                  | ((uint32_t)payload[3]);

    uint8_t  data_n = (uint8_t)(count - 5);   /* data bytes only */
    const uint8_t *data = &payload[4];

    const armbl_memmap *m = &ctx->cfg->memmap;
    if (addr < m->app_start_addr) return "addr";
    if (addr + data_n - 1 > m->app_end_addr) return "addr";

    /* Program word-aligned chunks. The S-record always lays out
     * bytes in increasing address order; we walk it the same way.
     * If `addr` is not 4-aligned the partial word read-modify-write
     * would clobber neighbouring data, so just reject -- objcopy's
     * srec output is always aligned for our purposes. */
    if (addr & 0x3) return "addr";

    for (uint8_t off = 0; off < data_n; ) {
        uint8_t take = (uint8_t)(data_n - off);
        if (take > 4) take = 4;

        uint32_t word = le_word(&data[off], take);
        armbl_flash_result r =
            ctx->cfg->hooks.flash_write(addr + off, word);
        if (r != ARMBL_FLASH_OK) return "flash";

        if (ctx->cfg->hooks.flash_read(addr + off) != word) return "flash";

        off = (uint8_t)(off + take);
    }

    return NULL;
}

/* check_terminator returns 1 if rx_buf holds a valid S7/S8/S9
 * record. Checksum is verified; address is ignored (the original
 * tools sometimes set it to the entry point, sometimes to zero). */
static int check_terminator(armbl_ctx *ctx)
{
    const uint8_t *line = ctx->rx_buf;
    uint16_t       len  = ctx->rx_len;

    while (len > 0 && (line[len - 1] == '\n' || line[len - 1] == '\r')) len--;
    if (len < 4) return 0;
    if (line[0] != 'S') return 0;
    if (line[1] != '7' && line[1] != '8' && line[1] != '9') return 0;

    uint8_t count;
    if (hex_byte((char)line[2], (char)line[3], &count) < 0) return 0;
    if ((uint16_t)(4 + 2 * count) != len) return 0;

    uint8_t sum = count;
    for (uint8_t i = 0; i < (uint8_t)(count - 1); i++) {
        uint8_t b;
        if (hex_byte((char)line[4 + 2 * i],
                     (char)line[5 + 2 * i], &b) < 0) return 0;
        sum += b;
    }
    uint8_t given;
    if (hex_byte((char)line[4 + 2 * (count - 1)],
                 (char)line[5 + 2 * (count - 1)], &given) < 0) return 0;
    return ((uint8_t)~sum) == given;
}

/* ------------------------------------------------------------ */
/* Version-line parsing                                         */
/* ------------------------------------------------------------ */

/* parse_version_line reads "V <major> <minor>" out of rx_buf and
 * stores the result in ctx. Returns 0 on success, -1 on failure. */
static int parse_version_line(armbl_ctx *ctx)
{
    const uint8_t *p = ctx->rx_buf;
    uint16_t       n = ctx->rx_len;
    if (n < 5) return -1;
    if (p[0] != 'V' || p[1] != ' ') return -1;

    uint16_t i = 2;
    uint32_t maj = 0;
    int      saw = 0;
    while (i < n && p[i] >= '0' && p[i] <= '9') {
        maj = maj * 10 + (uint32_t)(p[i] - '0');
        if (maj > 255) return -1;
        i++; saw = 1;
    }
    if (!saw || i >= n || p[i] != ' ') return -1;
    i++;

    uint32_t min = 0;
    saw = 0;
    while (i < n && p[i] >= '0' && p[i] <= '9') {
        min = min * 10 + (uint32_t)(p[i] - '0');
        if (min > 255) return -1;
        i++; saw = 1;
    }
    if (!saw) return -1;

    ctx->app_major = (uint8_t)maj;
    ctx->app_minor = (uint8_t)min;
    return 0;
}

/* ------------------------------------------------------------ */
/* Default decide policy                                        */
/* ------------------------------------------------------------ */

static armbl_decision default_decide(int got_version,
                                       uint8_t app_major,
                                       uint8_t app_minor,
                                       uint32_t flash_version,
                                       uint32_t flash_done)
{
    (void)app_minor;

    /* No reply from app -- assume there's nothing usable up there. */
    if (!got_version) {
        return (flash_done == 0) ? ARMBL_DECIDE_UPGRADE
                                 : ARMBL_DECIDE_BOOT;
    }

    /* App responded with a sentinel "0.0" -- explicit upgrade ask. */
    if (app_major == 0 && app_minor == 0) return ARMBL_DECIDE_UPGRADE;

    /* Otherwise upgrade only if the major in flash is stale. */
    uint8_t flash_major = (uint8_t)(flash_version & 0xFF);
    if (flash_major != app_major) return ARMBL_DECIDE_UPGRADE;
    return ARMBL_DECIDE_BOOT;
}

/* ------------------------------------------------------------ */
/* Public API                                                   */
/* ------------------------------------------------------------ */

int armbl_init(armbl_ctx *ctx, const armbl_config *cfg)
{
    if (!ctx || !cfg) return -1;
    if (!cfg->hooks.flash_erase || !cfg->hooks.flash_write
        || !cfg->hooks.flash_read || !cfg->hooks.write_line
        || !cfg->hooks.jump_to_app) return -1;
    if (cfg->memmap.app_start_addr >= cfg->memmap.app_end_addr) return -1;

    memset(ctx, 0, sizeof(*ctx));
    ctx->cfg   = cfg;
    ctx->state = ST_INIT;
    return 0;
}

void armbl_feed_byte(armbl_ctx *ctx, uint8_t b)
{
    if (b == '\r') return;       /* tolerate CRLF transports */
    if (b == '\n') {
        if (ctx->rx_overflow) {
            ctx->rx_len      = 0;
            ctx->rx_overflow = 0;
            ctx->pending_err = 1;
            ctx->pending_err_reason = "len";
            return;
        }
        if (ctx->rx_len == 0) return;   /* blank line */
        ctx->rx_line_ready = 1;
        return;
    }

    if (ctx->rx_line_ready) return;     /* drop until tick consumes it */

    if (ctx->rx_len >= ARMBL_LINE_MAX) {
        ctx->rx_overflow = 1;
        return;
    }
    ctx->rx_buf[ctx->rx_len++] = b;
}

static void clear_rx(armbl_ctx *ctx)
{
    ctx->rx_len        = 0;
    ctx->rx_line_ready = 0;
    ctx->rx_overflow   = 0;
}

void armbl_tick(armbl_ctx *ctx)
{
    const armbl_config *cfg = ctx->cfg;

    /* Drain a pending OK / ERR before doing anything else, so the
     * app gets a synchronous answer to its previous record. */
    if (ctx->pending_ack) {
        send_str(ctx, "OK");
        ctx->pending_ack = 0;
    }
    if (ctx->pending_err) {
        char tmp[ARMBL_LINE_MAX];
        size_t n = 0;
        memcpy(tmp, "ERR ", 4); n = 4;
        const char *r = ctx->pending_err_reason ? ctx->pending_err_reason : "?";
        for (; n < sizeof(tmp) - 1 && *r; n++, r++) tmp[n] = *r;
        send_line(ctx, tmp, n);
        ctx->pending_err = 0;
        ctx->pending_err_reason = NULL;
    }

    switch (ctx->state) {

    case ST_INIT:
        log_state(ctx, ST_ASK_VER);
        ctx->retry_left = cfg->version_retries
                        ? cfg->version_retries : 3;
        break;

    case ST_ASK_VER:
        send_str(ctx, "V?");
        ctx->got_version = 0;
        ctx->timer_ticks = cfg->version_timeout_ticks
                         ? cfg->version_timeout_ticks : 100;
        clear_rx(ctx);
        log_state(ctx, ST_WAIT_VER);
        break;

    case ST_WAIT_VER:
        if (ctx->rx_line_ready) {
            if (parse_version_line(ctx) == 0) {
                ctx->got_version = 1;
                clear_rx(ctx);
                log_state(ctx, ST_DECIDE);
                break;
            }
            clear_rx(ctx);  /* unrelated line, keep waiting */
        }
        if (ctx->timer_ticks == 0) {
            if (ctx->retry_left > 0) {
                ctx->retry_left--;
                log_state(ctx, ST_ASK_VER);
            } else {
                log_state(ctx, ST_DECIDE);
            }
        } else {
            ctx->timer_ticks--;
        }
        break;

    case ST_DECIDE: {
        uint32_t fv = cfg->hooks.flash_read(cfg->memmap.version_addr);
        uint32_t fd = cfg->hooks.flash_read(cfg->memmap.done_flag_addr);
        armbl_decision d = cfg->hooks.decide
            ? cfg->hooks.decide(ctx->got_version,
                                ctx->app_major, ctx->app_minor,
                                fv, fd)
            : default_decide(ctx->got_version,
                             ctx->app_major, ctx->app_minor,
                             fv, fd);

        if (d == ARMBL_DECIDE_BOOT) {
            log_state(ctx, ST_BOOT);
        } else {
            log_state(ctx, ST_ERASE);
        }
        break;
    }

    case ST_ERASE: {
        armbl_flash_result r = cfg->hooks.flash_erase(
            cfg->memmap.app_start_addr, cfg->memmap.app_end_addr);
        if (r != ARMBL_FLASH_OK) {
            log_state(ctx, ST_TIMEOUT);
            break;
        }
        ctx->requested_line = 1;
        ctx->retry_left     = cfg->line_retries
                            ? cfg->line_retries : 3;
        ctx->saw_terminator = 0;
        log_state(ctx, ST_ASK_LINE);
        break;
    }

    case ST_ASK_LINE: {
        char buf[16];
        size_t n = 0;
        memcpy(buf, "L? ", 3); n = 3;
        n += u16_to_dec((uint16_t)ctx->requested_line, buf + n);
        send_line(ctx, buf, n);
        ctx->timer_ticks = cfg->line_timeout_ticks
                         ? cfg->line_timeout_ticks : 500;
        clear_rx(ctx);
        log_state(ctx, ST_WAIT_LINE);
        break;
    }

    case ST_WAIT_LINE:
        if (ctx->rx_line_ready) {
            if (check_terminator(ctx)) {
                ctx->saw_terminator = 1;
                clear_rx(ctx);
                log_state(ctx, ST_MARK_DONE);
                break;
            }
            const char *err = parse_s3(ctx);
            clear_rx(ctx);
            if (err) {
                if (ctx->retry_left > 0) {
                    ctx->retry_left--;
                    ctx->pending_err = 1;
                    ctx->pending_err_reason = err;
                    log_state(ctx, ST_ASK_LINE);
                } else {
                    log_state(ctx, ST_TIMEOUT);
                }
                break;
            }
            ctx->pending_ack    = 1;
            ctx->retry_left     = cfg->line_retries
                                ? cfg->line_retries : 3;
            ctx->requested_line++;
            log_state(ctx, ST_ASK_LINE);
            break;
        }
        if (ctx->timer_ticks == 0) {
            if (ctx->retry_left > 0) {
                ctx->retry_left--;
                log_state(ctx, ST_ASK_LINE);
            } else {
                log_state(ctx, ST_TIMEOUT);
            }
        } else {
            ctx->timer_ticks--;
        }
        break;

    case ST_MARK_DONE:
        if (cfg->hooks.flash_write(cfg->memmap.done_flag_addr,
                                   cfg->memmap.done_flag_value)
            != ARMBL_FLASH_OK) {
            log_state(ctx, ST_TIMEOUT);
            break;
        }
        send_str(ctx, "DONE");
        log_state(ctx, ST_BOOT);
        break;

    case ST_BOOT:
        cfg->hooks.jump_to_app(cfg->memmap.app_start_addr);
        /* If the hook returns (it shouldn't), park here. */
        break;

    case ST_TIMEOUT:
        /* Terminal. Caller's watchdog will reset the device. */
        break;

    default:
        ctx->state = ST_TIMEOUT;
        break;
    }
}
