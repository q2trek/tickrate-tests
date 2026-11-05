#!/usr/bin/env python3
"""status_n.py PORT RCON_PASSWORD RATE: the fork server publishes sv_fps in serverinfo (OOB `status`) and answers
rcon `status n` with the pacing table and the recommended client packet rate (>= RATE). A bare protocol 34 connect
puts one client in the table (status lists no table for an empty server)."""
import socket
import sys
import time

port, pw, rate = int(sys.argv[1]), sys.argv[2], int(sys.argv[3])


def oob(text, s=None):
    s = s or socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    s.settimeout(1.0)
    for _ in range(20):
        s.sendto(b'\xff\xff\xff\xff' + text.encode(), ('127.0.0.1', port))
        try:
            return s.recv(65536)[4:].decode('latin-1')
        except socket.timeout:
            time.sleep(0.5)
    raise SystemExit(f'FAIL no answer to {text!r}')


info = oob('status').split('\n')[1].split('\\')[1:]
info = dict(zip(info[::2], info[1::2]))
print('serverinfo sv_fps =', info.get('sv_fps'))
peer = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
challenge = oob('getchallenge', peer).split()[1]
print(oob(f'connect 34 1 {challenge} "\\name\\pacer"', peer).strip())
out = oob(f'rcon {pw} status n')
print(out)
ok = info.get('sv_fps') == str(rate)
ok &= 'num name              pps  cmds   dup   rej held%' in out
ok &= ' pacer ' in out
ok &= f'recommended client packet rate: >= {rate}' in out
print('PASS' if ok else 'FAIL')
sys.exit(0 if ok else 1)
