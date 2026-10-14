#!/usr/bin/env python3
"""MULTOS/8 (Computer Methods' time-sharing OS/8) on the host build.

The MULTOS/8 pack is not part of this project; point MULTOS_RK at a copy of
multos8.rk05 (from pdp8online.com).  The test works on a scratch copy.

Checks: OS/8 boots from the pack, DATE + R MULTOS + the time start the
time-sharing, the console and multiplexer line K01 both log on with Ctrl-H,
K01 sees its own disk (J2, with the BASIC games) and runs a BASIC program while
the console runs SYSTAT, which lists both jobs, and R LOGOFF signs K01 off.
Then over SSH: a login gets K01, a second one is told the lines are busy, and
R LOGOFF ends the SSH session, and dropping a
session logs its job off (Ctrl-C, R LOGOFF) for the next user.
"""
import os, re, sys, time, shutil, socket, pexpect

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
SRC = os.environ.get('MULTOS_RK', os.path.join(ROOT, 'sdcard', 'multos8.rk05'))
if not os.path.exists(SRC):
    print('skip  no MULTOS/8 pack (set MULTOS_RK=/path/to/multos8.rk05)')
    sys.exit(0)
WORK = '/tmp/multostest'
shutil.rmtree(WORK, ignore_errors=True)
os.makedirs(WORK)
shutil.copy(SRC, WORK + '/multos8.rk05')
LPORT, SPORT, PW = 4651, 2451, 'pdp8pw'
failures = []

def check(cond, what):
    print(('PASS  ' if cond else 'FAIL  ') + what)
    sys.stdout.flush()
    if not cond: failures.append(what)

host = pexpect.spawn('%s -u -lines %d -ssh %d %s multos8.rk05' % (os.path.join(ROOT, 'build', 'pdp8host'), LPORT, SPORT, PW),
                     cwd=WORK, encoding='latin1', timeout=20)
host.logfile_read = open(WORK + '/host.log', 'w')

def drain(rd, secs):
    out, end = '', time.time() + secs
    while time.time() < end:
        try: out += rd()
        except (pexpect.TIMEOUT, socket.timeout, BlockingIOError): pass
        except pexpect.EOF: break
    return out

con_rd = lambda: host.read_nonblocking(65536, timeout=0.2)
def con(text, secs=2):
    for ch in text: host.send(ch); time.sleep(0.02)
    return drain(con_rd, secs)

host.expect(r'host key')
drain(con_rd, 2)
out = con('DIR MULTOS.SV\r')
check('MULTOS.SV' in out, 'OS/8 boots from the MULTOS/8 pack')
# give job 2 its own copies of the MULTOS utilities, as the README suggests
out = con('COPY J2:<LOGOFF.SV,SYSTAT.SV,TIME.SV,TALK.SV\r', 3)
con('DATE 12-OCT-77\r')
out = con('R MULTOS\r', 3)
check('MULTI-USER OS/8 TIMESHARING' in out, 'MULTOS/8 starts and asks for the time')
out = con('1601\r', 3)
check('NOW  TIMESHARING' in out, 'time-sharing running')
con('\x08')
out = con('\r')
check(out.rstrip().endswith('.'), 'console logs on with Ctrl-H')

k = socket.create_connection(('127.0.0.1', LPORT)); k.settimeout(0.2)
k_rd = lambda: k.recv(65536).decode('latin1')
def k1(text, secs=2):
    for ch in text: k.send(ch.encode()); time.sleep(0.02)
    return drain(k_rd, secs)
k1('\x08'); out = k1('\r')
check(out.rstrip().endswith('.'), 'line K01 logs on with Ctrl-H')
out = k1('DIR\r', 4)
check('ADVENT.SV' in out and 'BASIC .SV' in out, 'K01 sees its own disk J2 (BASIC games)')
k1('R BASIC\r'); k1('NEW\r'); k1('SQ\r')
k1('10 FOR I=1 TO 5\r20 PRINT I;I*I\r30 NEXT I\r40 END\r', 2)
k.send(b'RUN\r')
out = con('R SYSTAT\r', 4)
check(re.search(r'SELF\s+1', out) is not None and re.search(r'\n\s*2\s+1\s', out.replace('\r', '')) is not None,
      'SYSTAT on the console lists both jobs')
out = drain(k_rd, 4)
check(re.search(r'5\s+25', out) is not None, 'BASIC program ran on K01 meanwhile')
k1('BYE\r', 3)
out = k1('R LOGOFF\r', 3)
check('LOGGED OFF' in out, 'R LOGOFF signs K01 off')
if 'LOGGED OFF' not in out: print('      got:', repr(out[-300:]))
k.close()
time.sleep(1)

SSH = ('ssh -p %d -o StrictHostKeyChecking=no -o UserKnownHostsFile=/dev/null '
       '-o PreferredAuthentications=password -o PubkeyAuthentication=no' % SPORT)
def ssh(user):
    s = pexpect.spawn('%s %s@127.0.0.1' % (SSH, user), encoding='latin1', timeout=20)
    s.expect('password:'); s.sendline(PW)
    return s
s1 = ssh('anyone')
i = s1.expect(['connected to line K01 of MULTOS/8', 'in use', pexpect.EOF, pexpect.TIMEOUT])
check(i == 0, 'SSH login gets MULTOS/8 line K01')
s1_rd = lambda: s1.read_nonblocking(65536, timeout=0.2)
def sk(text, secs=2):
    for ch in text: s1.send(ch); time.sleep(0.02)
    return drain(s1_rd, secs)
sk('\x08'); out = sk('\r')
out = sk('DIR *.FC\r', 4)
check('BARON .FC' in out, 'MULTOS/8 session over SSH works')
sk('R LOGOFF\r', 0)
i = s1.expect(['closed', pexpect.EOF, pexpect.TIMEOUT], timeout=10)
check(i in (0, 1), 'R LOGOFF ends the SSH session')
s1.close(force=True)
time.sleep(1)
s1 = ssh('k01')
i = s1.expect(['connected to line K01 of MULTOS/8', pexpect.EOF, pexpect.TIMEOUT])
sk('\x08'); out = sk('\r')
check(out.rstrip().endswith('.'), 'logged on again over SSH')
sk('R BASIC\r'); sk('OLD\r'); sk('HELLO\r', 3)
s2 = ssh('second')
i = s2.expect(['All MULTOS/8 terminal lines are in use', 'connected to', pexpect.EOF, pexpect.TIMEOUT])
check(i == 0, 'second SSH login told the line is busy')
s2.close(force=True)
s1.close(force=True)                                   # drop without logging off
time.sleep(5)
out = con('R SYSTAT\r', 4)
ok = re.search(r'\n\s*2\s+1\s', out.replace('\r', '')) is None
check(ok, 'dropped SSH session was logged off')
if not ok: print('      got:', repr(out[-500:]))

host.sendcontrol('e')
host.close(force=True)
log = open(WORK + '/host.log', errors='replace').read()
check('HALT' not in log, 'no unexpected halts')
print('RESULT: %s' % ('PASS' if not failures else 'FAIL (%d)' % len(failures)))
sys.exit(1 if failures else 0)
