# TM4C1294NCPDT worked example

This directory wires the `c-ARM-bootload` library (`armbl_*` API)
into a real Texas Instruments TM4C1294NCPDT (Tiva-C, Cortex-M4F,
1 MiB flash) using TivaWare DriverLib. It is not a complete
project — there is no linker script (IAR `.icf`) or startup file
or Makefile — only the four C files that make up the
bootloader's own translation units. Drop them into your existing
IAR / Code Composer Studio / arm-none-eabi-gcc project that
already links against TivaWare (`driverlib.lib` /
`libdriver.a`) and you have a working bootloader.

## Files in this directory

| File | What it does |
|---|---|
| `main.c` | Bootloader entry point. Brings up FPU + system clock, GPIOA pins for UART0, UART0 itself, SysTick (10 ms tick), Watchdog0 and the NVIC, then runs the main loop that pets the watchdog and ticks the engine every 10 ms. Owns the two ISRs (`UART0IntHandler`, `SysTickIntHandler`). |
| `hooks.c` | The five `armbl_hooks` callbacks: flash erase / write / read, `write_line`, and `jump_to_app`. All TivaWare `Flash*` and `UART*` code lives here. |
| `hooks.h` | Declarations of the five hooks. Pulled in by `main.c`. |
| `armbl_app_layout.h` | TM4C129-specific knobs grouped in one header: flash addresses, page size, baud, target system clock. Edit this header (and only this header) when retargeting to a different application split. |

The library itself (`armbl.h` / `armbl.c`) sits two directories
up. Includes use the relative path `"../../armbl.h"`.

## Reference hardware

| Property | Value |
|---|---|
| MCU | TM4C1294NCPDT (Tiva-C, Cortex-M4F) |
| Flash size | 1 MiB |
| Flash erase granularity | 1 KiB block (`FlashErase`) |
| RAM | 256 KiB |
| External crystal | 25 MHz on OSC0 / OSC1 |
| SYSCLK | 120 MHz (`SysCtlClockFreqSet` with `SYSCTL_CFG_VCO_480`) |
| Bootloader UART | UART0 on PA0 (U0RX) / PA1 (U0TX), 115 200 8N1 |
| Tick source | SysTick, 10 ms reload |
| Watchdog | Watchdog0 (clocked from system clock), 3.0 s timeout |

If your board uses a different crystal or a different baud,
change the relevant init function in `main.c` and adjust
constants in `armbl_app_layout.h`. The library files don't
change.

## Flash partitioning

The example uses a 16 KiB bootloader at the bottom of flash, a
~240 KiB application above, and two reserved 4-byte slots near
the end of the application region for the version word and the
upgrade-done sentinel.

```
0x0000_0000  ┐
             │   bootloader text + rodata + vectors        16 KiB
0x0000_3FFF  ┘
0x0000_4000  ┐ <- APP_START_ADDR
             │   application image                       ≈240 KiB
0x0003_FFEF  ┘
0x0003_FFF0      version word (4 B)         <- VERSION_ADDR
0x0003_FFF4      done sentinel (4 B)        <- DONE_FLAG_ADDR
0x0003_FFF8      reserved
0x0003_FFFF                                 <- APP_END_ADDR
0x0004_0000      ----- unused ROM (reserve for future) -----
```

Note the address range — TM4C129 flash starts at `0x00000000`,
not `0x08000000` like STM32. The example only uses the first
256 KiB of the chip's 1 MiB flash because the typical
bootloader-managed application is mid-sized; bump `APP_END_ADDR`
to `0x000FFFFF` if you want to use the whole device.

The application's linker script must:

- Place its vector table at `APP_START_ADDR` (in IAR `.icf`:
  `place at address mem:0x00004000 { readonly section .intvec };`
  with the rest of `readonly` placed in a region starting at
  `0x00004000`).
- Run `IntVTableBaseSet(0x00004000)` (or write `HWREG(NVIC_VTABLE)`
  directly) early in `Reset_Handler`. Many TivaWare project
  templates already do this from `main()` instead.
- Reserve the two trailing words at `VERSION_ADDR` and
  `DONE_FLAG_ADDR` so the linker doesn't pack code over them.

The bootloader's own linker script puts its vector table at
`0x00000000` and limits its `.text` to `< 0x00004000`.

## The five hooks

The whole point of the library is that nothing inside it knows
about TM4C129. The example hooks translate the abstract
operations into TivaWare DriverLib calls.

### 1. `flash_erase(start, end)`

```c
armbl_flash_result hooks_flash_erase(uint32_t start, uint32_t end);
```

Walks `[start, end]` inclusive in `FLASH_PAGE_SIZE`-aligned
strides, calling `FlashErase(addr)` for each block.

- **No flash unlock dance.** Unlike STM32, the TM4C1294's flash
  controller does not expose an unlock register through DriverLib.
  Calling `FlashErase` directly is safe; there is no
  `FLASH_Unlock` / `FLASH_Lock` pair to manage. This also means
  `flash_write` doesn't need a "leave it unlocked" comment — the
  controller is always accessible to the CPU.
- A full 240-KiB application erase is 240 blocks. Each
  `FlashErase` takes ~20 ms on real silicon, so the worst-case
  run is around 4.8 s — which is *longer* than our 3-second
  watchdog timeout. Two ways to handle this:
  1. Bump the watchdog reload to ~6 s if you keep a single
     fire-and-forget erase call. Documented in
     `armbl_app_layout.h` if you change `APP_END_ADDR` to use
     more flash.
  2. Or feed the watchdog inside the erase loop. The example
     keeps the loop minimal because the *typical* upgrade only
     erases a much smaller in-use region; the engine runs the
     erase exactly once per upgrade and the next steps (line
     I/O) feed the watchdog naturally.
- `FlashErase` returns 0 on success and non-zero on a hardware
  error (e.g. trying to erase a write-protected block). The
  hook bails on the first failure with `ARMBL_FLASH_ERR`.

### 2. `flash_write(addr, word)`

```c
armbl_flash_result hooks_flash_write(uint32_t addr, uint32_t word);
```

`FlashProgram(&buf, addr, 4)` — the source pointer must be
word-aligned, so we stage the input on the stack:

```c
uint32_t buf = word;
int32_t s = FlashProgram(&buf, addr, 4);
```

`FlashProgram` returns 0 on success. The library's own
verify-after-write reads back through `flash_read` and compares,
so we don't repeat that here.

### 3. `flash_read(addr)`

```c
uint32_t hooks_flash_read(uint32_t addr);
```

`return *(volatile uint32_t *)addr;` — the literal one-liner.
TM4C129 flash is memory-mapped; the pointer load is the right
thing.

### 4. `write_line(line, len)`

```c
void hooks_write_line(const char *line, size_t len);
```

Polled blocking transmit on UART0. Per byte:

```c
UARTCharPut(UART0_BASE, byte);
```

Then a final `'\n'`, then:

```c
while (UARTBusy(UART0_BASE)) { }
```

The `UARTBusy` wait at the end matters because the very next
thing the engine does after `DONE\n` is call `jump_to_app`,
which disables UART0 — without the wait the last byte would be
cut mid-character.

### 5. `jump_to_app(app_start)`

```c
void hooks_jump_to_app(uint32_t app_start);
```

The standard Cortex-M application-launch sequence, expressed in
TivaWare:

```c
IntMasterDisable();                          /* mask CPU IRQs */

UARTIntDisable(UART0_BASE, UART_INT_RX | UART_INT_RT);
UARTDisable   (UART0_BASE);
SysTickIntDisable();
SysTickDisable();
IntDisable    (INT_UART0);

IntVTableBaseSet(app_start);                 /* relocate vectors */

__set_MSP(*(volatile uint32_t *)app_start);
((void (*)(void))*(volatile uint32_t *)(app_start + 4))();
```

In order:

1. **Mask IRQs** so a stale ISR pending bit doesn't fire while
   we are mid-handover.
2. **Quiet the peripherals** at three layers — interrupt enable,
   peripheral enable, NVIC enable. SysTick has no NVIC entry
   (it's a core exception), so only the SysTick-side disables
   are needed for it.
3. **Relocate the vector table.** `IntVTableBaseSet` is the
   TivaWare wrapper around `HWREG(NVIC_VTABLE) = …`. It plays
   nicely with codebases that already use the wrapper elsewhere.
4. **Stack pointer.** The first 32-bit word of any Cortex-M
   image is the initial main-stack-pointer value. Load it via
   `__set_MSP`, the CMSIS intrinsic shared by IAR and GCC.
5. **Reset vector.** The second word is the address of
   `Reset_Handler` (with bit 0 set for Thumb). Cast and call.

The tail `while(1)` is unreachable but keeps the compiler from
emitting a bare return into garbage.

## ISR vs main-loop split

```
UART0 RX ISR              SysTick ISR              main loop
---------------------     -------------            --------------------
status = IntStatus()      g_tick_10ms_flag = 1     WatchdogReloadSet()
IntClear(status)
while CharsAvail()                                 if (flag) {
    armbl_feed_byte()                                  flag = 0;
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
2. **SysTick doesn't do work — it lifts a flag.** Erasing flash
   from inside a tick ISR would block all interrupts for tens of
   milliseconds, long enough to drop incoming UART bytes
   silently.
3. **The watchdog is fed only from the main loop.** Never from
   an ISR. If main-loop progress stalls (for example a hung
   `armbl_tick` due to a bad hook), the watchdog must reset the
   chip; an ISR-side reload would mask exactly the failure mode
   the watchdog exists to catch.

## Watchdog math

```
Watchdog0 source clock = system clock = 120 MHz
Reload  3 × 120 000 000 = 360 000 000 ⇒ ≈3 s timeout
```

TM4C129 watchdogs are two-stage by default: the first timeout
fires the watchdog interrupt and the *second* one resets the
device. We deliberately leave the watchdog interrupt disabled
so the first timeout itself triggers the reset path —
`WatchdogResetEnable` does this, no IRQ vector wiring needed.

`WatchdogLockState` is checked before `WatchdogReloadSet` in
`watchdog_init` so a warm boot from a previously-locked WDT
doesn't silently reject the reload write. Once running, the
watchdog's load register is reachable without unlocking, so the
main-loop pet just calls `WatchdogReloadSet` again.

Worst-case time budget you should consider when tuning the
reload value:

```
ASK_VER timeout    1.0 s        (3 retries × 1 s)
ERASE              up to ~5 s   (240 × 20 ms, full app region)
PROG_LINE          ~ms          (per S-record, ~16 words)
MARK_DONE          < 100 µs
```

The default 3 s reload covers all paths *except* a full-app
erase. If you keep `APP_END_ADDR` at `0x0003FFFF` you should
either bump the watchdog reload or break up the erase. The
typical case — incremental upgrades to a smaller in-use region
— never trips this.

## End-to-end upgrade walk-through

What actually happens on a reset, byte by byte:

1. CPU resets. Reset vector runs, `main()` enters, TivaWare init
   brings up the FPU, the 120 MHz PLL, GPIOA, UART0, SysTick and
   Watchdog0; `armbl_init` zeros the engine context.
2. First `armbl_tick` pushes state from `INIT` to `ASK_VER`,
   sends `V?\n` over UART0.
3. The application side (which can be the running firmware over
   a phone bridge, a relay MCU, an attached SBC, …) responds:
   - `V 1 2\n` → engine compares against the version word in
     flash; default policy boots if the major matches and
     upgrades otherwise.
   - `V 0 0\n` → explicit upgrade ask.
   - silence  → engine retries, then upgrades if no done
     sentinel is set.
4. On the upgrade path: engine sends `L? 1\n`. Host replies
   with the first S-record from
   `arm-none-eabi-objcopy -O srec --srec-forceS3 firmware.elf firmware.s19`.
5. Engine parses the record, calls `flash_erase` for the whole
   application region (only on the first record), programs the
   record's bytes word-by-word, sends `OK\n`, asks `L? 2\n`,
   loops.
6. After the last data record the host sends an `S7…\n`
   terminator. Engine writes `0xCAFEBABE` at `DONE_FLAG_ADDR`,
   sends `DONE\n`, waits for `UARTBusy` to clear, then calls
   `jump_to_app`.

Every transition logs nothing locally — there is no
serial-debug channel competing with the protocol. If you want
visibility while bringing up a new board, set `armbl_config.log`
to a small function that toggles a GPIO pin per state change.
The library is wired for it.

## TM4C129 specifics, called out

A few items worth flagging when comparing to the STM32F1
example next door:

| Topic | STM32F1 | TM4C129 |
|---|---|---|
| Flash base address | `0x08000000` | `0x00000000` |
| Flash unlock | `FLASH_Unlock` / `FLASH_Lock` required | none — controller is always live |
| Flash erase API | `FLASH_ErasePage(addr)` (1 KiB) | `FlashErase(addr)` (1 KiB block) |
| Flash program API | `FLASH_ProgramWord(addr, w)` | `FlashProgram(&w, addr, 4)` |
| Tick source | TIM3 update IRQ | SysTick exception |
| Watchdog | IWDG (LSI 40 kHz) | Watchdog0 (sysclk) |
| Vector relocation | `NVIC_SetVectorTable(NVIC_VectTab_FLASH, off)` | `IntVTableBaseSet(addr)` |
| ISR symbol form | `USART1_IRQHandler` (CMSIS) | `UART0IntHandler` (TivaWare startup) |
| FPU | none (Cortex-M3) | enable + lazy-stack on Cortex-M4F |

The library doesn't see any of this — every chip-specific
quirk is contained in `hooks.c` and the init functions in
`main.c`.

## Adapting to other Tiva / SimpleLink parts

Most of the changes live in `armbl_app_layout.h`:

| Family | Differences |
|---|---|
| TM4C123 (LM4F) | 80 MHz cap, 256 KiB flash, same `FlashErase` / `FlashProgram`. Drop `SysCtlClockFreqSet` for `SysCtlClockSet`; lower `SYS_CLOCK_HZ` to `80000000`. UART0 layout is identical. |
| TM4C129 high-flash variants | Same code. Just bump `APP_END_ADDR`. |
| MSP432 | Different flash controller (`FlashCtl_*`); rewrite the three flash hooks against the new API. UART, SysTick and watchdog are similar. |
| CC13xx / CC26xx (SimpleLink) | Driver model is different (`FlashSafe_*`); start from scratch on the hooks but keep the `main.c` shape. |

The library never needs to change. That is the entire point of
keeping it vendor-free — every retarget is just edits to
`hooks.c` and `armbl_app_layout.h`.

## License

[MIT](../../LICENSE).
