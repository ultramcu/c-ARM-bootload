/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 MaIII Themd
 *
 * armbl.h -- portable ARM bootloader engine.
 *
 * One state machine that asks the application for its current
 * firmware version, decides whether to upgrade, erases the
 * application region, pulls the new firmware line by line as
 * Motorola S-records over a text protocol, programs flash, writes
 * a "done" sentinel, and jumps to the application.
 *
 * The library is pure C99. It does not include or call anything
 * vendor-specific -- every hardware interaction (flash erase /
 * program / read, UART line write, jump-to-app) goes through
 * caller-supplied callbacks. The same library runs on STM32 (F1
 * / F4 / L4), GD32, AT32, NXP LPC and any other Cortex-M part
 * with memory-mapped flash, plus on a host for testing.
 *
 * Wire protocol (line-based ASCII, '\n' terminator):
 *
 *     bootloader -> app                    app -> bootloader
 *     -----------------                    -----------------
 *     V?                                   V <major> <minor>
 *     L? <n>                               S<record>...<CC>
 *     OK                                   (S3 data, S7/S8/S9 terminator)
 *     ERR <reason>
 *     DONE
 *
 * `<reason>` is a short ASCII token: csum, addr, hex, flash, len.
 *
 * Lifecycle:
 *
 *     reset -> ASK_VER -> WAIT_VER -> DECIDE -> {ERASE -> ASK_LINE
 *               <-> WAIT_LINE -> MARK_DONE -> BOOT} | BOOT | TIMEOUT
 */

#ifndef ARMBL_H
#define ARMBL_H

#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Maximum length of one received line, including the trailing '\n'.
 * The default fits one S3 record with up to ~58 data bytes (which
 * is more than arm-none-eabi-objcopy -O srec emits by default).
 * Override at compile time if your toolchain emits longer records. */
#ifndef ARMBL_LINE_MAX
#define ARMBL_LINE_MAX 128
#endif

/* Result codes returned by the flash callbacks. Anything other
 * than _OK aborts the upgrade and the engine drops to TIMEOUT. */
typedef enum {
    ARMBL_FLASH_OK  = 0,
    ARMBL_FLASH_ERR = 1
} armbl_flash_result;

/* Decision returned by the policy hook after the version reply
 * is in (or has timed out). */
typedef enum {
    ARMBL_DECIDE_BOOT    = 0,
    ARMBL_DECIDE_UPGRADE = 1
} armbl_decision;

/* Caller-supplied callbacks. Only `flash_erase`, `flash_write`,
 * `flash_read`, `write_line` and `jump_to_app` are required. */
typedef struct {
    /* Erase every page in the closed range [start, end]. The
     * caller must round to the chip's page granularity. */
    armbl_flash_result (*flash_erase)(uint32_t start_addr,
                                        uint32_t end_addr);

    /* Program a single 32-bit word at `addr` (which must be
     * 4-byte aligned). */
    armbl_flash_result (*flash_write)(uint32_t addr,
                                        uint32_t word);

    /* Read a 32-bit word from flash. On real STM32 a bare
     *     return *(volatile uint32_t *)addr;
     * is enough; the hook exists for the host simulator. */
    uint32_t (*flash_read)(uint32_t addr);

    /* Send one full line (no '\n' included) to the application
     * over the active transport. The library appends the '\n'
     * itself. `len` is the byte count of `line`, excluding any
     * terminator. */
    void (*write_line)(const char *line, size_t len);

    /* Hand control to the application image. Must NOT return.
     * The caller is responsible for SP/PC reset, vector-table
     * relocation and any other pre-jump cleanup. */
    void (*jump_to_app)(uint32_t app_start_addr);

    /* Optional. Called once after WAIT_VER to choose between
     * boot and upgrade. If NULL the library uses a built-in
     * default: upgrade if no version was received OR if the
     * received major != flash[version_addr] >> 0 & 0xFF;
     * otherwise boot. The hook receives:
     *   got_version    : 1 if a "V x y" line was parsed, else 0
     *   app_major/minor: parsed values (zero if !got_version)
     *   flash_version  : flash_read(memmap.version_addr)
     *   flash_done     : flash_read(memmap.done_flag_addr)            */
    armbl_decision (*decide)(int got_version,
                               uint8_t app_major,
                               uint8_t app_minor,
                               uint32_t flash_version,
                               uint32_t flash_done);

    /* Optional. Called with a short label on every state
     * transition. NULL is fine; the library never logs by default. */
    void (*log)(const char *label);
} armbl_hooks;

/* Memory map describing where the application image lives and
 * where the version + done-sentinel words are stored. All values
 * are absolute flash addresses on the target. */
typedef struct {
    uint32_t app_start_addr;     /* first byte of erasable region */
    uint32_t app_end_addr;       /* last byte (inclusive) */
    uint32_t version_addr;       /* 4 bytes: app version word */
    uint32_t done_flag_addr;     /* 4 bytes: sentinel slot */
    uint32_t done_flag_value;    /* magic, e.g. 0xCAFEBABE */
} armbl_memmap;

/* Engine configuration: hooks + memmap + timing. */
typedef struct {
    armbl_hooks  hooks;
    armbl_memmap memmap;

    /* Timeouts are in caller-defined ticks. The library does not
     * know real time -- the caller picks the tick rate when it
     * calls armbl_tick() (10 ms is typical on STM32). */
    uint16_t version_timeout_ticks;   /* default 100  -> 1 s @ 10 ms */
    uint16_t version_retries;         /* default 3 */
    uint16_t line_timeout_ticks;      /* default 500  -> 5 s @ 10 ms */
    uint16_t line_retries;            /* default 3 */
} armbl_config;

/* Engine context. Treat as opaque. Storage is provided by the
 * caller; the library does no heap allocation. */
typedef struct {
    const armbl_config *cfg;

    int      state;
    uint16_t timer_ticks;
    uint16_t retry_left;
    uint32_t requested_line;     /* 1-based */

    uint8_t  app_major;
    uint8_t  app_minor;
    uint8_t  got_version;

    uint8_t  rx_buf[ARMBL_LINE_MAX];
    uint16_t rx_len;
    uint8_t  rx_overflow;
    uint8_t  rx_line_ready;

    uint8_t  saw_terminator;
    uint8_t  pending_ack;        /* 1: send OK on next tick */
    uint8_t  pending_err;        /* 1: send ERR on next tick */
    const char *pending_err_reason;
} armbl_ctx;

/* Initialise the engine. Returns 0 on success, -1 if the config
 * is malformed (missing required hook, app_start >= app_end,
 * etc.). After this call the engine sits in INIT until the first
 * armbl_tick(). */
int armbl_init(armbl_ctx *ctx, const armbl_config *cfg);

/* Feed one byte received from the application UART (or whatever
 * transport you're using). The library accumulates bytes until a
 * '\n' arrives. '\r' is silently stripped. Lines longer than
 * ARMBL_LINE_MAX are dropped with an "ERR len" reply. Safe to
 * call from an ISR. */
void armbl_feed_byte(armbl_ctx *ctx, uint8_t b);

/* Advance the state machine by one tick. Call at a fixed cadence
 * (10 ms is recommended). The library performs all UART writes
 * and synchronous flash operations during this call. */
void armbl_tick(armbl_ctx *ctx);

#ifdef __cplusplus
}
#endif

#endif /* ARMBL_H */
