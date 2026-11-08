#!/usr/bin/env python3
"""Explicit host, QEMU, and board checks with retained logs and provenance."""
import argparse
import ast
import json
import os
import re
import shlex
import shutil
import signal
import subprocess
import sys
import time
from datetime import datetime, timezone
from pathlib import Path

from provenance import ROOT, digest, snapshot, source_state, validate_baseline

RS = ROOT / 'rvemu-rs'


def require_tool(name, hint):
    if not shutil.which(name):
        raise RuntimeError(f'Missing tool: {name}. {hint}')


def require_file(path, hint):
    if not path.is_file() or not path.stat().st_size:
        raise RuntimeError(f'Missing or empty file: {path.relative_to(ROOT)}. {hint}')


def preflight(tier):
    for name in ('git', 'cargo', 'rustc', 'bash', 'make', 'cc'):
        require_tool(name, 'See docs/development.md.')
    if tier == 'host':
        return
    require_tool('dtc', 'Install device-tree-compiler (Linux) or dtc (Homebrew).')
    require_tool('rustup', 'Install Rust with rustup.')
    targets = subprocess.check_output(['rustup', 'target', 'list', '--installed'], cwd=ROOT, text=True)
    wanted = ['riscv32imac-unknown-none-elf']
    if tier == 'board':
        wanted.append('riscv32im-unknown-none-elf')
    for target in wanted:
        if target not in targets.splitlines():
            raise RuntimeError(f'Missing Rust target. Run: rustup target add {target}')
    for name in ('Image', 'rootfs.cpio'):
        require_file(ROOT / 'guest/out' / name, 'Run: (cd guest && ./run-build.sh all)')
    if tier == 'qemu':
        require_tool('qemu-system-riscv32', 'Install qemu-system-misc (Linux) or qemu (Homebrew).')
    else:
        if sys.platform != 'darwin':
            raise RuntimeError('Board scripts currently require macOS; host and QEMU checks also support Linux.')
        for name in ('docker', 'picotool', 'lsof', 'shasum'):
            require_tool(name, 'See docs/development.md.')
        subprocess.run(['docker', 'info'], stdout=subprocess.DEVNULL, check=True)
        probe = os.environ.get('PROBE_RS', str(Path.home() / 'git/toolchains/probe-rs-master/bin/probe-rs'))
        require_tool(probe, 'Set PROBE_RS to the RP2350-capable probe-rs binary.')
        host = next(s[6:] for s in subprocess.check_output(['rustc', '-vV'], text=True).splitlines() if s.startswith('host: '))
        sysroot = subprocess.check_output(['rustc', '--print', 'sysroot'], text=True).strip()
        for tool in ('llvm-objcopy', 'llvm-objdump', 'llvm-nm'):
            if not (Path(sysroot) / 'lib/rustlib' / host / 'bin' / tool).is_file():
                raise RuntimeError('Missing LLVM tools. Run: rustup component add llvm-tools-preview')
        for folder, names in [('out', ('System.map',)), ('out-disk', ('Image', 'rootfs.squashfs', 'System.map', 'linux.config'))]:
            for name in names:
                require_file(ROOT / 'guest' / folder / name, 'Run: (cd guest && ./run-build.sh all && ./run-build.sh disk)')
        validate_baseline(RS / 'target/perf-gate')


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('tier', choices=['host', 'qemu', 'board'])
    parser.add_argument('--artifacts', type=Path, help='New directory for logs and result.json')
    parser.add_argument('--timeout', type=int, default=1800, help='Per-command timeout in seconds (default: 1800)')
    args = parser.parse_args()
    if args.timeout <= 0:
        parser.error('--timeout must be positive')
    dest = args.artifacts or ROOT / 'target/checks' / (datetime.now(timezone.utc).strftime('%Y%m%dT%H%M%S-%fZ') + '-' + args.tier)
    dest = dest.resolve()
    dest.mkdir(parents=True, exist_ok=False)
    print(f'Artifacts: {dest}', flush=True)
    record = {'tier': args.tier, 'timeout_seconds': args.timeout, 'status': 'running', 'steps': [], 'provenance': snapshot()}

    def save():
        (dest / 'result.json').write_text(json.dumps(record, indent=2) + '\n')

    cancelled = False

    def terminate(signum, frame):
        nonlocal cancelled
        cancelled = True

    previous_sigterm = signal.signal(signal.SIGTERM, terminate)

    def check_cancelled():
        if cancelled:
            raise KeyboardInterrupt('Received SIGTERM')

    def run(name, command, cwd=ROOT, env=None):
        check_cancelled()
        log = dest / (name + '.log')
        step = {'name': name, 'command': command, 'cwd': str(cwd.relative_to(ROOT)), 'log': log.name}
        record['steps'].append(step)
        save()
        print(f'[{name}] {shlex.join(command)}', flush=True)
        start = time.monotonic()
        with log.open('w') as stream:
            proc = subprocess.Popen(command, cwd=cwd, stdout=stream, stderr=subprocess.STDOUT, start_new_session=True,
                                    env={**os.environ, **(env or {})})
            try:
                deadline = start + args.timeout
                while True:
                    check_cancelled()
                    remaining = deadline - time.monotonic()
                    if remaining <= 0:
                        raise subprocess.TimeoutExpired(command, args.timeout)
                    try:
                        code = proc.wait(timeout=min(0.2, remaining))
                        check_cancelled()
                        break
                    except subprocess.TimeoutExpired:
                        continue
            except (subprocess.TimeoutExpired, KeyboardInterrupt):
                try:
                    os.killpg(proc.pid, signal.SIGTERM)
                except ProcessLookupError:
                    pass
                try:
                    proc.wait(timeout=5)
                except subprocess.TimeoutExpired:
                    os.killpg(proc.pid, signal.SIGKILL)
                    proc.wait()
                # The shell may exit before a descendant that ignores SIGTERM.
                try:
                    os.killpg(proc.pid, signal.SIGKILL)
                except ProcessLookupError:
                    pass
                step['status'] = 'interrupted_or_timed_out'
                raise
            finally:
                step['seconds'] = round(time.monotonic() - start, 3)
                step['exit_code'] = proc.returncode
                save()
        if code:
            step['status'] = 'failed'
            print(log.read_text(errors='replace')[-8000:], file=sys.stderr)
            raise RuntimeError(f'{name} failed ({code}); see {log}')
        step['status'] = 'passed'
        save()
        return log.read_text(errors='replace')

    try:
        preflight(args.tier)
        if args.tier == 'host':
            tracked = subprocess.check_output(['git', 'ls-files', '--cached', '--others', '--exclude-standard', '-z'], cwd=ROOT).decode().split('\0')
            for name in sorted(set(tracked)):
                path = ROOT / name
                if name.endswith('.sh') and path.is_file():
                    run('syntax-' + name.replace('/', '_'), ['bash', '-n', name])
                elif name.endswith('.py') and path.is_file():
                    ast.parse(path.read_bytes(), filename=name)
            run('runner-tests', [sys.executable, '-m', 'unittest', 'discover', '-s', 'scripts/tests'])
            run('cargo-test', ['cargo', 'test', '--locked'], RS)
            run('cargo-build', ['cargo', 'build', '--release', '--locked'], RS)
            run('c-build', ['make', '-C', 'rvemu'])
        elif args.tier == 'qemu':
            run('compare', ['bash', 'scripts/compare.sh'], RS)
            for mode in ('interp', 'jit', 'lockstep'):
                log = run('qemu-' + mode, ['bash', 'scripts/virt.sh', mode], RS)
                if not re.search(r'to --until match [1-9][0-9]*', log):
                    record['steps'][-1]['status'] = 'failed'
                    raise RuntimeError(f'QEMU {mode} exited without reaching login; see its log')
                if mode == 'lockstep' and not re.search(r'lockstep: [1-9][0-9]* events checked, all matched', log):
                    record['steps'][-1]['status'] = 'failed'
                    raise RuntimeError('QEMU did not report successful lockstep verification')
        else:
            print('Board checks will flash and reboot the attached board.', flush=True)
            run('firmware-build', ['cargo', 'build', '--locked', '--release', '-p', 'rvemu-pico', '--target', 'riscv32imac-unknown-none-elf'], RS)
            run('core1', ['python3', 'scripts/check-core1.py'], RS)
            run('jittest', ['bash', 'scripts/pico.sh'], RS,
                env={'JITTEST_CAPTURE': str(dest / 'jittest.bin')})
            run('performance', ['bash', 'scripts/perf-gate.sh'], RS)
        check_cancelled()
        record['status'] = 'passed'
    except (RuntimeError, OSError, ValueError, SyntaxError, subprocess.SubprocessError, KeyboardInterrupt) as exc:
        record['status'] = 'failed'
        record['error'] = str(exc) or type(exc).__name__
        print(f'check: {record["error"]}', file=sys.stderr)
    finally:
        signal.signal(signal.SIGTERM, previous_sigterm)
        record['final_source_state'] = source_state()
        captured = dest / 'jittest.bin'
        if captured.is_file():
            record['generated_inputs'] = {'jittest.bin': digest(captured)}
        if args.tier == 'board' and (RS / 'target/perf-gate').is_dir():
            shutil.copytree(RS / 'target/perf-gate', dest / 'perf-gate')
        save()
    print(f'{args.tier}: {record["status"]}; {dest / "result.json"}', flush=True)
    return 0 if record['status'] == 'passed' else 1


if __name__ == '__main__':
    sys.exit(main())
