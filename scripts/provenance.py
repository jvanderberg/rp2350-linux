#!/usr/bin/env python3
"""Portable build/check provenance; records selected inputs, never the environment."""
import argparse
import hashlib
import json
import math
import os
import platform
import subprocess
from datetime import datetime, timezone
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent


def output(*command):
    try:
        result = subprocess.run(command, cwd=ROOT, text=True, stdout=subprocess.PIPE,
                                stderr=subprocess.STDOUT, timeout=15)
        return result.stdout.strip() if result.returncode == 0 else None
    except (OSError, subprocess.TimeoutExpired):
        return None


def digest(path):
    h = hashlib.sha256()
    with path.open('rb') as stream:
        for block in iter(lambda: stream.read(1024 * 1024), b''):
            h.update(block)
    return h.hexdigest()


def source_state():
    tracked = output('git', 'ls-files', '--cached', '--others', '--exclude-standard') or ''
    # Includes working-copy edits, in addition to the commit and dirty status.
    inputs = {name: digest(ROOT / name) for name in tracked.splitlines()
              if (ROOT / name).is_file()}
    return {'source_sha256': inputs,
            'worktree_status': output('git', 'status', '--porcelain')}


def snapshot():
    sources = source_state()
    guests = {str(p.relative_to(ROOT)): digest(p)
              for folder in ('out', 'out-disk')
              for p in (ROOT / 'guest' / folder).glob('*')
              if p.is_file() and p.name != 'build.json'}
    versions = {name: output(*command) for name, command in {
        'rustc': ['rustc', '-vV'], 'cargo': ['cargo', '--version'],
        'python': ['python3', '--version'], 'cc': ['cc', '--version'],
        'make': ['make', '--version'], 'dtc': ['dtc', '--version'],
        'qemu': ['qemu-system-riscv32', '--version'],
        'docker': ['docker', '--version'], 'picotool': ['picotool', 'version'],
        'probe_rs': [os.environ.get('PROBE_RS', str(Path.home() / 'git/toolchains/probe-rs-master/bin/probe-rs')), '--version'],
    }.items()}
    return {'schema': 1, 'time_utc': datetime.now(timezone.utc).isoformat(),
            'platform': platform.platform(), 'commit': output('git', 'rev-parse', 'HEAD'),
            **sources, 'guest_sha256': guests, 'tools': versions,
            'buildroot_commit': output('git', '-C', 'guest/src/buildroot', 'rev-parse', 'HEAD'),
            'container_image': output('docker', 'image', 'inspect', 'rvemu-guest', '--format', '{{.Id}}')}


BASELINE_FILES = ('perf-baseline.json', 'md5-files.list')
BASELINE_METRICS = ('initramfs_ms', 'boot_s', 'first_runs_s', 'mpy_s', 'lua_s',
                    'fork_exec_s', 'md5_cold_s', 'md5_warm_s', 'mem_avail_kb')


def baseline_hashes(folder):
    return {name: digest(folder / name) for name in BASELINE_FILES}


def validate_baseline(folder):
    baseline = json.loads((folder / 'perf-baseline.json').read_text())
    if not isinstance(baseline, dict) or baseline.get('checksums_ok') != 1:
        raise ValueError('Invalid baseline checksum result')
    for key in (*BASELINE_METRICS, 'mem_adj_kb'):
        if key == 'mem_adj_kb' and key not in baseline:
            continue
        value = baseline.get(key)
        if type(value) not in (int, float) or not math.isfinite(value) or value <= 0:
            raise ValueError(f'Invalid baseline metric: {key}')
    if not (folder / 'md5-files.list').read_text().strip():
        raise ValueError('Empty baseline checksum list')
    provenance = json.loads((folder / 'baseline-provenance.json').read_text())
    if not isinstance(provenance, dict) or provenance.get('baseline_sha256') != baseline_hashes(folder):
        raise ValueError('Baseline files do not match their provenance; restore the complete original bundle')


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--output', required=True, type=Path)
    parser.add_argument('--stage', required=True)
    args = parser.parse_args()
    data = snapshot()
    data['stage'] = args.stage
    if args.stage == 'board-baseline':
        data['baseline_sha256'] = baseline_hashes(args.output.parent)
    temporary = args.output.with_suffix('.json.tmp')
    temporary.write_text(json.dumps(data, indent=2, allow_nan=False) + '\n')
    temporary.replace(args.output)
