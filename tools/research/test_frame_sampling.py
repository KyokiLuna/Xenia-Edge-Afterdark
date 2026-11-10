"""Linux synthetic process tests; never launches Xenia or reads other memory."""
import contextlib
import importlib.util
import json
import os
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
import time
import unittest

HERE = Path(__file__).resolve().parent
WRAPPER = HERE / 'frame_wrapper.py'
SAMPLER = HERE / 'frame_sample.py'


@unittest.skipUnless(sys.platform.startswith('linux') and shutil.which('cc') and shutil.which('nm'),
                     'Linux C compiler/nm needed for synthetic permission test')
class FrameSamplingTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.temp = tempfile.TemporaryDirectory(prefix='xenia-frame-synthetic-')
        cls.root = Path(cls.temp.name)
        source = cls.root / 'counter.c'
        source.write_text('''#include <stdint.h>
#include <unistd.h>
volatile uint32_t counter __asm__("_ZN2xe20global_frame_number_E") = 0;
int main(int argc, char **argv) { for (int n=0;n<1000;n++) {counter++;usleep(10000);} return 0; }
''')
        cls.binary = cls.root / 'counter'
        subprocess.run(['cc', '-fPIE', '-pie', '-O0', '-o', str(cls.binary), str(source)], check=True,
                       capture_output=True)

    @classmethod
    def tearDownClass(cls):
        cls.temp.cleanup()

    @contextlib.contextmanager
    def target(self):
        run = Path(tempfile.mkdtemp(dir=self.root))
        record = run / 'frame-wrapper.json'
        env = dict(os.environ, XENIA_RESEARCH_BINARY=str(self.binary),
                   XENIA_RESEARCH_WRAPPER_RECORD=str(record))
        process = subprocess.Popen([str(WRAPPER), '--log_file='+str(run/'xenia.log')],
                                   env=env, stdout=subprocess.DEVNULL, stderr=subprocess.PIPE)
        try:
            deadline = time.monotonic()+5
            while time.monotonic()<deadline:
                if process.poll() is not None:
                    self.fail('Synthetic wrapper exited: '+process.stderr.read().decode())
                try:
                    if record.exists() and os.readlink(f'/proc/{process.pid}/exe') == str(self.binary):
                        json.loads(record.read_text())
                        break
                except (OSError, ValueError):
                    pass
                time.sleep(.01)
            else:
                self.fail('Synthetic wrapper did not exec')
            yield process, run, record
        finally:
            if process.poll() is None:
                process.terminate()
            process.wait(timeout=5)
            process.stderr.close()

    def command(self, process, run, record, *extra):
        return [sys.executable, str(SAMPLER), '--pid', str(process.pid), '--binary', str(self.binary),
                '--wrapper-record', str(record), '--output', str(run/'samples.jsonl'),
                '--interval', '.1', '--duration', '.4', *extra]

    def test_sibling_permission_pie_and_counter_only(self):
        with self.target() as (process, run, record):
            # Sampler and wrapped counter are sibling children, as in baseline runs.
            result = subprocess.run(self.command(process, run, record), capture_output=True, text=True)
            self.assertEqual(result.returncode, 0, result.stderr)
            lines = [json.loads(s) for s in (run/'samples.jsonl').read_text().splitlines()]
            self.assertEqual(lines[0]['read_size'], 4)
            self.assertEqual(lines[0]['pid'], process.pid)
            self.assertNotEqual(lines[0]['load_bias'], '0x0')
            self.assertIn('wrapper_sha256', lines[0])
            self.assertIn('native_binary_sha256', lines[0])
            self.assertTrue(all(0<s['guest_present_fps']<250 for s in lines if s['event']=='sample'))
            self.assertEqual(lines[-1]['samples'], 4)
            self.assertGreater(lines[-1]['present_delta'], 0)

    def test_wrong_executable_rejected_before_output(self):
        with self.target() as (process, run, record):
            result = subprocess.run(self.command(process, run, record, '--binary', '/bin/sleep'),
                                    capture_output=True, text=True)
            self.assertNotEqual(result.returncode, 0)
            self.assertIn('executable', result.stderr)
            self.assertFalse((run/'samples.jsonl').exists())

    def test_stale_starttime_record_rejected(self):
        with self.target() as (process, run, record):
            value=json.loads(record.read_text());value['starttime_ticks']-=1
            stale=run/'stale.json';stale.write_text(json.dumps(value))
            result=subprocess.run(self.command(process,run,stale),capture_output=True,text=True)
            self.assertNotEqual(result.returncode,0)
            self.assertIn('provenance',result.stderr)
            self.assertFalse((run/'samples.jsonl').exists())

    def test_existing_output_preserved(self):
        with self.target() as (process, run, record):
            output=run/'samples.jsonl';output.write_text('preserve me\n')
            result=subprocess.run(self.command(process,run,record),capture_output=True,text=True)
            self.assertNotEqual(result.returncode,0)
            self.assertEqual(output.read_text(),'preserve me\n')

    def test_wrapper_requires_absolute_binary(self):
        result=subprocess.run([str(WRAPPER)],env=dict(os.environ,XENIA_RESEARCH_BINARY='relative'),
                              capture_output=True,text=True)
        self.assertEqual(result.returncode,127)
        self.assertIn('absolute',result.stderr)


if __name__=='__main__':
    unittest.main()
