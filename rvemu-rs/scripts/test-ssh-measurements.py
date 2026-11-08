#!/usr/bin/env python3
"""Check connection-only accounting and fail-closed benchmark validation."""
import importlib.util
from pathlib import Path
import unittest

spec=importlib.util.spec_from_file_location('ssh_eval',Path(__file__).with_name('ssh-eval.py'))
module=importlib.util.module_from_spec(spec)
spec.loader.exec_module(module)

class MeasurementTests(unittest.TestCase):
    def log(self, native=True):
        return '\n'.join(f'benchmark point tag={i} guest={1000+100*i} host={9000+600*i} ns={10000+1000*i} idle=50 native=[0, 0, 0, 0, {i if native else 0}, {i if native else 0}, {i if native else 0}, {i if native else 0}, {i if native else 0}, {i if native else 0}, {i if native else 0}, {i if native else 0}, {i if native else 0}]' for i in range(4))

    def test_only_boundary_deltas_count(self):
        text='guest instructions 999999999\nhost instructions 999999999\n'+self.log()
        samples,medians=module.connection_measurements(text,1,'jit',True)
        self.assertEqual(medians['connection']['host_instructions'],1800)
        self.assertEqual(medians['setup_and_key_exchange']['host_instructions'],1200)
        self.assertEqual(medians['authentication']['host_instructions'],600)
        self.assertEqual(samples[0]['connection']['native_buffer_calls'][4:6],[3,3])
        self.assertIsNone(medians['connection']['host_elapsed_seconds'])

    def test_missing_duplicate_and_reversed_boundaries_fail(self):
        for text in [self.log().rsplit('\n',1)[0],self.log()+'\n'+self.log(),self.log().replace('tag=2','tag=1'),self.log().replace('host=10200','host=100')]:
            with self.assertRaises(ValueError): module.connection_measurements(text,1,'jit',True)

    def test_software_cannot_impersonate_native(self):
        with self.assertRaises(ValueError): module.connection_measurements(self.log(False),1,'jit',True)
        with self.assertRaises(ValueError): module.connection_measurements(self.log(),1,'jit',False)
        _,m=module.connection_measurements(self.log(False),1,'host',False)
        self.assertIsNone(m['connection']['host_instructions'])
        self.assertAlmostEqual(m['connection']['host_elapsed_seconds'],0.000003)

if __name__=='__main__': unittest.main()
