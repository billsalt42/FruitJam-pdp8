#!/usr/bin/env python3
"""SSH terminal sessions on the host build, with real SSH clients.

Runs DEC TSS/8.24 with `pdp8host -ssh` and checks, using OpenSSH (and
asyncssh / Dropbear's dbclient when installed):
  * the host key fingerprint the server reports matches what clients see
  * wrong passwords are refused, and the third failure ends the connection
  * four sessions get K01-K04, log in and run BASIC at the same time
  * a fifth automatic session is turned away (all lines busy)
  * user k00 gets the console; an explicit busy line is refused
  * a long paste is flow-controlled into TSS/8 without loss
  * the session survives forced re-keying (RekeyLimit)
  * a line is free again after its session disconnects
"""
import os, re, sys, time, shutil, subprocess, pexpect, asyncio

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
SD = os.path.join(ROOT, 'sdcard')
PORT, RAWPORT, PW = 2422, 4622, 'fruitjam8'
WORK = '/tmp/sshtest'
shutil.rmtree(WORK, ignore_errors=True)
os.makedirs(WORK)
shutil.copy(os.path.join(SD, 'tss8_rf.dsk'), WORK + '/rf.dsk')
failures = []

def check(cond, what):
    print(('PASS  ' if cond else 'FAIL  ') + what)
    sys.stdout.flush()
    if not cond: failures.append(what)

BIN = os.path.join(ROOT, os.environ.get('PDP8HOST', 'build/pdp8host'))
NINA = 'nina' in BIN
print('server: %s%s' % (os.path.basename(BIN), ' (SSH via net/nina.c and a simulated ESP32)' if NINA else ''))
host = pexpect.spawn('%s -ssh %d %s -tss8 rf.dsk %s %d' % (BIN, PORT, PW,
                     os.path.join(SD, 'tss8_init.bin'), RAWPORT), cwd=WORK, encoding='latin1', timeout=20)
host.logfile_read = open(WORK + '/host.log', 'w')
host.expect(r'host key (SHA256:\S+)')
fingerprint = host.match.group(1)
host.expect(r'LOAD, DUMP, START, ETC\?')
for s in ('START\r', '9-25-84\r', '09:35\r'):
    host.send(s); time.sleep(0.8)
if NINA:
    host.expect(r'\[WiFi "testnet": ssh -p %d 127\.0\.0\.1\]' % PORT)
    check(True, 'simulated ESP32: nina-fw started, joined "testnet", SSH listener up')

SSH = ('ssh -p %d -o StrictHostKeyChecking=no -o UserKnownHostsFile=/dev/null '
       '-o PreferredAuthentications=password -o PubkeyAuthentication=no' % PORT)

def ssh(user, extra='', password=PW, verbose=False):
    s = pexpect.spawn('%s %s %s %s@127.0.0.1' % (SSH, '-v' if verbose else '', extra, user),
                      encoding='latin1', timeout=20)
    s.expect('password:')
    s.sendline(password)
    return s

def typ(s, text, delay=0.02):
    for ch in text:
        s.send(ch); time.sleep(delay)

def drain(s, secs):
    out, end = '', time.time() + secs
    while time.time() < end:
        try: out += s.read_nonblocking(8192, timeout=0.1)
        except pexpect.TIMEOUT: pass
        except pexpect.EOF: break
    return out

# --- host key fingerprint
scan = subprocess.run('ssh-keyscan -p %d -t ed25519 127.0.0.1 2>/dev/null | ssh-keygen -lf -' % PORT,
                      shell=True, capture_output=True, text=True).stdout
check(fingerprint in scan, 'fingerprint printed by the server matches ssh-keygen (%s)' % fingerprint)
time.sleep(0.5)

# --- wrong passwords
b = pexpect.spawn('%s -o NumberOfPasswordPrompts=5 nobody@127.0.0.1' % SSH, encoding='latin1', timeout=20)
for _ in range(3):
    b.expect('password:'); b.sendline('wrong')
i = b.expect(['password:', 'Permission denied', 'Disconnected', 'closed', pexpect.EOF])
check(i != 0, 'connection dropped after three wrong passwords')
b.close(force=True)

# --- four sessions, one per multiplexer line
sess = []
for n in range(4):
    s = ssh('user%d' % n)
    s.expect(r'connected to line (K0\d)')
    sess.append((s, s.match.group(1)))
check(sorted(k for _, k in sess) == ['K01', 'K02', 'K03', 'K04'], 'four SSH sessions get K01-K04')

f = ssh('fifth')
i = f.expect(['in use', 'connected to', pexpect.EOF])
check(i == 0, 'fifth automatic session refused: all lines in use')
if i != 0: print('      got:', repr(f.before[-300:]))
f.close(force=True)

prog = '10 FOR I=1 TO 2000\r20 LET X=SIN(I)\r30 NEXT I\r40 PRINT "DONE";X\r50 END\r'
outs = [''] * 4
for i, (s, k) in enumerate(sess):
    for cmd in ('\r', 'LOGIN 2 LXHE\r', 'R BASIC\r', 'NEW\r', 'P%d\r' % i, prog):
        typ(s, cmd)
        outs[i] += drain(s, 0.4)
for i, (s, k) in enumerate(sess):
    check(re.search(r'JOB \d+\s+\[00,02\]\s+%s' % k, outs[i]) is not None, 'TSS/8 login over SSH on %s' % k)
for s, k in sess: typ(s, 'RUN\r')
end = time.time() + 60
while time.time() < end and not all(re.search(r'DONE\s*-?\.\d', o) for o in outs):
    for i, (s, k) in enumerate(sess): outs[i] += drain(s, 0.05)
for i, (s, k) in enumerate(sess):
    check(re.search(r'DONE\s*\.930\d', outs[i]) is not None, 'BASIC ran on %s' % k)

# --- explicit lines
x = ssh('k02')
i = x.expect(['already in use', 'connected to', pexpect.EOF])
check(i == 0, 'explicit busy line K02 refused')
x.close(force=True)
time.sleep(1)
c = ssh('k00')
c.expect(r'connected to the console')
typ(c, '\r'); time.sleep(0.5)
typ(c, 'LOGIN 2 LXHE\r')
out = drain(c, 2)
check(re.search(r'JOB \d+\s+\[00,02\]\s+K00', out) is not None, 'user k00 gets the TSS/8 console')
typ(c, 'SYSTAT\r')
out = drain(c, 3)
check(all(('K0%d' % n) in out for n in range(5)), 'SYSTAT on the console lists K00-K04')

# --- long paste: flow control into the 32-character line buffer
s, k = sess[0]
typ(s, 'BYE\r'); drain(s, 1)
typ(s, 'R BASIC\r'); drain(s, 1); typ(s, 'NEW\r'); drain(s, 0.5); typ(s, 'PASTE\r'); drain(s, 1)
lines = ['%d PRINT "LINE %d OF A LONG PASTED PROGRAM";%d' % (10 * n, n, n * n) for n in range(1, 41)]
lines.append('999 END')
s.send('\r'.join(lines) + '\r')                          # ~1.7 KB in one go
drain(s, 12)
typ(s, 'RUN\r')
out = drain(s, 12)
got = re.findall(r'LINE (\d+) OF A LONG PASTED PROGRAM\s+(\d+)', out)
check(len(got) == 40 and all(int(a) ** 2 == int(b) for a, b in got),
      'pasted 40-line program arrived intact (%d lines ran)' % len(got))

# --- LOGOUT hangs up; a dropped session's job is logged out for it
typ(s, 'BYE\r'); drain(s, 1.5)
typ(s, 'LOGOUT\r')
i = s.expect(['Connection to 127.0.0.1 closed', pexpect.EOF, pexpect.TIMEOUT], timeout=10)
check(i in (0, 1), 'TSS/8 LOGOUT ends the SSH session')
for t, _ in sess[1:]: t.close(force=True)              # K02-K04 just drop, still logged in
c.close(force=True)
time.sleep(5)
x = ssh('k02')
x.expect('connected to line K02')
typ(x, '\r'); time.sleep(0.5); typ(x, 'LOGIN 2 LXHE\r')
out = drain(x, 2)
check(re.search(r'JOB \d+\s+\[00,02\]\s+K02', out) is not None, 'dropped session was logged out: fresh LOGIN on K02')
typ(x, 'LOGOUT\r'); x.expect(['closed', pexpect.EOF], timeout=10); x.close(force=True)

# --- re-keying
time.sleep(1)
r = ssh('k03', extra='-o RekeyLimit=2K -vv -E %s/rekey.log' % WORK)
r.expect('connected to line K03')
check(True, 'line K03 free again after its session ended')
typ(r, '\r'); typ(r, 'LOGIN 2 LXHE\r'); drain(r, 1.5)
typ(r, 'R CAT\r')
out = drain(r, 4)
typ(r, 'SYSTAT\r')
out += drain(r, 4)
n_rekey = open(WORK + '/rekey.log').read().count('SSH2_MSG_NEWKEYS received')
check('BASIC' in out and 'SYSTAT' in out and n_rekey >= 2, 'session survives forced re-keying (%d key exchanges)' % n_rekey)
drain(r, 2)
typ(r, 'LOGOUT\r'); r.expect(['closed', pexpect.EOF], timeout=10)
r.close(force=True)

# --- other client implementations
async def try_asyncssh():
    import asyncssh
    async with asyncssh.connect('127.0.0.1', PORT, username='k01', password=PW, known_hosts=None) as conn:
        proc = await conn.create_process(term_type='vt100')
        out = ''
        end = time.time() + 5
        while 'connected to' not in out and time.time() < end:
            out += await asyncio.wait_for(proc.stdout.read(1024), 5)
        proc.stdin.write('\r')
        await asyncio.sleep(0.8)
        proc.stdin.write('LOGIN 2 LXHE\r')
        end = time.time() + 6
        while not re.search(r'JOB.*K01', out) and time.time() < end:
            try: out += await asyncio.wait_for(proc.stdout.read(1024), 1)
            except asyncio.TimeoutError: pass
        proc.close()
        return out
try:
    import asyncssh  # noqa
    out = asyncio.run(try_asyncssh())
    ok = re.search(r'JOB \d+\s+\[00,02\]\s+K01', out) is not None
    check(ok, 'asyncssh client logs in on K01')
    if not ok: print('      got:', repr(out[-300:]))
except ImportError:
    print('skip  asyncssh not installed')

if shutil.which('dbclient'):
    time.sleep(1)
    d = pexpect.spawn('dbclient -y -p %d k04@127.0.0.1' % PORT, encoding='latin1', timeout=20,
                      env=dict(os.environ, HOME=WORK))
    d.expect('assword')
    d.sendline(PW)
    i = d.expect(['connected to line K04', pexpect.EOF, pexpect.TIMEOUT])
    if i == 0:
        typ(d, '\r'); time.sleep(0.8); typ(d, 'LOGIN 2 LXHE\r')
        out = drain(d, 3)
        ok = re.search(r'JOB \d+\s+\[00,02\]\s+K04', out) is not None
        check(ok, 'Dropbear dbclient logs in on K04')
        if not ok: print('      got:', repr(out[-300:]))
    else:
        check(False, 'Dropbear dbclient connects')
        print('      got:', repr(d.before[-300:]))
    d.close(force=True)
else:
    print('skip  dbclient not installed')

# --- junk on the port must not upset the server
import socket, random
random.seed(8)
for trial in range(30):
    try:
        k = socket.create_connection(('127.0.0.1', PORT), timeout=2)
        k.recv(64)
        if trial % 3 == 0: k.send(b'SSH-2.0-junk\r\n')
        k.send(bytes(random.getrandbits(8) for _ in range(random.randint(1, 3000))))
        time.sleep(0.05)
        k.close()
    except OSError:
        pass
time.sleep(1)
j = ssh('k02')
i = j.expect(['connected to line K02', pexpect.EOF, pexpect.TIMEOUT])
check(i == 0, 'still accepting logins after 30 connections of random junk')
j.close(force=True)
host.sendcontrol('e')
host.close(force=True)

# --- OS/8: SSH gets the console
shutil.copy(os.path.join(SD, 'os8.rk05'), WORK + '/os8.rk05')
host = pexpect.spawn('%s -ssh %d %s os8.rk05' % (BIN, PORT, PW), cwd=WORK, encoding='latin1', timeout=20)
host.logfile_read = open(WORK + '/host2.log', 'w')
host.expect(r'\.')
if NINA: host.expect(r'ssh -p'); time.sleep(0.5)
o = ssh('anyone')
i = o.expect(['connected to the console', pexpect.EOF, pexpect.TIMEOUT])
check(i == 0, 'under OS/8 an SSH login gets the console')
typ(o, 'DIR SYS:*.SV\r')
out = drain(o, 4)
check('PIP' in out and 'FREE BLOCKS' in out, 'OS/8 DIR over SSH')
o.close(force=True)
host.sendcontrol('e')
host.close(force=True)
for log in ('host.log', 'host2.log'):
    t = open(os.path.join(WORK, log), errors='replace').read()
    check('runtime error' not in t and 'AddressSanitizer' not in t and 'FAKE ESP' not in t,
          'no sanitizer or simulated-ESP complaints in %s' % log)
print('RESULT: %s' % ('PASS' if not failures else 'FAIL (%d)' % len(failures)))
sys.exit(1 if failures else 0)
