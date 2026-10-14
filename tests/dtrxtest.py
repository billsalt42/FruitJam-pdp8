#!/usr/bin/env python3
"""TC08 DECtape and RX8E floppy: the same sessions on SIMH and on our core.

1. OS/8 from RK05 with DTA0 = a distribution tape, DTA1 and RXA0 blank:
   ZERO, COPY in both directions, DIR, TYPE, RUN from tape, PIP to floppy.
   Transcripts must match, and so must the resulting tape and floppy images,
   byte for byte.
2. OS/8 booted from a DECtape system tape (the TC08 bootstrap), with a
   BASIC program run from it.

Needs SIMH (SIMH_PDP8).  The tape used is sdcard/os8.tu56 (an OS/8 V3F TC08
system tape), as both the system tape and the data tape.
"""
import os, sys, shutil, difflib, pexpect

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
SIMH = os.environ.get('SIMH_PDP8', 'pdp8')
OURS = os.path.join(ROOT, 'build', 'pdp8host')
RK = os.path.join(ROOT, 'sdcard', 'os8.rk05')
DT_SYS = os.environ.get('DT_SYS', os.path.join(ROOT, 'sdcard', 'os8.tu56'))
DT_DATA = os.environ.get('DT_DATA', DT_SYS)
if not (os.path.exists(DT_SYS) and os.path.exists(DT_DATA)) or not shutil.which(SIMH):
    print('skip  needs SIMH (SIMH_PDP8) and sdcard/os8.tu56')
    sys.exit(0)
W = '/tmp/dtrxtest'
shutil.rmtree(W, ignore_errors=True)
os.makedirs(W)

TEXT = 'THE QUICK BROWN FOX\rJUMPS OVER THE LAZY DOG\r1234567890\r'
BAS = '10 FOR I=1 TO 6\r20 PRINT I;I*I;SQR(I)\r30 NEXT I\r40 END\r'

def media(who):
    d = os.path.join(W, who)
    os.makedirs(d)
    shutil.copy(RK, d + '/os8.rk05')
    shutil.copy(DT_DATA, d + '/data.tu56')
    shutil.copy(DT_SYS, d + '/sys.tu56')
    open(d + '/blank.tu56', 'wb').write(bytes(1474 * 129 * 2))
    open(d + '/blank.rx01', 'wb').write(bytes(77 * 26 * 128))
    return d

def start(who, d, boot, rx28=False):
    if who == 'simh':
        p = pexpect.spawn(SIMH, cwd=d, encoding='latin1', timeout=90)
        cmds = ['set cpu 32k', 'set df disabled', 'set tti ksr'] + (['set rx rx28'] if rx28 else [])
        if boot == 'rk':
            cmds += ['att rk0 os8.rk05', 'att dt0 data.tu56', 'att dt1 blank.tu56', 'att rx0 blank.rx01']
        else:
            cmds += ['att dt0 sys.tu56', 'att dt1 data.tu56']
        for c in cmds:
            p.expect('sim>'); p.sendline(c)
        p.expect('sim>'); p.sendline('boot ' + ('rk0' if boot == 'rk' else 'dt0'))
    else:
        args = ('os8.rk05 -dt 0 data.tu56 -dt 1 blank.tu56 -rx 0 blank.rx01' if boot == 'rk'
                else '-dt 0 sys.tu56 -dt 1 data.tu56 -bootdt')
        p = pexpect.spawn(OURS + (' -rx28 ' if rx28 else ' ') + args, cwd=d, encoding='latin1', timeout=90)
    p.expect(r'\n\.')
    return p

def finish(who, p):
    p.sendcontrol('e')
    if who == 'simh':
        p.expect('sim>'); p.sendline('det all'); p.expect('sim>')
        if os.environ.get('DTRX_DEBUG'): print('SIMH detach:', p.before)
        p.sendline('quit')
    p.expect(pexpect.EOF)

def session1(who):
    d = media(who)
    p = start(who, d, 'rk')
    log = []
    def step(send, expect=r'\n\.'):
        p.send(send); p.expect(expect); log.append(p.before + str(p.after))
    step('DIR DTA0:\r')
    step('ZERO DTA1:\r')
    step('COPY DTA1:<SYS:PIP.SV,FOTP.SV\r')
    step('COPY DTA1:<DTA0:HELP.HL,BASIC.SV,BRTS.SV,BCOMP.SV,BLOAD.SV\r')
    step('DIR DTA1:\r')
    step('R PIP\r', r'\*'); step('DTA1:FOX.TX<TTY:\r' + TEXT + '\x1a', r'\*'); step('\x03')
    step('TYPE DTA1:FOX.TX\r')
    step('ZERO RXA0:\r')
    step('COPY RXA0:<DTA1:*.TX,*.HL\r')
    step('COPY RXA0:<SYS:PIP.SV,DIRECT.SV\r')
    step('DIR RXA0:\r')
    step('TYPE RXA0:FOX.TX\r')
    step('RUN DTA1:PIP\r', r'\*'); step('RXA0:TWO.TX<DTA1:FOX.TX,FOX.TX\r', r'\*'); step('\x03')
    step('TYPE RXA0:TWO.TX\r')
    step('COPY DTA1:<RXA0:DIRECT.SV\r')
    step('DELETE DTA1:FOTP.SV\r')
    step('SQUISH DTA1:\r', r'(\?|\n\.)')
    if '?' in log[-1][-3:]: step('Y\r')
    p.expect([r'\n\.', pexpect.TIMEOUT], timeout=3)
    step('DIR DTA1:\r', r'FREE BLOCKS\s*\n\.')
    finish(who, p)
    return log, d

def session2(who):
    d = os.path.join(W, who)
    p = start(who, d, 'dt')
    log = []
    def step(send, expect=r'\n\.'):
        p.send(send); p.expect(expect); log.append(p.before + str(p.after))
    step('DIR\r')
    step('DIR DTA1:\r')
    step('R BASIC\r', 'NEW OR OLD--'); step('NEW\r', 'FILE NAME--'); step('SQ\r', 'READY')
    for l in BAS.split('\r')[:-1]: p.send(l + '\r')
    step('RUN\r', 'READY'); step('\x03')
    finish(who, p)
    return log

def norm(s):
    return [l.rstrip() for l in s.replace('\r', '').upper().split('\n')]

ok = True
def compare(name, a, b):
    global ok
    same = True
    for i, (x, y) in enumerate(zip(a, b)):
        if norm(x) != norm(y):
            same = ok = False
            print('=== %s: mismatch in step %d' % (name, i))
            print('\n'.join(difflib.unified_diff(norm(x), norm(y), 'simh', 'ours', lineterm='')))
    if len(a) != len(b): same = ok = False
    print(('PASS  ' if same else 'FAIL  ') + name)

a1, ds = session1('simh'); b1, do = session1('ours')
compare('RK05 OS/8 with DECtape and floppy: transcripts', a1, b1)
for f in ('blank.tu56', 'blank.rx01'):
    x = open(os.path.join(ds, f), 'rb').read(); y = open(os.path.join(do, f), 'rb').read()
    n = max(len(x), len(y)); x = x.ljust(n, b'\0'); y = y.ljust(n, b'\0')
    same = x == y
    if not same: ok = False
    print(('PASS  ' if same else 'FAIL  ') + 'written %s identical to SIMH\'s' % ('tape' if 'tu56' in f else 'floppy'))
a2 = session2('simh'); b2 = session2('ours')
compare('OS/8 booted from DECtape', a2, b2)
for x in b1 + b2: print(x.replace('\r', ''))
print('RESULT:', 'IDENTICAL' if ok else 'DIFFERENT')
sys.exit(0 if ok else 1)
