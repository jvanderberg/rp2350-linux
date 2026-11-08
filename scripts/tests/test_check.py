"""Exercise runner outcomes using disposable shell scripts, without guest images."""
import contextlib
import io
import json
import subprocess
import time
import sys
import tempfile
import unittest
from pathlib import Path
from unittest.mock import patch

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
import check
import provenance


class RunnerTests(unittest.TestCase):
    def run_qemu(self, body, timeout=5):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            scripts = root / 'rvemu-rs/scripts'
            scripts.mkdir(parents=True)
            (scripts / 'compare.sh').write_text('exit 0\n')
            (scripts / 'virt.sh').write_text(body)
            dest = root / 'results'
            args = ['check.py', 'qemu', '--artifacts', str(dest), '--timeout', str(timeout)]
            with patch.object(check, 'ROOT', root), patch.object(check, 'RS', scripts.parent), \
                 patch.object(check, 'snapshot', return_value={'source_sha256': {'input': 'before'}}), \
                 patch.object(check, 'source_state', return_value={'source_sha256': {'input': 'after'}}), \
                 patch.object(check, 'preflight'), patch.object(sys, 'argv', args), \
                 contextlib.redirect_stdout(io.StringIO()), contextlib.redirect_stderr(io.StringIO()):
                status = check.main()
            result = json.loads((dest / 'result.json').read_text())
            result['test_logs'] = {p.name: p.read_text() for p in dest.glob('*.log')}
            return status, result

    def test_success_requires_boot_and_lockstep_evidence(self):
        status, result = self.run_qemu('echo "to --until match 42"\necho "lockstep: 12 events checked, all matched"\n')
        self.assertEqual(status, 0)
        self.assertEqual(len(result['steps']), 4)

    def test_zero_exit_without_boot_is_failure(self):
        status, result = self.run_qemu('echo "guest halted"\n')
        self.assertEqual(status, 1)
        self.assertIn('without reaching login', result['error'])

    def test_missing_lockstep_evidence_is_failure(self):
        status, result = self.run_qemu('echo "to --until match 42"\n')
        self.assertEqual(status, 1)
        self.assertIn('lockstep verification', result['error'])

    def test_process_failure_is_retained(self):
        status, result = self.run_qemu('exit 7\n')
        self.assertEqual(status, 1)
        self.assertEqual(result['steps'][-1]['exit_code'], 7)

    def test_timeout_is_failure(self):
        for attempt in range(10):
            with self.subTest(attempt=attempt):
                status, result = self.run_qemu('sleep 30\n', timeout=1)
                diagnostic = json.dumps(result, indent=2)
                self.assertEqual(status, 1, diagnostic)
                self.assertEqual(result['steps'][-1].get('status'),
                                 'interrupted_or_timed_out', diagnostic)
                self.assertIn('timed out', result['error'], diagnostic)

    def test_initial_and_final_source_states_are_retained(self):
        _, result = self.run_qemu('exit 7\n')
        self.assertEqual(result['provenance']['source_sha256']['input'], 'before')
        self.assertEqual(result['final_source_state']['source_sha256']['input'], 'after')

    def test_generated_guest_is_captured_even_on_board_step_failure(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            rs = root / 'rvemu-rs'
            rs.mkdir()
            dest = root / 'results'
            popen = subprocess.Popen
            def fake_board(command, **kwargs):
                # Replace every board/build command, so this test cannot access hardware.
                if command == ['bash', 'scripts/pico.sh']:
                    command = ['bash', '-c', 'printf regenerated > "$JITTEST_CAPTURE"; exit 7']
                else:
                    command = ['bash', '-c', 'exit 0']
                return popen(command, **kwargs)
            with patch.object(check, 'ROOT', root), patch.object(check, 'RS', rs), \
                 patch.object(check, 'snapshot', return_value={}), \
                 patch.object(check, 'source_state', return_value={}), \
                 patch.object(check, 'preflight'), patch.object(check.subprocess, 'Popen', side_effect=fake_board), \
                 patch.object(sys, 'argv', ['check.py', 'board', '--artifacts', str(dest)]), \
                 contextlib.redirect_stdout(io.StringIO()), contextlib.redirect_stderr(io.StringIO()):
                self.assertEqual(check.main(), 1)
            result = json.loads((dest / 'result.json').read_text())
            self.assertEqual((dest / 'jittest.bin').read_bytes(), b'regenerated')
            self.assertEqual(result['generated_inputs']['jittest.bin'], provenance.digest(dest / 'jittest.bin'))

    def test_sigterm_kills_descendants_and_saves_failure(self):
        # This child ignores TERM, so cleanup must kill the remaining group even
        # after its parent shell has exited. Wait for readiness before cancelling.
        body = """
python3 -c 'import os,signal,time; signal.signal(signal.SIGTERM, signal.SIG_IGN); print(os.getpid(), flush=True); open("ready", "w").close(); time.sleep(30)' &
while [ ! -f ready ]; do sleep 0.01; done
kill -TERM "$PPID"
wait
"""
        status, result = self.run_qemu(body)
        self.assertEqual(status, 1)
        self.assertEqual(result['steps'][-1]['status'], 'interrupted_or_timed_out')
        pid = int(result['test_logs']['qemu-interp.log'].splitlines()[0])
        for _ in range(100):
            state = subprocess.run(['ps', '-o', 'stat=', '-p', str(pid)],
                                   capture_output=True, text=True).stdout.strip()
            if not state or state.startswith('Z'):
                break
            time.sleep(0.01)
        else:
            self.fail(f'Descendant {pid} survived SIGTERM cleanup')


class ProcessGroupTests(unittest.TestCase):
    def test_darwin_permission_error_requires_no_live_members(self):
        with patch.object(check.sys, 'platform', 'darwin'), \
             patch.object(check.os, 'killpg', side_effect=PermissionError('denied')):
            for table in ('', '123 Z\n456 S\n', '123 Z+\n'):
                with patch.object(check.subprocess, 'check_output', return_value=table):
                    check.signal_group(123, 9)
            with patch.object(check.subprocess, 'check_output', return_value='123 Z\n123 S\n'):
                with self.assertRaises(PermissionError):
                    check.signal_group(123, 9)
            with patch.object(check.subprocess, 'check_output', side_effect=OSError('ps failed')):
                with self.assertRaises(OSError):
                    check.signal_group(123, 9)

    def test_other_platform_permission_errors_are_not_ignored(self):
        with patch.object(check.sys, 'platform', 'linux'), \
             patch.object(check.os, 'killpg', side_effect=PermissionError('denied')):
            with self.assertRaises(PermissionError):
                check.signal_group(123, 9)


class BaselineTests(unittest.TestCase):
    def test_bundle_binding_and_numeric_validation(self):
        with tempfile.TemporaryDirectory() as tmp:
            folder = Path(tmp)
            baseline = {key: 1 for key in provenance.BASELINE_METRICS}
            baseline['checksums_ok'] = 1
            metrics = folder / 'perf-baseline.json'
            files = folder / 'md5-files.list'
            manifest = folder / 'baseline-provenance.json'
            def write_bundle():
                metrics.write_text(json.dumps(baseline))
                files.write_text('usr/bin/example\n')
                manifest.write_text(json.dumps({'baseline_sha256': provenance.baseline_hashes(folder)}))
            write_bundle()
            provenance.validate_baseline(folder)
            for target in (metrics, files):
                original = target.read_text()
                target.write_text(original + ' ')
                with self.assertRaisesRegex(ValueError, 'do not match'):
                    provenance.validate_baseline(folder)
                target.write_text(original)
            manifest.unlink()
            with self.assertRaises(FileNotFoundError):
                provenance.validate_baseline(folder)
            for value in (float('nan'), float('inf'), -1, 0, True):
                baseline['boot_s'] = value
                write_bundle()
                with self.assertRaisesRegex(ValueError, 'Invalid baseline metric'):
                    provenance.validate_baseline(folder)


if __name__ == '__main__':
    unittest.main()
