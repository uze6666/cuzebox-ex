# GDB remote stub

Start the emulator with `--gdb` and `avr-gdb`, or any IDE that drives it
(Eclipse, VS Code, Microchip Studio through avr-gdb), can attach to the running
game: source-level breakpoints and stepping against the game's `.elf`,
registers, flash, SRAM, EEPROM and SPI RAM, watchpoints, and a set of `monitor`
commands for things only an emulator can do. The same connection lets a script
or a coding agent drive a ROM without a person at the keyboard: type into the
keyboard dongle, press controller buttons, run an exact number of frames, take
a screenshot.

It is built with `FLAG_GDB=1` (the default) and does nothing until `--gdb` is
given. It is not in the Emscripten build.

```sh
./cuzebox game.hex --gdb            # listens on TCP port 1234
avr-gdb game.elf -ex "target remote :1234"
```

## Command line

| Option | Effect |
| --- | --- |
| `--gdb[=port]` | Enable the stub (default port 1234) |
| `--gdb-wait` | Hold the CPU at reset until a client attaches |
| `--gdb-reverse` | Advertise reverse step / reverse continue. Off by default: avr-gdb 7.8 from the Microchip toolchain fails when it sees reverse support advertised |
| `--gdb-log[=file]` | Log the packet traffic (default `cuzebox-gdb.log`) |
| `--gdb-focus-ide` | Windows: when a breakpoint halts the CPU, bring the window that launched the emulator back to the front |
| `--gdb-nolimit` | Frame limiter off from the first frame, window hidden and sound muted: an unattended scripted run |
| `--gdb-show` | With `--gdb-nolimit`: keep the window visible |

## Address spaces

avr-gdb's usual layout, plus two regions it does not know about:

| Region | GDB address |
| --- | --- |
| Flash | `0x000000` |
| SRAM and I/O | `0x800000` |
| EEPROM | `0x810000` |
| SPI RAM | `0x820000`; `monitor spiram` also reads and writes it by its own address |

## Monitor commands

Typed in GDB as `monitor <command>`. Every one also takes `--json`.

| Command | Purpose |
| --- | --- |
| `status`, `cycles` | Halted or running, PC, cycle counters, frame and scanline position |
| `ports [<name>]` | Decoded ATmega644 I/O registers |
| `spiram read <addr> <len>`, `spiram write <addr> <hex>` | The SPI RAM |
| `screenshot [<path>] [--overlay]` | A PNG of the frame as it stands, also mid-frame |
| `savestate [<path>]`, `loadstate <path>` | The stub's own snapshot (see the limits below) |
| `reset` | Power-cycle the emulated machine, keeping the session |
| `input <player> [<buttons>]`, `input <player> press <btns> [frames]` | Set or tap controller buttons |
| `input record <path>`, `input replay <path>`, `input stop` | Controller state per frame, to and from a file |
| `key <name> [down\|up\|tap]` | A host key by its SDL name (`F9`, `Escape`, `Left_Ctrl`, `Return`) |
| `type <text>` | Type into the keyboard dongle; `--wait` halts once the ROM has read it all |
| `mouse <dx> <dy> [left\|right\|both]` | Move the SNES mouse |
| `frames [<n>]` | Run exactly n frames on resume, then halt |
| `speed [limited\|unlimited]` | The frame limiter, where F4 cannot reach |
| `break-on [<name> [on\|off\|<par>]]` | Stop on a Uzebox-specific fault: `video-overrun`, `stack-overflow`, `unmapped-io`, `uninit-read`, `illegal-opcode`, `irq-latency` |
| `profile [clear\|on\|off]` | Cycle cost per scanline: rows seen, the worst row, how many overran |
| `overlay [on\|off]` | The "paused" panel drawn over the picture while halted (F5 while halted) |
| `reverse depth [<frames>]` | How much history reverse execution keeps |

When a script types a line into a ROM, submit it with a separate
`monitor key Return --wait` rather than a `\n` inside `type`: an unescaped
newline is eaten by the scripting language and the line is typed but never
entered.

## Living beside the built-in debugger

The stub and the built-in debugger (the debugger window, the TCP API and the
web debugger) can be in the same build and used in the same run. Each keeps its
own breakpoints and watchpoints.

- **A stop by either stops both.** A GDB breakpoint halts the CPU and the stub
  holds it; the built-in debugger is not told it hit a breakpoint of its own.
  A breakpoint, watchpoint or step of the built-in debugger pauses the
  emulator as before, and an attached GDB client is told the target halted, as
  if Ctrl-C had been pressed, so both show the same stopped machine.
- **Resuming from GDB resumes the emulator**, including a pause the built-in
  debugger made. Resuming from the built-in debugger while GDB holds the CPU
  takes effect when GDB lets go.
- **The API and web pages stay up while GDB holds the CPU**, so memory, the
  disassembly and the SD and timing views can be read at a GDB breakpoint.
  The emulator's menus do not open during a GDB halt.
- **Both stop mid-scanline.** A halted frame is continued, not restarted, when
  the CPU runs again, and the finished picture stays on screen while halted.

Limits of this first version:

- The stub's snapshots (`savestate`, `loadstate`, and the history behind
  reverse execution) hold the CPU, I/O, video timing and SPI RAM. They do not
  hold the state of the UART and ESP module, multitap, lightgun, virtual input
  devices or the SD card. For a complete state use the emulator's save-state
  slots.
- Loading another ROM from the menu while a GDB client is attached does not
  reset the client's view; detach first, or use `monitor reset` after loading.
- GDB watchpoints cover the load and store instructions (`LD`, `LDD`, `LDS`,
  `ST`, `STD`, `STS`) and I/O register access, not pushes and pops.
- `--gdb-nolimit` turns the frame limiter off in the running configuration; if
  settings are saved during that run, the limiter is saved off with them.

## Keys

| Key | Action |
| --- | --- |
| F5, while a GDB client holds the CPU | Show or hide the "paused" panel |
| Ctrl+Q | Quit, also when a ROM has captured the keyboard |

## Tests

`test/gdb/run_tests.sh` starts the emulator with the stub on a scratch port and
runs `test/gdb/test_stub.py` against it (119 checks: registers, memory spaces,
breakpoints, stepping, watchpoints, monitor commands, input injection). It
needs Python 3 and, to rebuild the small probe program, `avr-gcc`.
