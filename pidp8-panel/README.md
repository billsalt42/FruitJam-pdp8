# PiDP-8/I front panel on the Fruit Jam

The PiDP-8/I panel is designed to sit on a Raspberry Pi's 40-pin header. It needs
23 GPIO lines, driven continuously to multiplex the lamps. The Fruit Jam doesn't
have that many free pins, so a **Raspberry Pi Pico 2** (or an original **Pico**)
takes the Pi's place behind the panel. It runs the lamps and switches and connects to
the Fruit Jam with a USB cable:

```
PiDP-8/I panel ──(23 wires + power)── Pico 2 ──USB── Fruit Jam USB-A port
                                                     (the other USB-A port: keyboard)
```

The Pico gets its power from the Fruit Jam over USB and passes it on to the panel.

## Firmware

1. Hold BOOTSEL on the Pico and plug it into a computer. It appears as a USB drive.
2. Copy the matching file onto that drive:
   * `pidp8_panel_pico2.uf2` for a Pico 2 or Pico 2 W
   * `pidp8_panel_pico.uf2` for an original Pico or Pico W
3. Flash `pdp8_fruitjam.uf2` v1.4 or later onto the Fruit Jam. It detects the panel
   automatically.

To build them yourself, run `make panel` (Pico SDK 2.x, `PICO_SDK_PATH` set).

## Adapter board

Instead of wiring by hand you can have a small board made: [`hardware/pico-adapter`](../hardware/pico-adapter/README.md).
It plugs into the PiDP-8/I where the Raspberry Pi was, with the Pico soldered to its back. The firmware
(v1.2 and later) recognises it automatically, so the table below is only needed for hand wiring.

## Wiring

**Remove the Raspberry Pi first.** Never connect a Pi and the Pico to the panel
at the same time.

Header pins are the physical pin numbers of the PiDP-8/I's 40-pin Raspberry Pi
connector. Female-to-female jumper wires work well, or a 40-pin ribbon cable with a
breakout.

| Signal | Pi GPIO (BCM) | Pi header pin | Pico GPIO | Pico pin |
|---|---|---|---|---|
| column 0  | 13 | 33 | GP0  | 1  |
| column 1  | 12 | 32 | GP1  | 2  |
| column 2  | 11 | 23 | GP2  | 4  |
| column 3  | 10 | 19 | GP3  | 5  |
| column 4  | 9  | 21 | GP4  | 6  |
| column 5  | 8  | 24 | GP5  | 7  |
| column 6  | 7  | 26 | GP6  | 9  |
| column 7  | 6  | 31 | GP7  | 10 |
| column 8  | 5  | 29 | GP8  | 11 |
| column 9  | 4  | 7  | GP9  | 12 |
| column 10 | 15 *(serial mod: 3)* | 10 *(serial mod: 5)* | GP10 | 14 |
| column 11 | 14 *(serial mod: 2)* | 8 *(serial mod: 3)*  | GP11 | 15 |
| lamp row 0 | 20 | 38 | GP12 | 16 |
| lamp row 1 | 21 | 40 | GP13 | 17 |
| lamp row 2 | 22 | 15 | GP14 | 19 |
| lamp row 3 | 23 | 16 | GP15 | 20 |
| lamp row 4 | 24 | 18 | GP16 | 21 |
| lamp row 5 | 25 | 22 | GP17 | 22 |
| lamp row 6 | 26 | 37 | GP18 | 24 |
| lamp row 7 | 27 | 13 | GP19 | 25 |
| switch row 0 | 16 | 36 | GP20 | 26 |
| switch row 1 | 17 | 11 | GP21 | 27 |
| switch row 2 | 18 | 12 | GP22 | 29 |
| 3.3 V | — | 1 (or 17) | 3V3 OUT | 36 |
| 5 V   | — | 2 (or 4)  | VBUS    | 40 |
| ground | — | 6 (or 9, 14, 20, 25, 30, 34, 39) | GND | 38 (or 3, 8, 13, ...) |

"Serial mod" refers to PiDP-8/I boards modified (Oscar Vermeulen's or James L-W's
modification) to free the Pi's serial port. On those boards, columns 10 and 11 come
from header pins 5 and 3 instead of 10 and 8.

The Pico drives the panel exactly the way the pidp8i software drives it from a Pi.
A lamp row is enabled by driving its row line high (into the board's UDN2981
source driver). A lamp lights when its column is pulled low. For the switches, the
columns become pulled-up inputs and each switch row is pulled low in turn. Both
boards use 3.3 V logic. Only one lamp row is lit at a time, so the current stays
modest, but check that nothing gets warm the first time you power it up.

## Self-test

Power the Pico on its own, from a phone charger or a computer with no program
using its serial port. It then runs a self-test:

* The PC, MA and MB lamp rows follow the three switch rows: flip the switch
  register, IF/DF and the START…SING INST keys, and the lamps above them follow.
* The AC row shows a brightness ramp from dark to full.
* A light chases across MQ and the rows below it.

`python3 paneltest.py /dev/ttyACM0` (needs pyserial) does the same from a
computer and also prints the switch settings. That makes it easy to find a
crossed wire.

## Troubleshooting the link

The Pico's on-board LED shows how far the USB connection got:

| LED | Meaning |
|---|---|
| short flash once a second | no USB host has configured the Pico (check the cable is a data cable, try the other USB-A port) |
| slow on/off blink | a host has configured it, but no lamp data is arriving |
| steady on | lamp frames are arriving: the panel is running from the emulator |

On the Fruit Jam, the F12 menu shows a `USB host:` line listing every device it
has found (the Pico is `2e8a:000a`) and a `PiDP-8/I panel:` status line. Press
the space bar in the menu to refresh it.

## Using it

When the panel is plugged into the Fruit Jam and a system is running:

| Switch | Action |
|---|---|
| switch register | read by the program (OSR/LAS); applied whenever you flip a switch, so the F11 on-screen panel and the hardware can both set it |
| LOAD ADD | PC ← switch register, IF ← IF switches, DF ← DF switches (while halted) |
| START | clear CPU and I/O, AC = L = 0, run from PC (while halted) |
| DEP | memory[IF:PC] ← switch register, PC + 1 (while halted) |
| EXAM | show memory[IF:PC] in MA/MB, PC + 1 (while halted) |
| CONT | continue; with SING STEP or SING INST up, execute one instruction |
| STOP | halt |
| SING STEP / SING INST | raising either halts a running program |

The lamps use the same averaged sampling as the on-screen panel, with 32
brightness levels and a little incandescent afterglow. That makes the famous
idle loops recognisable. While halted, the lamps show the registers as they
stand. PAUSE is not simulated.

The PiDP-8/I is a PDP-8/**I** panel and the emulator is a PDP-8/**E**, but the
two front panels do the same job. The 8/E's CLEAR and EXTD ADDR LOAD are folded
into START and LOAD ADD, as on the 8/I.

## Protocol

The protocol is in `pidp_proto.h`: a 97-byte lamp frame about 50 times a second
from the emulator, and a 7-byte switch report every 100 ms (and on each change)
from the panel. Any program that speaks it can drive the panel, for example
`paneltest.py`.
