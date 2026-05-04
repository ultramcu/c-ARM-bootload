# c-ARM-bootload

A small, portable ARM bootloader engine. One state machine that
asks the application for its current firmware version, decides
whether to upgrade, erases the application region, pulls the new
firmware over a text protocol carrying Motorola S-records, programs
flash, writes a "done" sentinel, and jumps to the application.

The library itself is **pure C99**. It does not include or call
anything vendor-specific — every hardware interaction (flash
erase / program / read, UART line write, jump-to-app) goes through
caller-supplied callbacks. The same source compiles cleanly on
STM32 (F1 / F4 / L4), GD32, AT32, NXP LPC, and any other Cortex-M
part with memory-mapped flash. It also compiles on the host for
the included self-test, with no MCU required.

```c
armbl_ctx    ctx;
armbl_config cfg = {
    .hooks  = { my_erase, my_write, my_read, my_uart_send_line, my_jump,
                /* .decide = */ NULL, /* .log = */ NULL },
    .memmap = { .app_start_addr  = 0x08004000,
                .app_end_addr    = 0x0800FBFF,
                .version_addr    = 0x0800FBF0,
                .done_flag_addr  = 0x0800FBF4,
                .done_flag_value = 0xCAFEBABE },
    .version_timeout_ticks = 100,    /* 1 s @ 10 ms tick */
    .version_retries       = 3,
    .line_timeout_ticks    = 500,    /* 5 s */
    .line_retries          = 3,
};
armbl_init(&ctx, &cfg);

/* In your UART RX ISR: */
armbl_feed_byte(&ctx, received_byte);

/* In a 10 ms periodic tick (SysTick / TIM): */
armbl_tick(&ctx);
```

## Wire protocol

Line-based ASCII, terminated by a single `\n`. `\r` is silently
ignored on input. All numbers are decimal unless they appear
inside an S-record body (which uses the standard hex encoding).

| Direction | Line | Meaning |
|---|---|---|
| BL → app | `V?` | Request the application's firmware version. |
| BL → app | `L? <n>` | Request line `<n>` of the firmware (1-based). |
| BL → app | `OK` | The previous S-record was programmed. |
| BL → app | `ERR <reason>` | The previous record failed. Reason is one of `csum`, `addr`, `hex`, `flash`, `len`. |
| BL → app | `DONE` | The whole image was committed and the done-sentinel is set. The bootloader will jump immediately after. |
| app → BL | `V <major> <minor>` | Version reply, e.g. `V 1 2`. Sentinel `V 0 0` forces an upgrade under the default policy. |
| app → BL | `S<...><CC>` | One Motorola S-record. `S3` (4-byte address data) is the only data form. `S7` / `S8` / `S9` are accepted as terminators. |

The S-record body is whatever your toolchain emits. The standard
GNU one-liner is:

```sh
arm-none-eabi-objcopy -O srec --srec-forceS3 firmware.elf firmware.s19
```

Each line of `firmware.s19` becomes one frame on the wire. The
host (or the running application acting as a relay) reads
`firmware.s19` and emits one record per `L? n` request. No
proprietary framing, no checksum on top of the S-record's own.

## State machine

```
INIT ─▶ ASK_VER ─▶ WAIT_VER ─▶ DECIDE ─┬─▶ BOOT
                                       │
                                       ╰─▶ ERASE ─▶ ASK_LINE ◀─╮
                                                       │       │
                                                  WAIT_LINE ───╯
                                                       │
                                                       ▼
                                                   MARK_DONE ─▶ BOOT

(any unrecoverable error) ─▶ TIMEOUT (terminal; caller's watchdog
                                       resets the device)
```

`DECIDE` invokes the caller-supplied `decide()` hook to choose
between BOOT and UPGRADE. If the hook is `NULL`, a built-in policy
runs:

- No version reply, no done-sentinel  → upgrade
- No version reply, done-sentinel set → boot
- App reports `V 0 0`                 → upgrade (explicit ask)
- App major != flash version major    → upgrade
- Otherwise                           → boot

Override it for anything more nuanced (e.g. enforce monotonic
versions, integrate a hardware "force-upgrade" jumper, etc.).

## Memory map

The library does not assume any specific layout — you tell it
via `armbl_memmap`. The fields are:

| Field | Meaning |
|---|---|
| `app_start_addr` | First byte of the erasable region. Must be page-aligned for the target. The vector table of the new image lives here. |
| `app_end_addr` | Last byte of the erasable region (inclusive). |
| `version_addr` | 4-byte slot inside the application image holding the version word. The lowest byte is treated as `major` by the default policy. |
| `done_flag_addr` | 4-byte slot. The bootloader writes `done_flag_value` here on a successful upgrade. |
| `done_flag_value` | Magic value, e.g. `0xCAFEBABE`. The default policy treats any other value at `done_flag_addr` as "no valid image". |

For an STM32F103 with a 16 KB bootloader and 48 KB application:

```
0x0800_0000  ┐
             │   bootloader (.text, .rodata)
0x0800_3FFF  ┘
0x0800_4000  ┐ <- app_start_addr
             │   application image
0x0800_FBEF  ┘
0x0800_FBF0      version  (4 B)        <- version_addr
0x0800_FBF4      done flag (4 B)       <- done_flag_addr
0x0800_FBF8      reserved
0x0800_FBFF                            <- app_end_addr
```

The exact addresses don't matter to the library — pick whatever
suits the part. Cortex-M parts other than STM32 (NXP LPC, GD32,
AT32, …) use a different flash base or page size; just plug the
right numbers into `armbl_memmap` and the engine is happy.

## Integration sketch

Five small hooks in your application code do all the
vendor-specific work. The library calls them; it never touches a
register directly.

| Hook | Generic responsibility |
|---|---|
| `flash_erase(s, e)` | Erase every page in the closed range `[s, e]`. On STM32 with ST-StdPeriph: `FLASH_Unlock` once, then `FLASH_ErasePage` in a loop. On other Cortex-M parts the equivalent IAP / FMC call. |
| `flash_write(addr, word)` | Program one 32-bit word. STM32: `FLASH_ProgramWord(addr, word)`. |
| `flash_read(addr)` | Read one 32-bit word. On any part with memory-mapped flash, `return *(volatile uint32_t *)addr;` is enough. |
| `write_line(line, len)` | Push the bytes of `line` to the chosen UART, then send a single `\n`. |
| `jump_to_app(addr)` | Disable interrupts, point the vector table at `addr`, load the new SP from `*(uint32_t *)addr`, jump to `*(uint32_t *)(addr + 4)`. On Cortex-M the typical sequence is to write `SCB->VTOR`, `__set_MSP(...)`, then call the reset-vector function pointer. The library guarantees this is the last call it makes. |

Bring-up:

- In the UART RX ISR: `armbl_feed_byte(&ctx, byte)`.
- In a 10 ms timer ISR (or in the main loop with a millis check):
  `armbl_tick(&ctx)`.

App side: the running application listens for the same text
protocol. On `V?` it replies `V <maj> <min>`. On `L? <n>` it
streams the n'th S-record from a firmware file it received over
its own transport (Bluetooth, Wi-Fi, USB MSC, etc.).

## Build the host self-test

```sh
make test
```

Builds `example/host_check.c` against the library, runs it,
prints `armbl: All OK.` on success and exits zero. The check
covers:

- A full upgrade lifecycle on a 64 KB simulated flash with a
  32-byte test image (two S3 records + S7 terminator). Verifies
  that erase ran over `[app_start, app_end]`, that every byte of
  the image landed at the right address, that the done sentinel
  is set, and that `jump_to_app` was called exactly once.
- Bad checksum  → `ERR csum`.
- Address below `app_start_addr` → `ERR addr`.
- Line longer than `ARMBL_LINE_MAX` → `ERR len`.

The simulated flash enforces "ones can only become zeros" so the
test also catches any code path that tries to program over an
un-erased word.

## Use in another project

Copy `armbl.h` and `armbl.c` into your firmware tree. No other
dependencies. The library does not include any vendor headers
(`stm32fxxx.h`, `core_cm3.h`, `cmsis_*.h`, `lpc_*.h`, …); bring
those in only inside your hook implementations.

## License

[MIT](LICENSE) — `SPDX-License-Identifier: MIT`. Drop into any
project, open or closed, commercial or otherwise.
