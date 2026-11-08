#!/usr/bin/env python3
"""Native crypto on the board: boot the crypto test guest (guest/out-crypto,
guest/tests/build-board-crypto.sh) with `pico-shell.sh -C`, log in on the
USB console and run the on-device tests. Results (JSON) and the console
transcript go to target/pico-crypto/.

  scripts/pico-crypto-test.py vectors            ABI, vectors (repeated), op cycle costs
  scripts/pico-crypto-test.py ssh                SSH integration: on, off, client, server
  scripts/pico-crypto-test.py connection [-n N]  N fresh boots each, native on/off
  scripts/pico-crypto-test.py responsive         ping + console latency during native ops
  scripts/pico-crypto-test.py remote             OpenSSH on the Mac -> Dropbear on the board
"""
import argparse
import json
import os
import re
import select
import subprocess
import sys
import termios
import time
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
OUT = ROOT / 'target/pico-crypto'
GUEST = ROOT.parent / 'guest/out-crypto'
VECTORS = '/usr/share/rvemu/crypto-vectors.bin'
PROMPT = 'RVPROMPT# '


class Console:
    def __init__(self, port, log):
        self.fd = os.open(port, os.O_RDWR | os.O_NOCTTY | os.O_NONBLOCK)
        a = termios.tcgetattr(self.fd)
        a[0] = 0; a[1] = 0; a[2] = termios.CS8 | termios.CREAD | termios.CLOCAL; a[3] = 0
        a[4] = a[5] = termios.B115200
        termios.tcsetattr(self.fd, termios.TCSANOW, a)
        self.buf = ''
        self.all = ''  # everything read, for firmware output that arrives asynchronously
        self.log = log

    def close(self):
        os.close(self.fd)

    def write(self, s):
        data = s.encode()
        while data:
            try:
                n = os.write(self.fd, data[:64])
                data = data[n:]
            except BlockingIOError:
                time.sleep(0.01)
            time.sleep(0.002)

    def read_some(self, timeout):
        r, _, _ = select.select([self.fd], [], [], timeout)
        if not r:
            return ''
        try:
            d = os.read(self.fd, 65536).decode(errors='replace')
        except BlockingIOError:
            return ''
        self.log.write(d)
        self.log.flush()
        self.buf += d
        self.all += d
        return d

    def expect(self, pattern, timeout):
        rx = re.compile(pattern)
        end = time.monotonic() + timeout
        while True:
            m = rx.search(self.buf)
            if m:
                out = self.buf[:m.end()]
                self.buf = self.buf[m.end():]
                return out, m
            left = end - time.monotonic()
            if left <= 0:
                raise TimeoutError(f'waiting for {pattern!r}; last output: {self.buf[-2000:]!r}')
            self.read_some(min(left, 0.5))

    def drain(self, quiet=0.5):
        while self.read_some(quiet):
            pass
        self.buf = ''

    def run(self, cmd, timeout=120):
        """Run one shell command; its output and exit status."""
        tag = f'E{time.monotonic_ns() % 10**9}'
        self.buf = ''
        self.write(f"{cmd}; echo {tag}=$?\n")
        out, m = self.expect(rf'{tag}=(\d+)\r?\n', timeout)
        # echo is off: everything before the tag is output (less prompts)
        out = out[:out.rfind(tag)].replace(PROMPT, '')
        self.expect(re.escape(PROMPT), 10)
        return out.replace('\r', ''), int(m[1])


def shell_port():
    ports = sorted(Path('/dev').glob('cu.usbmodemrvemu*'))
    return str(ports[0]) if ports else None


def boot(log):
    """(Re)boot the board into the crypto guest; a logged-in console."""
    r = subprocess.run(['bash', 'scripts/pico-shell.sh', '-C', '-d', str(GUEST)], cwd=ROOT, capture_output=True, text=True)
    log.write(f'\n=== pico-shell.sh -C -d {GUEST}\n{r.stdout}{r.stderr}\n')
    if r.returncode:
        raise SystemExit(f'pico-shell.sh failed:\n{r.stdout}{r.stderr}')
    t0 = time.monotonic()
    port = None
    while time.monotonic() - t0 < 20 and not port:
        port = shell_port()
        time.sleep(0.2)
    if not port:
        raise SystemExit('console port did not appear')
    c = Console(port, log)
    # the console replays the boot log; wait for the login prompt
    for _ in range(40):
        c.write('\n')
        try:
            c.expect(r'login: ', 3)
            break
        except TimeoutError:
            continue
    else:
        raise SystemExit('no login prompt')
    boot_s = time.monotonic() - t0
    c.write('root\n')
    time.sleep(1)
    c.write(f"PS1='{PROMPT}'; stty -echo 2>/dev/null\n")
    c.expect(re.escape(PROMPT), 20)
    c.drain()
    # the first command also returns what was echoed before stty -echo
    c.run('true')
    return c, boot_s


POINT = re.compile(r'benchmark point tag=(\d+) guest=(\d+) host=(\d+) ns=(\d+) idle=(\d+) native=\[([^\]]*)\]')
BOARD = re.compile(r'benchmark board tag=(\d+) cycles=(\d+) host_instret=(\d+) xip_hit=(\d+) xip_acc=(\d+)')


def parse_report(text):
    pts = []
    for m, b in zip(POINT.finditer(text), BOARD.finditer(text)):
        pts.append(dict(tag=int(m[1]), guest=int(m[2]), cycles=int(b[2]), instret=int(b[3]), idle=int(m[5]),
                        xip_hit=int(b[4]), xip_acc=int(b[5]), native=[int(x) for x in m[6].split(',')]))
    hz = re.search(r'benchmark board sys_hz=(\d+)', text)
    calls = re.search(r'native buffer calls \[([^\]]*)\]', text)
    pre = re.search(r'native preemptions (\d+)', text)
    aborts = re.search(r'native aborts (\d+)', text)
    peaks = re.search(r'native stack peaks \[([^\]]*)\] of (\d+) B', text)
    erased = re.search(r'native stack erased (true|false)', text)
    stack = re.search(r'firmware stack use: core0 (\d+) of (\d+) B, core1 (\d+) of (\d+) B', text)
    return dict(points=pts, sys_hz=int(hz[1]) if hz else None,
                native_calls=[int(x) for x in calls[1].split(',')] if calls else None,
                native_preemptions=int(pre[1]) if pre else None,
                native_aborts=int(aborts[1]) if aborts else None,
                native_stack_peaks=[int(x) for x in peaks[1].split(',')] if peaks else None,
                native_stack_bytes=int(peaks[2]) if peaks else None,
                native_stack_erased=erased[1] == 'true' if erased else None,
                core0_stack=[int(stack[1]), int(stack[2])] if stack else None,
                core1_stack=[int(stack[3]), int(stack[4])] if stack else None)


def report(c):
    out, rc = c.run('rvemu-bench-report', 30)
    assert rc == 0, out
    return parse_report(out), out


def interval(a, b, hz):
    return dict(cycles=b['cycles'] - a['cycles'], ms=(b['cycles'] - a['cycles']) * 1000 / hz,
                core0_instret=b['instret'] - a['instret'], guest_instructions=b['guest'] - a['guest'],
                guest_idle_ticks=b['idle'] - a['idle'], xip_accesses=b['xip_acc'] - a['xip_acc'],
                xip_hits=b['xip_hit'] - a['xip_hit'],
                native_calls=[y - x for x, y in zip(a['native'], b['native'])])


def check(cond, what, results):
    results.setdefault('checks', []).append(dict(check=what, passed=bool(cond)))
    print(('PASS ' if cond else 'FAIL ') + what, flush=True)
    return cond


def save(name, results):
    OUT.mkdir(parents=True, exist_ok=True)
    (OUT / f'{name}.json').write_text(json.dumps(results, indent=2) + '\n')
    print(f'results: {OUT / (name + ".json")}')


def cmd_vectors(a, log):
    c, boot_s = boot(log)
    res = dict(boot_to_login_s=round(boot_s, 1))
    out, _ = c.run('ls /dev/rvemu-crypto /dev/rvemu-ssh /dev/rvemu-bench; dmesg | grep -i -E "rvemu|Memory:|psram" | head -20')
    res['devices'] = out
    check(all(d in out for d in ['/dev/rvemu-crypto', '/dev/rvemu-ssh', '/dev/rvemu-bench']), 'adapter devices registered', res)
    out, _ = c.run('free; cat /proc/meminfo | head -2')
    res['meminfo'] = out
    rep, _ = report(c)
    res['before'] = rep
    out, rc = c.run('crypto-ioctl-test')
    check(rc == 0 and 'GENERIC CRYPTO ABI PASS' in out, 'crypto-ioctl-test (ABI, RFC vectors, malformed requests, rejection)', res)
    out, rc = c.run('ssh-ioctl-test')
    check(rc == 0 and 'SSH IOCTL BOUNDS AND AUTH PASS' in out, 'ssh-ioctl-test (packet ABI bounds, authentication failure)', res)
    out, rc = c.run(f'crypto-vectors {VECTORS} {a.repeats}', 1800)
    res['vectors'] = out.strip()
    check(rc == 0 and 'CRYPTO VECTORS PASS' in out, f'crypto-vectors x{a.repeats} (workspace reuse across ops, sizes, failures)', res)
    rep, raw = report(c)
    res['after_vectors'] = rep
    peaks = rep['native_stack_peaks']
    check(peaks and all(0 < p <= 20 * 1024 for p in peaks), 'every native op used its private stack, peak <= 20 KiB of 24 KiB', res)
    check(rep['core0_stack'] and rep['core0_stack'][0] < rep['core0_stack'][1], 'core0 ordinary stack within its region', res)
    check(rep['native_stack_erased'] is True, 'native execution stack fully erased between calls', res)
    # per-operation cycle cost through the whole path (ioctl, kernel copies, SBI, workspace)
    ops = {}
    out, rc = c.run(f'crypto-vectors {VECTORS} --bench {a.calls}', 1800)
    for m in re.finditer(r'BENCH CASE (\S+) op=(\d+) calls=(\d+)(.*?)(?=BENCH CASE|BENCH DONE)', out, re.S):
        r = parse_report(m[4])
        if len(r['points']) == 2:
            iv = interval(*r['points'], r['sys_hz'])
            ops[m[1]] = dict(op=int(m[2]), calls=int(m[3]), cycles_per_call=iv['cycles'] // int(m[3]),
                             us_per_call=round(iv['ms'] * 1000 / int(m[3]), 1),
                             core0_instret_per_call=iv['core0_instret'] // int(m[3]),
                             xip_miss_rate=round(1 - iv['xip_hits'] / iv['xip_accesses'], 4) if iv['xip_accesses'] else None)
    out, rc = c.run(f'crypto-vectors {VECTORS} --bench {a.calls} unknown-operation', 600)
    m = re.search(r'BENCH CASE (\S+) op=(\d+) calls=(\d+)(.*?)BENCH DONE', out, re.S)
    if m:
        r = parse_report(m[4])
        iv = interval(*r['points'], r['sys_hz'])
        ops['unknown-operation (ioctl + SBI, no workspace)'] = dict(calls=int(m[3]), cycles_per_call=iv['cycles'] // int(m[3]),
                                                                    us_per_call=round(iv['ms'] * 1000 / int(m[3]), 1))
    res['op_costs'] = ops
    check(len(ops) > 20, 'per-operation cycle costs measured', res)
    res['native_preemptions'] = rep['native_preemptions']
    rep, _ = report(c)
    res['final'] = rep
    c.close()
    save('vectors', res)
    return all(x['passed'] for x in res['checks'])


def cmd_ssh(a, log):
    c, boot_s = boot(log)
    res = dict(boot_to_login_s=round(boot_s, 1), runs={})
    for mode in a.modes:
        r0, _ = report(c)
        t0 = time.monotonic()
        out, rc = c.run(f'ssh-board-test integration {mode} {a.bulk}', 3600)
        dt = time.monotonic() - t0
        r1, _ = report(c)
        calls = [y - x for x, y in zip(r0['native_calls'], r1['native_calls'])]
        res['runs'][mode] = dict(rc=rc, wall_s=round(dt, 1), native_calls=calls, output=out.strip()[-3000:],
                                 native_stack_peaks=r1['native_stack_peaks'], core0_stack=r1['core0_stack'], core1_stack=r1['core1_stack'])
        ok = rc == 0 and all(s in out for s in ['SSH AUTH PASS', 'SSH EXIT STATUS PASS', 'SSH TRANSFER PASS 65536',
                                                    f'SSH STREAM PASS {a.bulk} MiB', 'SSH WRONG KEY REJECTED', 'SSH TEST PASS'])
        check(ok, f'ssh integration {mode}: auth, exit 7, 64 KiB each way, {a.bulk} MiB streamed each way, wrong key rejected', res)
        if mode == 'off':
            check(not any(calls), 'ssh off: no native calls', res)
        else:
            check(calls[4] > 10 and calls[5] > 10, f'ssh {mode}: native packet seal/open used', res)
            need = {6, 8, 9, 10, 11, 12} - ({11} if mode == 'client' else set()) - ({10, 12} if mode == 'server' else set())
            check(all(calls[o] > 0 for o in need), f'ssh {mode}: native handshake primitives used', res)
    c.close()
    save('ssh', res)
    return all(x['passed'] for x in res['checks'])


def cmd_connection(a, log):
    res = dict(samples=[])
    order = [m for _ in range(a.n) for m in ('on', 'off')]
    for i, mode in enumerate(order):
        c, boot_s = boot(log)
        # same quiet state for both: wait for the boot's background work
        time.sleep(a.settle)
        out, rc = c.run(f'ssh-board-test connection {mode}', 1800)
        r = parse_report(out)
        kex = re.search(r'SSH CONNECTION KEX (\S+)', out)
        s = dict(mode=mode, boot=i, rc=rc, kex=kex[1] if kex else None, sys_hz=r['sys_hz'],
                 native_stack_peaks=r['native_stack_peaks'], core0_stack=r['core0_stack'], core1_stack=r['core1_stack'])
        p = r['points']
        if rc == 0 and [x['tag'] for x in p] == [0, 1, 2, 3]:
            s['setup_and_key_exchange'] = interval(p[0], p[2], r['sys_hz'])
            s['authentication'] = interval(p[2], p[3], r['sys_hz'])
            s['connection'] = interval(p[0], p[3], r['sys_hz'])
        else:
            s['error'] = out[-3000:]
        res['samples'].append(s)
        print(json.dumps({k: v for k, v in s.items() if k in ('mode', 'rc', 'kex')} | {'ms': s.get('connection', {}).get('ms')}), flush=True)
        c.close()
    for mode in ('on', 'off'):
        ok = [s for s in res['samples'] if s['mode'] == mode and 'connection' in s]
        check(len(ok) == a.n, f'connection {mode}: {a.n} fresh-boot samples', res)
        for s in ok:
            calls = s['connection']['native_calls']
            if mode == 'off':
                check(not any(calls), f'connection off boot {s["boot"]}: no native calls in window', res)
            else:
                check(all(calls[o] > 0 for o in (4, 5, 6, 8, 9, 10, 11, 12)), f'connection on boot {s["boot"]}: native handshake + packets in window', res)
    kexes = {s['kex'] for s in res['samples']}
    check(len(kexes) == 1 and None not in kexes, f'same negotiated key exchange in every sample: {kexes}', res)
    save('connection', res)
    return all(x['passed'] for x in res['checks'])


def board_ip(c):
    for _ in range(30):
        out, _ = c.run('ip -4 addr show eth0')
        m = re.search(r'inet (\d+\.\d+\.\d+\.\d+)', out)
        if m:
            return m[1]
        time.sleep(1)
    return None


def ping_stats(ip, count, interval=0.2):
    r = subprocess.run(['ping', '-c', str(count), '-i', str(interval), ip], capture_output=True, text=True)
    m = re.search(r'(\d+) packets transmitted, (\d+) packets received.*?\n.*?= ([\d.]+)/([\d.]+)/([\d.]+)', r.stdout, re.S)
    return dict(sent=int(m[1]), received=int(m[2]), min_ms=float(m[3]), avg_ms=float(m[4]), max_ms=float(m[5])) if m else dict(raw=r.stdout)


def cmd_responsive(a, log):
    c, boot_s = boot(log)
    res = dict()
    ip = board_ip(c)
    check(ip, f'board has an address on Wi-Fi ({ip})', res)
    if not ip:
        save('responsive', res)
        return False
    res['ip'] = ip
    res['ping_idle'] = ping_stats(ip, 50)
    # console round trip while the guest is idle
    def echo_rtt(n):
        t = []
        for _ in range(n):
            t0 = time.monotonic()
            out, _ = c.run('echo x')
            t.append((time.monotonic() - t0) * 1000)
        return dict(n=n, min_ms=round(min(t), 1), max_ms=round(max(t), 1), avg_ms=round(sum(t) / n, 1))
    res['console_idle'] = echo_rtt(20)
    # background: back-to-back native KEM keypairs (the longest native call) and max-size AEAD
    for case in ('sntrup761-keypair-seeded', 'sntrup761-decap-seeded', 'aead-seal-max'):
        mark = len(c.all)
        c.run(f'(crypto-vectors {VECTORS} --bench {a.calls} {case} > /run/bg.log 2>&1; echo done >> /run/bg.log) & true')
        p = ping_stats(ip, 50)
        e = echo_rtt(10)
        out, _ = c.run('for i in $(seq 1 600); do grep -q done /run/bg.log && break; sleep 1; done; cat /run/bg.log', 900)
        # the firmware's report goes to the console, not to bg.log
        r = parse_report(c.all[mark:])
        iv = interval(*r['points'], r['sys_hz']) if len(r['points']) == 2 else None
        res[f'busy_{case}'] = dict(ping=p, console=e, native_us_per_call=round(iv['ms'] * 1000 / a.calls, 1) if iv else None,
                                   completed='BENCH DONE' in out)
        check('BENCH DONE' in out and p.get('received') == p.get('sent'), f'{case} in a loop: no ping loss, console answers, op completes', res)
    # a process killed in the middle of a native operation: the kernel
    # aborts it; the next operations and the erased stack are unaffected
    before, _ = report(c)
    out, _ = c.run(f'crypto-vectors {VECTORS} --bench 5 sntrup761-keypair-seeded > /dev/null 2>&1 & p=$!; sleep 2; kill -9 $p; wait $p; echo "killed status $?"', 120)
    killed = re.search(r'killed status (\d+)', out)
    mid, _ = report(c)
    vec, rc = c.run(f'crypto-vectors {VECTORS} 1', 900)
    rep, _ = report(c)
    res['after_kill'] = dict(status=int(killed[1]) if killed else None, aborts=[before['native_aborts'], mid['native_aborts']],
                             erased_after_abort=mid['native_stack_erased'], vectors=vec.strip(), erased=rep['native_stack_erased'])
    check(killed and killed[1] == '137' and mid['native_aborts'] == before['native_aborts'] + 1 and mid['native_stack_erased'],
          'kill -9 during a native keypair: the kernel aborts it, the stack is erased', res)
    check(rc == 0 and 'CRYPTO VECTORS PASS' in vec and rep['native_stack_erased'], 'after the abort every vector passes', res)
    res['report'] = rep
    c.close()
    save('responsive', res)
    return all(x['passed'] for x in res['checks'])


def cmd_remote(a, log):
    """OpenSSH on this Mac to Dropbear on the board over Wi-Fi."""
    c, boot_s = boot(log)
    res = dict(runs={})
    ip = board_ip(c)
    check(ip, f'board has an address on Wi-Fi ({ip})', res)
    if not ip:
        save('remote', res)
        return False
    work = OUT / 'remote'
    work.mkdir(parents=True, exist_ok=True)
    key = work / 'id_ed25519'
    wrong = work / 'wrong_ed25519'
    for k in (key, wrong):
        if not k.exists():
            subprocess.run(['ssh-keygen', '-q', '-t', 'ed25519', '-N', '', '-f', str(k)], check=True)
    pub = (work / 'id_ed25519.pub').read_text().strip()
    c.run('mkdir -m 700 -p /run/remote-ssh && dropbearkey -t ed25519 -f /run/remote-ssh/hostkey >/dev/null 2>&1')
    c.run(f"echo '{pub}' > /run/remote-ssh/authorized_keys && chmod 600 /run/remote-ssh/authorized_keys")
    payload = os.urandom(1 << 20)
    (work / 'payload').write_bytes(payload)
    import hashlib
    want = hashlib.sha256(payload).hexdigest()
    for mode in a.modes:
        env = '0' if mode == 'off' else '1'
        c.run('[ -f /run/remote-ssh/pid ] && kill $(cat /run/remote-ssh/pid); sleep 1; true')
        c.run(f'RVEMU_SSH_OFFLOAD={env} dropbear -E -s -D /run/remote-ssh -p 2223 -r /run/remote-ssh/hostkey -P /run/remote-ssh/pid > /run/remote-ssh/log 2>&1')
        time.sleep(1)
        r0, _ = report(c)
        base = ['ssh', '-p', '2223', '-o', 'StrictHostKeyChecking=no', '-o', 'UserKnownHostsFile=/dev/null', '-o', 'BatchMode=yes',
                '-o', 'IdentitiesOnly=yes', '-o', 'KexAlgorithms=sntrup761x25519-sha512,sntrup761x25519-sha512@openssh.com',
                '-o', 'Ciphers=chacha20-poly1305@openssh.com', '-o', 'HostKeyAlgorithms=ssh-ed25519', '-o', 'LogLevel=ERROR']
        run = lambda extra, cmd, **kw: subprocess.run(base + extra + [f'root@{ip}', cmd], capture_output=True, timeout=600, **kw)
        r = {}
        t0 = time.monotonic()
        p = run(['-i', str(key)], 'echo authenticated')
        r['connect_and_command_s'] = round(time.monotonic() - t0, 2)
        r['auth'] = p.returncode == 0 and p.stdout.strip() == b'authenticated'
        r['exit_status'] = run(['-i', str(key)], 'exit 7').returncode
        t0 = time.monotonic()
        p = run(['-i', str(key)], 'sha256sum', input=payload)
        r['upload_s'] = round(time.monotonic() - t0, 2)
        r['upload_ok'] = p.stdout.split(b' ')[0].decode() == want
        t0 = time.monotonic()
        p = run(['-i', str(key)], 'cat > /run/remote-ssh/p; cat /run/remote-ssh/p; rm /run/remote-ssh/p', input=payload)
        r['roundtrip_s'] = round(time.monotonic() - t0, 2)
        r['download_ok'] = p.stdout == payload
        p = run(['-i', str(wrong)], 'true')
        r['wrong_key_rejected'] = p.returncode != 0 and b'Permission denied' in p.stderr
        r1, _ = report(c)
        r['native_calls'] = [y - x for x, y in zip(r0['native_calls'], r1['native_calls'])]
        out, _ = c.run('tail -5 /run/remote-ssh/log')
        r['server_log'] = out
        res['runs'][mode] = r
        check(r['auth'] and r['exit_status'] == 7 and r['upload_ok'] and r['download_ok'] and r['wrong_key_rejected'],
              f'OpenSSH -> board Dropbear ({mode}): auth, exit 7, 1 MiB up + 1 MiB round trip, wrong key rejected', res)
        if mode == 'off':
            check(not any(r['native_calls']), 'remote off: no native calls', res)
        else:
            check(r['native_calls'][4] > 10 and r['native_calls'][5] > 10 and all(r['native_calls'][o] for o in (6, 8, 11)),
                  'remote on: native packets, X25519, Ed25519 sign (host key), sntrup encapsulation (server side)', res)
    c.run('[ -f /run/remote-ssh/pid ] && kill $(cat /run/remote-ssh/pid); true')
    out, _ = c.run('ssh -V 2>&1; true')
    res['mac_ssh'] = subprocess.run(['ssh', '-V'], capture_output=True, text=True).stderr.strip()
    c.close()
    save('remote', res)
    return all(x['passed'] for x in res['checks'])


if __name__ == '__main__':
    p = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = p.add_subparsers(dest='cmd', required=True)
    s = sub.add_parser('vectors'); s.add_argument('--repeats', type=int, default=3); s.add_argument('--calls', type=int, default=20)
    s = sub.add_parser('ssh'); s.add_argument('--bulk', type=int, default=1); s.add_argument('--modes', nargs='+', default=['on', 'off', 'client', 'server'])
    s = sub.add_parser('connection'); s.add_argument('-n', type=int, default=3); s.add_argument('--settle', type=float, default=15)
    s = sub.add_parser('responsive'); s.add_argument('--calls', type=int, default=100)
    s = sub.add_parser('remote'); s.add_argument('--modes', nargs='+', default=['on', 'off'])
    a = p.parse_args()
    OUT.mkdir(parents=True, exist_ok=True)
    with (OUT / f'{a.cmd}-console.log').open('a') as log:
        ok = globals()[f'cmd_{a.cmd}'](a, log)
    sys.exit(0 if ok else 1)
