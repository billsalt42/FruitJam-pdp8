#!/usr/bin/env python3
"""Check a PiDP-8/I panel interface from a computer (pip install pyserial).

    python3 paneltest.py /dev/ttyACM0        (Windows: COM5, macOS: /dev/cu.usbmodem...)

Prints the switches as you flip them and lights lamps to match:
  PC row = switch register, MA row = DF/IF switches, MB row = the momentary
  switches, AC row = a brightness ramp, and a lamp walks across MQ and the
  rows below it.  Ctrl-C to stop.
"""
import sys, time
import serial

LAMPS, HELLO, SWITCH = 0x81, 0x90, 0x91
NAMES2 = {11: 'START', 10: 'LOAD ADD', 9: 'DEP', 8: 'EXAM', 7: 'CONT', 6: 'STOP',
          5: 'SING STEP', 4: 'SING INST'}

def main():
    if len(sys.argv) != 2:
        sys.exit(__doc__)
    port = serial.Serial(sys.argv[1], 115200, timeout=0)
    port.dtr = True
    sw, last, rx, t0 = [0, 0, 0], None, [], time.time()
    while True:
        for b in port.read(256):
            if b & 0x80:
                rx = [b]
            elif rx:
                rx.append(b)
                if rx[0] == HELLO and len(rx) == 6:
                    print('panel interface: protocol %d, firmware %d.%d' % tuple(rx[3:6]))
                    rx = []
                elif rx[0] == SWITCH and len(rx) == 7:
                    sw = [rx[1 + 2 * r] | (rx[2 + 2 * r] << 6) for r in range(3)]
                    rx = []
        if sw != last:
            last = list(sw)
            keys = [n for bit, n in NAMES2.items() if sw[2] >> bit & 1]
            print('SR %04o   DF %o IF %o   %s' % (sw[0], sw[1] >> 9 & 7, sw[1] >> 6 & 7,
                                               ' '.join(keys) or '-'))
        lv = [[0] * 12 for _ in range(8)]
        for r in range(3):
            for c in range(12):
                lv[r][c] = 31 if sw[r] >> c & 1 else 0
        for c in range(12):
            lv[3][c] = c * 31 // 11
        p = int((time.time() - t0) * 12) % 48
        lv[4 + p // 12][11 - p % 12] = 31
        port.write(bytes([LAMPS] + [v for row in lv for v in row]))
        time.sleep(0.02)

if __name__ == '__main__':
    try:
        main()
    except KeyboardInterrupt:
        pass
