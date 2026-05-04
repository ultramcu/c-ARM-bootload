/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 MaIII Themd
 *
 * Self-test for the armbl engine. Builds and runs on the host.
 * No STM32 hardware required.
 *
 * The test stubs:
 *   - a 64 KB flash array (memory-mapped at 0x08000000),
 *   - a UART pair (lines from the library go into a tx queue;
 *     the fake "app" reads them and pushes its own lines back),
 *   - a tiny 32-byte test firmware that gets encoded into two
 *     16-byte S3 records plus an S7 terminator.
 *
 * Drives the engine through a full upgrade and asserts:
 *   - V? was sent, V 0 0 reply forced upgrade.
 *   - flash_erase covered [APP_START, APP_END].
 *   - every byte of the test firmware lands at the right address.
 *   - done sentinel == DONE_MAGIC.
 *   - jump_to_app(APP_START) was called exactly once.
 *
 * Then resets and runs three negative cases:
 *   - bad checksum  -> ERR csum
 *   - bad address   -> ERR addr
 *   - oversize line -> ERR len
 */

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>

#include "../armbl.h"

/* ------------------------------------------------------------ */
/* Memory map                                                   */
/* ------------------------------------------------------------ */

#define FLASH_BASE     0x08000000u
#define FLASH_SIZE     0x10000u
#define APP_START      0x08004000u
#define APP_END        0x0800FBFFu
#define VER_ADDR       0x0800FBF0u
#define DONE_ADDR      0x0800FBF4u
#define DONE_MAGIC     0xCAFEBABEu

static uint8_t  g_flash[FLASH_SIZE];

static int      g_erase_calls;
static uint32_t g_erase_start, g_erase_end;
static int      g_jumped;
static uint32_t g_jumped_addr;

static int fails = 0;

#define CHECK(expr) do { \
    if (!(expr)) { \
        printf("  ** FAIL: %s  (%s:%d)\n", #expr, __FILE__, __LINE__); \
        fails++; \
    } \
} while (0)

/* ------------------------------------------------------------ */
/* Flash mock                                                   */
/* ------------------------------------------------------------ */

static armbl_flash_result mock_erase(uint32_t s, uint32_t e)
{
    if (s < FLASH_BASE || e >= FLASH_BASE + FLASH_SIZE || s > e) {
        return ARMBL_FLASH_ERR;
    }
    memset(&g_flash[s - FLASH_BASE], 0xFF, e - s + 1);
    g_erase_calls++;
    g_erase_start = s;
    g_erase_end   = e;
    return ARMBL_FLASH_OK;
}

static uint32_t flash_word_at(uint32_t addr)
{
    uint32_t off = addr - FLASH_BASE;
    return  (uint32_t)g_flash[off]
         | ((uint32_t)g_flash[off + 1] <<  8)
         | ((uint32_t)g_flash[off + 2] << 16)
         | ((uint32_t)g_flash[off + 3] << 24);
}

static armbl_flash_result mock_write(uint32_t addr, uint32_t word)
{
    if (addr < FLASH_BASE
        || addr + 3 >= FLASH_BASE + FLASH_SIZE
        || (addr & 3)) {
        return ARMBL_FLASH_ERR;
    }
    /* Real flash only programs 1->0. Reject anything that would
     * require an erase in between. */
    uint32_t cur = flash_word_at(addr);
    if ((cur & word) != word) return ARMBL_FLASH_ERR;

    uint32_t off = addr - FLASH_BASE;
    g_flash[off    ] = (uint8_t)(word        & 0xFF);
    g_flash[off + 1] = (uint8_t)((word >>  8) & 0xFF);
    g_flash[off + 2] = (uint8_t)((word >> 16) & 0xFF);
    g_flash[off + 3] = (uint8_t)((word >> 24) & 0xFF);
    return ARMBL_FLASH_OK;
}

static uint32_t mock_read(uint32_t addr)
{
    if (addr < FLASH_BASE || addr + 3 >= FLASH_BASE + FLASH_SIZE) {
        return 0xFFFFFFFFu;
    }
    return flash_word_at(addr);
}

/* ------------------------------------------------------------ */
/* UART mock                                                    */
/* ------------------------------------------------------------ */

#define TX_LINES_MAX 128
#define TX_LINE_LEN  192

static char     g_tx_lines[TX_LINES_MAX][TX_LINE_LEN];
static size_t   g_tx_count;

static void clear_tx(void)
{
    g_tx_count = 0;
}

static void mock_write_line(const char *line, size_t len)
{
    if (g_tx_count >= TX_LINES_MAX) return;
    if (len >= TX_LINE_LEN) len = TX_LINE_LEN - 1;
    memcpy(g_tx_lines[g_tx_count], line, len);
    g_tx_lines[g_tx_count][len] = '\0';
    g_tx_count++;
}

static int tx_contains(const char *needle)
{
    for (size_t i = 0; i < g_tx_count; i++) {
        if (strcmp(g_tx_lines[i], needle) == 0) return 1;
    }
    return 0;
}

static int tx_starts_with(const char *needle)
{
    size_t nlen = strlen(needle);
    for (size_t i = 0; i < g_tx_count; i++) {
        if (strncmp(g_tx_lines[i], needle, nlen) == 0) return 1;
    }
    return 0;
}

static void mock_jump(uint32_t addr)
{
    g_jumped = 1;
    g_jumped_addr = addr;
}

/* ------------------------------------------------------------ */
/* Feed a whole NUL-terminated line (plus '\n') into the engine */
/* ------------------------------------------------------------ */

static void feed_line(armbl_ctx *ctx, const char *line)
{
    for (const char *p = line; *p; p++) armbl_feed_byte(ctx, (uint8_t)*p);
    armbl_feed_byte(ctx, (uint8_t)'\n');
}

/* ------------------------------------------------------------ */
/* S-record encoder for the fake app                            */
/* ------------------------------------------------------------ */

static size_t srec_data(uint32_t addr,
                        const uint8_t *data, size_t n,
                        char *out)
{
    /* count = n_data + 4 (addr) + 1 (checksum) */
    uint8_t  cnt = (uint8_t)(n + 5);
    uint32_t i   = 0;
    uint8_t  sum = cnt;

    i += (uint32_t)sprintf(out + i, "S3%02X%08X",
                           (unsigned)cnt, (unsigned)addr);
    sum += (uint8_t)((addr >> 24) & 0xFF);
    sum += (uint8_t)((addr >> 16) & 0xFF);
    sum += (uint8_t)((addr >>  8) & 0xFF);
    sum += (uint8_t)( addr        & 0xFF);
    for (size_t k = 0; k < n; k++) {
        i += (uint32_t)sprintf(out + i, "%02X", data[k]);
        sum += data[k];
    }
    i += (uint32_t)sprintf(out + i, "%02X", (uint8_t)~sum);
    return (size_t)i;
}

static size_t srec_terminator(uint32_t entry, char *out)
{
    uint8_t cnt = 5;                      /* 4 addr + 1 sum */
    uint8_t sum = cnt;
    sum += (uint8_t)((entry >> 24) & 0xFF);
    sum += (uint8_t)((entry >> 16) & 0xFF);
    sum += (uint8_t)((entry >>  8) & 0xFF);
    sum += (uint8_t)( entry        & 0xFF);
    return (size_t)sprintf(out, "S7%02X%08X%02X",
                           (unsigned)cnt, (unsigned)entry,
                           (uint8_t)~sum);
}

/* ------------------------------------------------------------ */
/* Drive the engine                                             */
/* ------------------------------------------------------------ */

static armbl_config make_config(void)
{
    armbl_config cfg;
    memset(&cfg, 0, sizeof(cfg));
    cfg.hooks.flash_erase = mock_erase;
    cfg.hooks.flash_write = mock_write;
    cfg.hooks.flash_read  = mock_read;
    cfg.hooks.write_line  = mock_write_line;
    cfg.hooks.jump_to_app = mock_jump;
    cfg.memmap.app_start_addr  = APP_START;
    cfg.memmap.app_end_addr    = APP_END;
    cfg.memmap.version_addr    = VER_ADDR;
    cfg.memmap.done_flag_addr  = DONE_ADDR;
    cfg.memmap.done_flag_value = DONE_MAGIC;
    cfg.version_timeout_ticks  = 5;
    cfg.version_retries        = 1;
    cfg.line_timeout_ticks     = 5;
    cfg.line_retries           = 1;
    return cfg;
}

static void reset_world(void)
{
    memset(g_flash, 0xFF, sizeof(g_flash));
    g_erase_calls = 0;
    g_erase_start = g_erase_end = 0;
    g_jumped      = 0;
    g_jumped_addr = 0;
    clear_tx();
}

/* tick_until pumps the engine until `pred(ctx)` returns true or
 * `max_ticks` runs out. Returns the number of ticks consumed. */
typedef int (*pred_fn)(const armbl_ctx *);

static int tick_until(armbl_ctx *ctx, pred_fn pred, int max_ticks)
{
    int t = 0;
    while (t < max_ticks) {
        if (pred(ctx)) return t;
        armbl_tick(ctx);
        t++;
    }
    return t;
}

static int saw_v_request(const armbl_ctx *ctx)
{
    (void)ctx;
    return tx_contains("V?");
}
static int saw_first_line_request(const armbl_ctx *ctx)
{
    (void)ctx;
    return tx_contains("L? 1");
}
static int saw_done_or_jumped(const armbl_ctx *ctx)
{
    (void)ctx;
    return tx_contains("DONE") || g_jumped;
}

/* ------------------------------------------------------------ */
/* Test: happy-path upgrade                                     */
/* ------------------------------------------------------------ */

static void test_happy_path(void)
{
    printf("-- happy-path upgrade --\n");
    reset_world();

    /* 32-byte test firmware. The first word is a fake initial-SP,
     * the second word is a fake reset vector, the rest is filler. */
    uint8_t fw[32] = {
        0x00, 0x04, 0x00, 0x20,
        0x81, 0x40, 0x00, 0x08,
        0xDE, 0xAD, 0xBE, 0xEF,
        0xCA, 0xFE, 0xBA, 0xBE,
        0x11, 0x22, 0x33, 0x44,
        0x55, 0x66, 0x77, 0x88,
        0x99, 0xAA, 0xBB, 0xCC,
        0x01, 0x02, 0x03, 0x04
    };

    char rec1[256], rec2[256], term[64];
    srec_data(APP_START,      &fw[0],  16, rec1);
    srec_data(APP_START + 16, &fw[16], 16, rec2);
    srec_terminator(APP_START, term);

    armbl_config cfg = make_config();
    armbl_ctx    ctx;
    CHECK(armbl_init(&ctx, &cfg) == 0);

    /* 1) Engine should send "V?" within a few ticks. */
    tick_until(&ctx, saw_v_request, 5);
    CHECK(tx_contains("V?"));

    /* 2) Reply "V 0 0" -> default policy chooses upgrade. */
    feed_line(&ctx, "V 0 0");
    clear_tx();

    /* 3) Engine erases and asks for line 1. */
    tick_until(&ctx, saw_first_line_request, 10);
    CHECK(g_erase_calls == 1);
    CHECK(g_erase_start == APP_START);
    CHECK(g_erase_end   == APP_END);
    CHECK(tx_contains("L? 1"));

    /* 4) Feed record 1, ack + L? 2. */
    clear_tx();
    feed_line(&ctx, rec1);
    armbl_tick(&ctx);                     /* parse */
    armbl_tick(&ctx);                     /* drain OK + send L? 2 */
    CHECK(tx_contains("OK"));
    CHECK(tx_contains("L? 2"));

    /* 5) Feed record 2, ack + L? 3. */
    clear_tx();
    feed_line(&ctx, rec2);
    armbl_tick(&ctx);
    armbl_tick(&ctx);
    CHECK(tx_contains("OK"));
    CHECK(tx_contains("L? 3"));

    /* 6) Feed terminator, expect DONE + jump. */
    clear_tx();
    feed_line(&ctx, term);
    tick_until(&ctx, saw_done_or_jumped, 10);
    /* one more tick to clear the BOOT state */
    armbl_tick(&ctx);

    CHECK(tx_contains("DONE"));
    CHECK(g_jumped == 1);
    CHECK(g_jumped_addr == APP_START);

    /* 7) Verify every byte of the firmware landed correctly. */
    int byte_mismatch = 0;
    for (size_t i = 0; i < sizeof(fw); i++) {
        if (g_flash[(APP_START + i) - FLASH_BASE] != fw[i]) {
            byte_mismatch++;
        }
    }
    CHECK(byte_mismatch == 0);

    /* 8) Done sentinel landed. */
    CHECK(mock_read(DONE_ADDR) == DONE_MAGIC);
}

/* ------------------------------------------------------------ */
/* Negative cases                                               */
/* ------------------------------------------------------------ */

static void prime_for_line_input(armbl_ctx *ctx)
{
    tick_until(ctx, saw_v_request, 5);
    feed_line(ctx, "V 0 0");
    tick_until(ctx, saw_first_line_request, 10);
    clear_tx();
}

static void test_bad_checksum(void)
{
    printf("-- bad checksum -> ERR csum --\n");
    reset_world();
    armbl_config cfg = make_config();
    armbl_ctx    ctx;
    armbl_init(&ctx, &cfg);
    prime_for_line_input(&ctx);

    uint8_t data[4] = { 0xAA, 0xBB, 0xCC, 0xDD };
    char rec[64];
    size_t n = srec_data(APP_START, data, 4, rec);
    rec[n - 1] ^= 0x01;     /* flip a bit in the checksum */

    feed_line(&ctx, rec);
    armbl_tick(&ctx);
    armbl_tick(&ctx);     /* drain pending err */
    CHECK(tx_starts_with("ERR csum"));
}

static void test_bad_address(void)
{
    printf("-- bad address -> ERR addr --\n");
    reset_world();
    armbl_config cfg = make_config();
    armbl_ctx    ctx;
    armbl_init(&ctx, &cfg);
    prime_for_line_input(&ctx);

    uint8_t data[4] = { 0xAA, 0xBB, 0xCC, 0xDD };
    char rec[64];
    /* Below the application region. */
    srec_data(0x08000000u, data, 4, rec);

    feed_line(&ctx, rec);
    armbl_tick(&ctx);
    armbl_tick(&ctx);
    CHECK(tx_starts_with("ERR addr"));
}

static void test_oversize_line(void)
{
    printf("-- oversize line -> ERR len --\n");
    reset_world();
    armbl_config cfg = make_config();
    armbl_ctx    ctx;
    armbl_init(&ctx, &cfg);
    prime_for_line_input(&ctx);

    /* Send ARMBL_LINE_MAX + 16 bytes of garbage, then '\n'. */
    for (int i = 0; i < ARMBL_LINE_MAX + 16; i++) {
        armbl_feed_byte(&ctx, (uint8_t)('X'));
    }
    armbl_feed_byte(&ctx, (uint8_t)'\n');
    armbl_tick(&ctx);
    CHECK(tx_starts_with("ERR len"));
}

/* ------------------------------------------------------------ */
/* main                                                          */
/* ------------------------------------------------------------ */

int main(void)
{
    test_happy_path();
    test_bad_checksum();
    test_bad_address();
    test_oversize_line();

    if (fails == 0) {
        printf("armbl: All OK.\n");
        return 0;
    }
    printf("armbl: %d FAILURE(S)\n", fails);
    return 1;
}
