#!/usr/bin/env python3
"""
Write the adapter's schematic (KiCad 7 .kicad_sch) from the board's nets, so
the two always agree, and export it as PDF/SVG with kicad-cli.

Every pin carries a net label; pins with no net get a no-connect flag.
"""
import os, subprocess, uuid
import pcbnew

HERE = os.path.dirname(os.path.abspath(__file__))
os.chdir(HERE)
board = pcbnew.LoadBoard('pidp8-pico-adapter.kicad_pcb')
nets = {}
for fp in board.GetFootprints():
    for p in fp.Pads():
        nets[(fp.GetReference(), p.GetNumber())] = p.GetNetname()

U = lambda: str(uuid.uuid4())
ROOT = U()
F = '(effects (font (size 1.27 1.27)))'
FH = '(effects (font (size 1.27 1.27)) hide)'

PI_NAMES = {1: '3V3', 2: '5V', 3: 'GPIO2', 4: '5V', 5: 'GPIO3', 6: 'GND', 7: 'GPIO4', 8: 'GPIO14', 9: 'GND',
            10: 'GPIO15', 11: 'GPIO17', 12: 'GPIO18', 13: 'GPIO27', 14: 'GND', 15: 'GPIO22', 16: 'GPIO23',
            17: '3V3', 18: 'GPIO24', 19: 'GPIO10', 20: 'GND', 21: 'GPIO9', 22: 'GPIO25', 23: 'GPIO11',
            24: 'GPIO8', 25: 'GND', 26: 'GPIO7', 27: 'ID_SD', 28: 'ID_SC', 29: 'GPIO5', 30: 'GND', 31: 'GPIO6',
            32: 'GPIO12', 33: 'GPIO13', 34: 'GND', 35: 'GPIO19', 36: 'GPIO16', 37: 'GPIO26', 38: 'GPIO20',
            39: 'GND', 40: 'GPIO21'}
PICO_NAMES = {1: 'GP0', 2: 'GP1', 3: 'GND', 4: 'GP2', 5: 'GP3', 6: 'GP4', 7: 'GP5', 8: 'GND', 9: 'GP6',
              10: 'GP7', 11: 'GP8', 12: 'GP9', 13: 'GND', 14: 'GP10', 15: 'GP11', 16: 'GP12', 17: 'GP13',
              18: 'GND', 19: 'GP14', 20: 'GP15', 21: 'GP16', 22: 'GP17', 23: 'GND', 24: 'GP18', 25: 'GP19',
              26: 'GP20', 27: 'GP21', 28: 'GND', 29: 'GP22', 30: 'RUN', 31: 'GP26', 32: 'GP27', 33: 'AGND',
              34: 'GP28', 35: 'ADC_VREF', 36: '3V3_OUT', 37: '3V3_EN', 38: 'GND', 39: 'VSYS', 40: 'VBUS'}

def pin(num, name, x, y, ang):
    return ('(pin passive line (at %.2f %.2f %d) (length 2.54) (name "%s" %s) (number "%s" %s))'
            % (x, y, ang, name, F, num, F))

def two_column_symbol(lib, left, right, height, ref, value, width=10.16):
    """left/right: lists of (number, name), top to bottom"""
    hw = width / 2
    body = '(rectangle (start %.2f %.2f) (end %.2f %.2f) (stroke (width 0.254) (type default)) (fill (type background)))' % (
        -hw, height / 2, hw, -height / 2)
    pins = []
    pos = {}
    for i, (n, nm) in enumerate(left):
        y = height / 2 - 2.54 * (i + 1)
        pins.append(pin(n, nm, -hw - 2.54, y, 0)); pos[n] = (-hw - 2.54, y, 'L')
    for i, (n, nm) in enumerate(right):
        y = height / 2 - 2.54 * (i + 1)
        pins.append(pin(n, nm, hw + 2.54, y, 180)); pos[n] = (hw + 2.54, y, 'R')
    name = lib.split(':')[1]
    s = ('(symbol "%s" (pin_names (offset 1.016)) (in_bom yes) (on_board yes)\n'
         '  (property "Reference" "%s" (at 0 %.2f 0) %s)\n  (property "Value" "%s" (at 0 %.2f 0) %s)\n'
         '  (property "Footprint" "" (at 0 0 0) %s)\n  (property "Datasheet" "" (at 0 0 0) %s)\n'
         '  (symbol "%s_0_1" %s)\n  (symbol "%s_1_1" %s))'
         % (lib, ref, height / 2 + 1.5, F, value, -height / 2 - 1.5, F, FH, FH, name, body, name, '\n    '.join(pins)))
    return s, pos

pi_sym, pi_pos = two_column_symbol('adapter:RPi_GPIO_40',
                                   [(str(n), PI_NAMES[n]) for n in range(1, 41, 2)],
                                   [(str(n), PI_NAMES[n]) for n in range(2, 41, 2)], 55.88, 'J', 'RPi_GPIO_40', 25.4)
pico_sym, pico_pos = two_column_symbol('adapter:Pico',
                                       [(str(n), PICO_NAMES[n]) for n in range(1, 21)],
                                       [(str(n), PICO_NAMES[n]) for n in range(40, 20, -1)], 55.88, 'U', 'Pico', 25.4)
jp_sym, jp_pos = two_column_symbol('adapter:SolderJumper3', [('1', 'STD'), ('3', 'SER')], [('2', 'C')], 7.62, 'JP', 'SolderJumper_3', 7.62)

items = []
def place(lib, ref, value, footprint, x, y, pos, pins):
    props = ('(property "Reference" "%s" (at %.2f %.2f 0) %s) (property "Value" "%s" (at %.2f %.2f 0) %s) '
             '(property "Footprint" "%s" (at %.2f %.2f 0) %s) (property "Datasheet" "" (at %.2f %.2f 0) %s)'
             % (ref, x, y - 30.5 if ref in ('J1', 'U1') else y - 6, F, value, x, y + 30.5 if ref in ('J1', 'U1') else y + 6, F,
                footprint, x, y, FH, x, y, FH))
    items.append('(symbol (lib_id "%s") (at %.2f %.2f 0) (unit 1) (in_bom yes) (on_board yes) (dnp no) (uuid %s) %s %s '
                 '(instances (project "pidp8-pico-adapter" (path "/%s" (reference "%s") (unit 1)))))'
                 % (lib, x, y, U(), props, ' '.join('(pin "%s" (uuid %s))' % (p, U()) for p in pins), ROOT, ref))
    for n, (px, py, side) in pos.items():
        X, Y = x + px, y - py
        nn = nets.get((ref, n), '')
        if not nn:
            items.append('(no_connect (at %.2f %.2f) (uuid %s))' % (X, Y, U()))
        else:
            ang, just = (180, 'right') if side == 'L' else (0, 'left')
            items.append('(label "%s" (at %.2f %.2f %d) (fields_autoplaced) (effects (font (size 1.27 1.27)) (justify %s bottom)) (uuid %s))'
                         % (nn, X, Y, ang, just, U()))

place('adapter:RPi_GPIO_40', 'J1', 'PiDP-8/I connector', 'adapter:RPi_GPIO_2x20', 63.5, 76.2, pi_pos, [str(n) for n in range(1, 41)])
place('adapter:Pico', 'U1', 'Raspberry Pi Pico / Pico 2', 'adapter:RPi_Pico_back', 167.64, 76.2, pico_pos, [str(n) for n in range(1, 41)])
place('adapter:SolderJumper3', 'JP1', 'COL10 STD/SER', 'adapter:SolderJumper_3', 116.84, 127.0, jp_pos, ['1', '2', '3'])
place('adapter:SolderJumper3', 'JP2', 'COL11 STD/SER', 'adapter:SolderJumper_3', 116.84, 144.78, jp_pos, ['1', '2', '3'])

def note(t, x, y, size=1.5):
    items.append('(text "%s" (at %.2f %.2f 0) (effects (font (size %.2f %.2f)) (justify left bottom)) (uuid %s))' % (t, x, y, size, size, U()))

note('PiDP-8/I <-> Raspberry Pi Pico panel interface adapter', 25.4, 25.4, 2.5)
note('J1 takes the Raspberry Pi\'s place on the PiDP-8/I. U1 sits on the back of the board.', 25.4, 31.75)
note('Net names: COLn = matrix column n, LROWn = lamp row n, SROWn = switch row n (pidp8i gpio-common.c).', 25.4, 35.56)
note('GPIO choice is in pidp8-panel/board_map.h. GP28 is grounded so the firmware knows it is on this board.', 25.4, 39.37)
note('JP1/JP2: bridge C-STD for a standard PiDP-8/I, C-SER for one with the serial-port mod.', 101.6, 157.48)

sch = ('(kicad_sch (version 20230121) (generator eeschema)\n  (uuid %s)\n  (paper "A3")\n'
       '  (title_block (title "PiDP-8/I Pico adapter") (rev "1.0") (company "PDP-8 Fruit Jam project"))\n'
       '  (lib_symbols\n%s\n%s\n%s)\n%s\n  (sheet_instances (path "/" (page "1")))\n)\n'
       % (ROOT, pi_sym, pico_sym, jp_sym, '\n'.join('  ' + i for i in items)))
open('pidp8-pico-adapter.kicad_sch', 'w').write(sch)
subprocess.check_call(['kicad-cli', 'sch', 'export', 'pdf', '-o', 'pidp8-pico-adapter-schematic.pdf', 'pidp8-pico-adapter.kicad_sch'],
                      stdout=subprocess.DEVNULL)
subprocess.check_call(['kicad-cli', 'sch', 'export', 'svg', '-o', '.', 'pidp8-pico-adapter.kicad_sch'], stdout=subprocess.DEVNULL)
os.replace('pidp8-pico-adapter.svg', 'pidp8-pico-adapter-schematic.svg')
print('schematic written')
