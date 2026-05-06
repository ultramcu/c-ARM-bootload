# STM32F103 worked example

This directory wires the `c-ARM-bootload` library (`armbl_*` API)
into a real STM32F103 medium-density part (64 KB flash, e.g. an
STM32F103C8) using the ST StdPeriph driver library. It is not a
complete project — there is no linker script, startup file or
Makefile — only the four C files that make up the bootloader's
own translation units. Drop them into your existing IAR / Keil /
STM32CubeIDE / arm-none-eabi-gcc project that already has
StdPeriph and CMSIS set up and you have a working bootloader.

## Files in this directory

| File | What it does |
|---|---|
| `main.c` | Bootloader entry point. Brings up the clock tree, GPIO, USART1, TIM3, IWDG and NVIC, then runs the main loop that pets the watchdog and ticks the engine every 10 ms. Owns the two ISRs (USART1 RX, TIM3 update). |
| `hooks.c` | The five `armbl_hooks` callbacks: flash erase / write / read, `write_line`, and `jump_to_app`. All StdPeriph FLASH and USART code lives here. |
| `hooks.h` | Declarations of the five hooks. Pulled in by `main.c` so `g_cfg` can refer to them by name. |
| `armbl_app_layout.h` | The STM32-specific knobs grouped in one header: flash addresses, page size, baud rate, done-flag magic. Edit this header (and only this header) when retargeting to a different density part. |

The library itself (`armbl.h` / `armbl.c`) sits two directories
up. Includes use the relative path `"../../armbl.h"`.

## Reference hardware

| Property | Value |
|---|---|
| MCU | STM32F103C8 / CB / RB (medium density, 64 KB flash) |
| Flash page size | 1 KiB |
| SYSCLK | 72 MHz (HSE 8 MHz × PLL 9, the StdPeriph default) |
| Bootloader UART | USART1 on PA9 (TX) / PA10 (RX), 57 600 8N1 |
| Tick source | TIM3, 10 ms update IRQ |
| Watchdog | IWDG, 3.0 s timeout |

If your board uses a different USART, baud, or oscillator, edit
the relevant init function in `main.c` and adjust the constants
in `armbl_app_layout.h`. None of the library files change.

## Flash partitioning

The example uses a 16 KiB bootloader at the bottom of flash and
the rest for the application image, with two reserved 4-byte
slots near the very end for the version word and the
upgrade-done sentinel.

```
0x0800_0000  ┐
             │   bootloader text + rodata + vectors        16 KiB
0x0800_3FFF  ┘
0x0800_4000  ┐ <- APP_START_ADDR
             │   application image                         ≈47 KiB
0x0800_FBEF  ┘
0x0800_FBF0      version word (4 B)        <- VERSION_ADDR
0x0800_FBF4      done sentinel (4 B)       <- DONE_FLAG_ADDR
0x0800_FBF8      reserved
0x0800_FBFF                                <- APP_END_ADDR
```

The application's linker script must:

- Place its vector table at `APP_START_ADDR` (or the equivalent
  `_estack`/`_isr_vector` setup for your toolchain).
- Run `SCB->VTOR = APP_START_ADDR` early in `Reset_Handler`, so
  IRQs fire through the application's table.
- Reserve the two trailing words at `VERSION_ADDR` and
  `DONE_FLAG_ADDR` so the linker doesn't pack code over them.

The bootloader's own linker script puts its vector table at
`0x08000000` and limits its `.text` to `< 0x08004000`. Standard
StdPeriph project templates need only the start-address and
total-size tweak to fit.

## The five hooks

The whole point of the library is that nothing inside it knows
about STM32. The example hooks translate the abstract operations
into StdPeriph calls.

### 1. `flash_erase(start, end)`

```c
armbl_flash_result hooks_flash_erase(uint32_t start, uint32_t end);
```

Walks `[start, end]` inclusive in `FLASH_PAGE_SIZE`-aligned
strides, calling `FLASH_ErasePage(addr)` for each page.

- `FLASH_Unlock()` once at entry. **The success path leaves
  flash unlocked** — the engine immediately follows up with a
  long stream of `flash_write` calls and unlock / lock per word
  would just burn cycles. The error path does call `FLASH_Lock`
  before returning so we don't leave the controller open if the
  upgrade is going to abort.
- A full medium-density application erase is 47 pages. Each page
  takes ~30 ms on real silicon, so the worst-case run is around
  1.4 s. The IWDG is sized for that.
- The library guarantees the caller owns flash unlocking
  externally if you'd rather pair lock / unlock manually — just
  add `FLASH_Lock()` to the success path here and the engine
  works the same way, slightly slower.

### 2. `flash_write(addr, word)`

```c
armbl_flash_result hooks_flash_write(uint32_t addr, uint32_t word);
```

One-liner around `FLASH_ProgramWord`. Assumes flash is already
unlocked because the engine never calls `flash_write` without
first calling `flash_erase`. The library's own
verify-after-write happens in `armbl.c` (it reads back through
`flash_read` and compares); we don't repeat it here.

### 3. `flash_read(addr)`

```c
uint32_t hooks_flash_read(uint32_t addr);
```

`return *(volatile uint32_t *)addr;` — that's the whole hook.
STM32 flash is memory-mapped, so the literal pointer load is the
right thing.

### 4. `write_line(line, len)`

```c
void hooks_write_line(const char *line, size_t len);
```

Polled blocking transmit on USART1. Loops on `USART_FLAG_TXE`
before each byte, then sends a final `'\n'`, then waits on
`USART_FLAG_TC` (transmission complete) so the last bit has
actually left the wire. The TC wait matters because the very
next thing the engine does after the final `DONE\n` is call
`jump_to_app`, which disables USART1 — without the TC wait the
last byte would be cut mid-character.

DMA / interrupt-driven transmit would work too but adds enough
state that it isn't worth the complexity for a bootloader.

### 5. `jump_to_app(app_start)`

```c
void hooks_jump_to_app(uint32_t app_start);
```

The standard Cortex-M3 application-launch sequence:

```c
__disable_irq();                         /* mask CPU IRQs */

USART_ITConfig(USART1, USART_IT_RXNE, DISABLE);
USART_Cmd      (USART1, DISABLE);
TIM_ITConfig   (TIM3, TIM_IT_Update, DISABLE);
TIM_Cmd        (TIM3, DISABLE);

NVIC_DisableIRQ(USART1_IRQn);
NVIC_DisableIRQ(TIM3_IRQn);

NVIC_SetVectorTable(NVIC_VectTab_FLASH,
                    app_start - 0x08000000u);

__set_MSP(*(volatile uint32_t *)app_start);
((void (*)(void))*(volatile uint32_t *)(app_start + 4))();
```

In order:

1. **Mask IRQs** so a stale ISR pending bit doesn't fire while
   we are mid-handover.
2. **Quiet the peripherals** at three layers — IT enable, CMD
   enable, NVIC enable. Belt-and-braces; any of the three can
   keep an interrupt firing on its own.
3. **Relocate the vector table**. `NVIC_SetVectorTable` is the
   StdPeriph wrapper around `SCB->VTOR = …`. Same effect, plays
   nicer with codebases that already use the wrapper elsewhere.
4. **Stack pointer.** The first 32-bit word of any Cortex-M
   image is the initial main-stack-pointer value. Load it.
5. **Reset vector.** The second word is the address of
   `Reset_Handler` (with bit 0 set for Thumb). Cast and call.

The tail `while(1)` is unreachable but keeps the compiler from
emitting a bare return into garbage.

## ISR vs main-loop split

```
USART1 RX ISR          TIM3 update ISR           main loop
-----------------      -----------------         ----------------
read RXDR              clear UIF                 IWDG_ReloadCounter()
armbl_feed_byte(&ctx)  g_tick_10ms_flag = 1
clear RXNE                                       if (flag) {
                                                     flag = 0;
                                                     armbl_tick(&ctx);
                                                 }
```

Three rules the example follows and you should preserve when
adapting it:

1. **No printf, no flash, no peripheral re-init inside an ISR.**
   `armbl_feed_byte` is documented ISR-safe by the library — it
   only touches state that isn't read by `armbl_tick` between
   tick boundaries. Anything else inside the ISR is asking for
   trouble.
2. **TIM3 doesn't do work — it lifts a flag.** Erasing flash
   from inside a tick ISR would block all interrupts for ~30 ms,
   long enough to drop incoming UART bytes silently.
3. **The watchdog is fed only from the main loop.** Never from
   an ISR. If main-loop progress stalls (e.g. a hung
   `armbl_tick` due to a bad hook), the watchdog must reset the
   chip; an ISR-side reload would mask exactly the failure mode
   IWDG exists to catch.

## Watchdog math

```
LSI ≈ 40 kHz
prescaler 32 ⇒ counter clock = 1.25 kHz  (0.8 ms / count)
reload   3750 ⇒ timeout       = 3000 ms
```

Worst-case sequence to time-budget:

```
ASK_VER timeout  1.0 s          (3 retries × 1 s, but main loop
WAIT_VER         3.0 s            kicks IWDG every iteration)
ERASE            1.4 s          (47 × 30 ms FLASH_ErasePage)
ASK_LINE / WAIT  5.0 s          (with 3 retries)
PROG_LINE        ~ms            (per S-record, ~16 words)
MARK_DONE        < 100 µs
```

The 3-second IWDG covers a single `flash_erase` plus several
ticks of slack. The longer per-state timeouts inside
`armbl_config` are independent — they live above the watchdog
and the engine resets them naturally during the upgrade run.

## End-to-end upgrade walk-through

What actually happens on a reset, byte by byte:

1. CPU resets. `Reset_Handler` runs, `main()` enters,
   StdPeriph init brings up the peripherals, `armbl_init`
   zeros the engine context.
2. First `armbl_tick` pushes state from `INIT` to `ASK_VER`,
   sends `V?\n` over USART1.
3. Application (running over a phone-side serial bridge, a
   Raspberry Pi, an ESP32 acting as relay, …) responds with
   either:
   - `V 1 2\n` → engine compares against the version word in
     flash and, for the default policy, boots if the major
     matches and upgrades otherwise.
   - `V 0 0\n` → explicit upgrade ask.
   - silence  → engine retries, then upgrades if no done
     sentinel is set.
4. On the upgrade path: engine sends `L? 1\n`. Host replies
   with the first S-record from `arm-none-eabi-objcopy -O srec
   --srec-forceS3 firmware.elf firmware.s19`.
5. Engine parses the record, calls `flash_erase` for the whole
   application region (only on the first record), programs the
   record's bytes word-by-word, sends `OK\n`, asks `L? 2\n`,
   loops.
6. After the last data record the host sends an `S7…\n`
   terminator. Engine writes `0xCAFEBABE` at `DONE_FLAG_ADDR`,
   sends `DONE\n`, waits for the TC bit, then calls
   `jump_to_app`.

Every transition logs nothing locally — there is no
serial-debug channel competing with the protocol. If you want
visibility while bringing up a new board, set `armbl_config.log`
to a small function that toggles a GPIO pin per state change;
the library is wired for it.

## Adapting to other STM32 families

The example targets the F103, but the hooks are nearly
identical on F0 / F4 / L4. What changes:

| Family | Page / sector size | Key differences |
|---|---|---|
| F0 | 1 KiB pages | StdPeriph names match. Update `RCC_*` and `*_IRQn` for the smaller peripheral set. |
| F1 high-density (>128 KB) | 2 KiB pages | Just change `FLASH_PAGE_SIZE` to `0x800` in `armbl_app_layout.h`. |
| F4 | sector-based, irregular sizes | Replace `FLASH_ErasePage` with `FLASH_EraseSector`. The erase loop is no longer a uniform stride; either hard-code the sector list or use the HAL helper. Voltage range needs to be set for `FLASH_ProgramWord`. |
| L4 | 2 KiB pages | Use HAL's `HAL_FLASH_Program(FLASH_TYPEPROGRAM_DOUBLEWORD, addr, data64)`. Two consecutive 32-bit words become one hook call upstream — easier than it sounds. |

The library never needs to change. That is the entire point of
keeping it vendor-free — every retarget is just edits to
`hooks.c` and `armbl_app_layout.h`.

## License

[MIT](../../LICENSE).
