#!/usr/bin/env python3
"""ETOS V5B (Educational Data Systems' virtual-machine time-sharing) on the host build.

ETOS needs the TSC8-75 time-share board (-tsc).  The pack is not part of this
project; point ETOS_RK at a copy of etosv5b-demo.rk05 (from pdp8online.com).
The test works on a scratch copy.

Checks: OS/8 boots from the pack, R ETOS + option T starts the time-sharing,
the console logs in (0,4 USER1) and multiplexer line K01 (0,5 USER2), K01 runs
the BASIC golf game while the console's SYSTAT lists both jobs, the console
runs its own BASIC program, and LOGOUT signs K01 off.  Then over SSH: a login
gets K01, LOGOUT ends the SSH session, a second login is told the line is
busy, and dropping a session logs its job out for the next user.
"""
import os, re, sys, time, shutil, socket, pexpect

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
SRC = os.environ.get('ETOS_RK', os.path.join(ROOT, 'sdcard', 'etosv5b-demo.rk05'))
if not os.path.exists(SRC):
    print('skip  no ETOS pack (set ETOS_RK=/path/to/etosv5b-demo.rk05)')
    sys.exit(0)
WORK = '/tmp/etostest'
shutil.rmtree(WORK, ignore_errors=True)
os.makedirs(WORK)
shutil.copy(SRC, WORK + '/etos.rk05')
LPORT, SPORT, PW = 4661, 2461, 'pdp8pw'
failures = []

def check(cond, what):
    print(('PASS  ' if cond else 'FAIL  ') + what)
    sys.stdout.flush()
    if not cond: failures.append(what)

host = pexpect.spawn('%s -u -tsc -lines %d -ssh %d %s etos.rk05' % (os.path.join(ROOT, 'build', 'pdp8host'), LPORT, SPORT, PW),
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
out = con('DIR ETOS.SV\r')
check('ETOS  .SV' in out, 'OS/8 boots from the ETOS pack')
out = con('R ETOS\r', 4)
check('OPTION?' in out, 'ETOS starts and asks for an option')
con('T\r', 3)
out = con('\r')
check('LOGIN PLEASE' in out, 'time-sharing running (option T)')
con('LOGIN\r'); con('0,4\r'); out = con('USER1\r', 4)
check('LOGGED IN ON CONSOLE  0' in out and out.rstrip().endswith('.'), 'console logs in')

k = socket.create_connection(('127.0.0.1', LPORT)); k.settimeout(0.2)
k_rd = lambda: k.recv(65536).decode('latin1')
def k1(text, secs=2):
    for ch in text: k.send(ch.encode()); time.sleep(0.02)
    return drain(k_rd, secs)
time.sleep(0.5)
k1('\r'); k1('LOGIN\r'); k1('0,5\r'); out = k1('USER2\r', 4)
check('LOGGED IN ON CONSOLE  1' in out, 'line K01 logs in')
k1('R BASIC\r'); k1('OLD SYS:GOLF\r'); out = k1('RUN\r', 5)
check('EDUCOMP COUNTRY CLUB' in out, 'K01 runs BASIC golf')
con('R BASIC\r'); con('NEW\r'); con('SQ\r'); con('10 FOR I=1 TO 5\r20 PRINT I;I*I\r30 NEXT I\r40 END\r')
out = con('RUN\r', 4)
check(re.search(r'5\s+25', out) is not None, 'console runs its own BASIC program meanwhile')
con('BYE\r')
con('\x16S\r'); out = con('SYSTAT\r', 4)
check(re.search(r'\[00,04\]', out) is not None and re.search(r'\[00,05\] 01/\d+\s+BASIC', out) is not None,
      'SYSTAT on the console lists both jobs')
k1('\x03'); k1('\x03'); k1('\x16S\r'); out = k1('LOGOUT\r', 3)
check('LOGGED OUT ON CONSOLE  1' in out, 'LOGOUT signs K01 off')
k.close()
time.sleep(1)

SSH = ('ssh -p %d -o StrictHostKeyChecking=no -o UserKnownHostsFile=/dev/null '
       '-o PreferredAuthentications=password -o PubkeyAuthentication=no' % SPORT)
def ssh(user):
    s = pexpect.spawn('%s %s@127.0.0.1' % (SSH, user), encoding='latin1', timeout=20)
    s.expect('password:'); s.sendline(PW)
    return s
s1 = ssh('anyone')
i = s1.expect(['connected to line K01 of ETOS', 'in use', pexpect.EOF, pexpect.TIMEOUT])
check(i == 0, 'SSH login gets ETOS line K01')
s1_rd = lambda: s1.read_nonblocking(65536, timeout=0.2)
def sk(text, secs=2):
    for ch in text: s1.send(ch); time.sleep(0.02)
    return drain(s1_rd, secs)
sk('\r'); sk('LOGIN\r'); sk('0,5\r'); out = sk('USER2\r', 4)
out = sk('DIR\r', 4)
check('FREE BLOCKS' in out, 'ETOS session over SSH works')
sk('\x16S\r'); sk('LOGOUT\r', 0)
i = s1.expect(['closed', pexpect.EOF, pexpect.TIMEOUT], timeout=10)
check(i in (0, 1), 'LOGOUT ends the SSH session')
s1.close(force=True)
time.sleep(1)
s1 = ssh('k01')
i = s1.expect(['connected to line K01 of ETOS', pexpect.EOF, pexpect.TIMEOUT])
sk('\r'); sk('LOGIN\r'); sk('0,5\r'); out = sk('USER2\r', 4)
check('LOGGED IN ON CONSOLE  1' in out, 'logged in again over SSH')
sk('R BASIC\r'); sk('OLD SYS:GOLF\r'); sk('RUN\r', 3)
s2 = ssh('second')
i = s2.expect(['All ETOS terminal lines are in use', 'connected to', pexpect.EOF, pexpect.TIMEOUT])
check(i == 0, 'second SSH login told the line is busy')
s2.close(force=True)
s1.close(force=True)                                   # drop without logging out
time.sleep(6)
con('\x16S\r'); out = con('SYSTAT\r', 4)
ok = '[00,05]' not in out and '[00,04]' in out
check(ok, 'dropped SSH session was logged out')
if not ok: print('      got:', repr(out[-600:]))

host.sendcontrol('e')
host.close(force=True)
log = open(WORK + '/host.log', errors='replace').read()
check('HALT' not in log.replace('VM HLT', ''), 'no unexpected halts')
print('RESULT: %s' % ('PASS' if not failures else 'FAIL (%d)' % len(failures)))
sys.exit(1 if failures else 0)
