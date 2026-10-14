#!/usr/bin/env python3
"""Cross-check the routed board against the PiDP-8/I wiring and board_map.h:
every matrix line must join the right Raspberry Pi header pin to the Pico GPIO
the firmware will use, through the jumpers for columns 10 and 11."""
import re, os, sys
import pcbnew
HERE = os.path.dirname(os.path.abspath(__file__))
b = pcbnew.LoadBoard(os.path.join(HERE, 'pidp8-pico-adapter.kicad_pcb'))
m = open(os.path.join(HERE, '..', '..', 'pidp8-panel', 'board_map.h')).read()
arr = lambda n: [int(x) for x in re.search(n + r'\[\d+\]\s*=\s*\{([^}]*)\}', m).group(1).split(',')]
cols, lrows, srows = arr('board_cols'), arr('board_lrows'), arr('board_srows')
strap = int(re.search(r'BOARD_STRAP_GP (\d+)', m).group(1))
GP_PIN = {0: 1, 1: 2, 2: 4, 3: 5, 4: 6, 5: 7, 6: 9, 7: 10, 8: 11, 9: 12, 10: 14, 11: 15, 12: 16, 13: 17,
          14: 19, 15: 20, 16: 21, 17: 22, 18: 24, 19: 25, 20: 26, 21: 27, 22: 29, 26: 31, 27: 32, 28: 34}
# PiDP-8/I (pidp8i gpio-common.c): cols BCM 13,12,11,10,9,8,7,6,5,4,15,14 ; ledrows 20-27 ; rows 16,17,18
BCM_PIN = {2: 3, 3: 5, 4: 7, 5: 29, 6: 31, 7: 26, 8: 24, 9: 21, 10: 19, 11: 23, 12: 32, 13: 33, 14: 8, 15: 10,
           16: 36, 17: 11, 18: 12, 19: 35, 20: 38, 21: 40, 22: 15, 23: 16, 24: 18, 25: 22, 26: 37, 27: 13}
lines = [('col%d' % i, bcm, cols[i]) for i, bcm in enumerate([13, 12, 11, 10, 9, 8, 7, 6, 5, 4, 15, 14])]
lines += [('ledrow%d' % i, 20 + i, lrows[i]) for i in range(8)]
lines += [('swrow%d' % i, 16 + i, srows[i]) for i in range(3)]
pad = {}
for fp in b.GetFootprints():
    for p in fp.Pads():
        pad[(fp.GetReference(), p.GetNumber())] = p.GetNetname()
bad = 0
def ok(c, what):
    global bad
    if not c: bad += 1
    print(('ok   ' if c else 'BAD  ') + what)
for name, bcm, gp in lines:
    hp = BCM_PIN[bcm]
    pico = pad[('U1', str(GP_PIN[gp]))]
    hdr = pad[('J1', str(hp))]
    if name in ('col10', 'col11'):
        jp = 'JP1' if name == 'col10' else 'JP2'
        ser = BCM_PIN[3 if name == 'col10' else 2]
        ok(pad[(jp, '2')] == pico and pad[(jp, '1')] == hdr and pad[(jp, '3')] == pad[('J1', str(ser))],
           '%-8s Pi pin %2d (STD) / %d (SER) -> %s -> Pico GP%d' % (name, hp, ser, jp, gp))
    else:
        ok(hdr == pico and hdr, '%-8s Pi pin %2d (BCM%-2d) -> Pico GP%-2d  net %s' % (name, hp, bcm, gp, hdr))
ok(pad[('U1', str(GP_PIN[strap]))] == 'GND', 'strap GP%d tied to GND' % strap)
ok(all(pad[('J1', str(n))] == '+3V3' for n in (1, 17)) and pad[('U1', '36')] == '+3V3', '3V3: Pi pins 1, 17 <- Pico 3V3(OUT)')
ok(all(pad[('J1', str(n))] == '+5V' for n in (2, 4)) and pad[('U1', '40')] == '+5V', '5V: Pi pins 2, 4 <- Pico VBUS')
ok(all(pad[('J1', str(n))] == 'GND' for n in (6, 9, 14, 20, 25, 30, 34, 39)), 'all Pi ground pins on GND')
used = set(cols + lrows + srows)
ok(len(used) == 23 and strap not in used, '23 distinct GPIOs, strap unused')
print('BOARD CHECK', 'PASS' if not bad else 'FAIL (%d)' % bad)
sys.exit(1 if bad else 0)
