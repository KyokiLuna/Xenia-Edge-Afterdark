"""Safety and lifecycle checks using a fake executable, never Xenia."""

import contextlib
import importlib.util
import io
import json
import os
from pathlib import Path
import signal
import subprocess
import sys
import tempfile
import time
from types import SimpleNamespace
import unittest
from unittest import mock


SPEC = importlib.util.spec_from_file_location(
    'baseline', Path(__file__).with_name('baseline.py'))
baseline = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(baseline)


@unittest.skipUnless(os.name == 'posix', 'Native Linux/macOS runner')
class BaselineTest(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory()
        self.addCleanup(self.temporary.cleanup)
        self.root = Path(self.temporary.name)
        disk_usage = mock.patch.object(
            baseline.shutil, 'disk_usage',
            return_value=SimpleNamespace(free=100 * 1024 ** 3))
        self.disk_usage = disk_usage.start()
        self.addCleanup(disk_usage.stop)
        self.source = self.root / 'staged'
        self.source.mkdir()
        (self.source / 'Account').write_bytes(b'original profile')
        (self.source / 'xenia-edge.config.toml').write_text(
            "[Storage]\ncontent_root = '/personal/data'\n")
        self.seed = self.root / 'seed'
        self.output = self.root / 'runs'
        self.fake = self.root / 'fake-xenia'
        self.write_fake('''
import json, os, pathlib, sys
args = dict(arg[2:].split('=', 1) for arg in sys.argv[1:]
            if arg.startswith('--'))
pathlib.Path('received.json').write_text(json.dumps({
    'argv': sys.argv[1:], 'environment': {
        k: os.environ[k] for k in
        ('XDG_DATA_HOME', 'XDG_CONFIG_HOME', 'XDG_CACHE_HOME')}}))
data = pathlib.Path(args['storage_root'])
(data / 'Account').write_bytes(b'run-only changed profile')
pathlib.Path(args['content_root']).mkdir(exist_ok=True)
pathlib.Path(args['cache_root']).mkdir(exist_ok=True)
pathlib.Path(args['log_file']).write_text(
    'Title ID: FFFE07D1\\nUnimplemented import TestExport\\n'
    'XHttp: no host HTTP client on this platform\\n')
print('fake capture completed')
''')
        self.invoke('prepare-seed', '--source', self.source,
                    '--destination', self.seed)

    def write_fake(self, source):
        self.fake.write_text('#!/usr/bin/env python3\n' + source)
        self.fake.chmod(0o755)

    def invoke(self, *arguments):
        with contextlib.redirect_stdout(io.StringIO()):
            return baseline.main([str(arg) for arg in arguments])

    def run_fake(self, *extra):
        return self.invoke('run', '--binary', self.fake, '--seed', self.seed,
                           '--output-root', self.output, '--scenario', 'test',
                           '--timeout', '5', '--grace', '0.2', *extra)

    def test_explicit_project_policy_is_bound_without_changing_mutable_isolation(self):
        self.assertEqual(self.run_fake('--protected-filesystem-root', self.root), 0)
        run = next(self.output.iterdir())
        received = json.loads((run / 'received.json').read_text())
        self.assertIn(f'--host_storage_protected_root={self.root.resolve()}', received['argv'])
        self.assertIn(f'--storage_root={run / "data"}', received['argv'])
        metadata = json.loads((run / 'metadata.json').read_text())
        policy = metadata['protected_filesystem_policy']
        self.assertEqual(policy['device'], self.root.stat().st_dev)
        self.assertEqual(policy['inode'], self.root.stat().st_ino)
        self.assertEqual(policy['reserve_bytes'], baseline.DISK_FREE_FLOOR_BYTES)
        self.assertEqual((self.source / 'Account').read_bytes(), b'original profile')

    def test_explicit_project_policy_refuses_low_space_before_any_run_copy(self):
        self.disk_usage.return_value = SimpleNamespace(free=baseline.DISK_FREE_FLOOR_BYTES - 1)
        with self.assertRaises(SystemExit):
            self.run_fake('--protected-filesystem-root', self.root)
        self.assertFalse(self.output.exists())
        self.assertEqual((self.source / 'Account').read_bytes(), b'original profile')

    def test_project_policy_cannot_be_overridden_in_extra_args(self):
        with self.assertRaises(SystemExit):
            self.run_fake('--emulator-arg=--host_storage_protected_root=/untrusted')
        self.assertFalse(self.output.exists())

    def test_copy_isolation_flags_and_unique_runs(self):
        before = baseline.tree_manifest(self.seed)
        for unused in range(2):
            self.assertEqual(self.run_fake('--emulator-arg=--log_level=3'), 0)
        runs = sorted(self.output.iterdir())
        self.assertEqual(len(runs), 2)
        self.assertEqual(baseline.tree_manifest(self.seed), before)
        self.assertEqual((self.source / 'Account').read_bytes(),
                         b'original profile')
        for run in runs:
            received = json.loads((run / 'received.json').read_text())
            args = dict(arg[2:].split('=', 1) for arg in received['argv'])
            self.assertEqual(args['storage_root'], str(run / 'data'))
            self.assertEqual(args['content_root'], str(run / 'data/content'))
            self.assertEqual(args['cache_root'], str(run / 'data/cache_host'))
            self.assertEqual(args['log_file'], str(run / 'xenia.log'))
            self.assertEqual(args['allow_game_relative_writes'], 'false')
            self.assertEqual(args['allow_plugins'], 'false')
            self.assertEqual(args['log_level'], '3')
            self.assertNotIn('system_readonly_source_root', args)
            self.assertTrue(all(str(run) in value for value in
                                received['environment'].values()))
            self.assertEqual((run / 'data/Account').read_bytes(),
                             b'run-only changed profile')
            metadata = json.loads((run / 'metadata.json').read_text())
            self.assertTrue(metadata['seed_unchanged'])
            self.assertTrue(metadata['capture_complete'])
            self.assertFalse(metadata['timed_out'])
            self.assertEqual(metadata['exit_code'], 0)
            self.assertEqual(metadata['binary_sha256'],
                             baseline.file_hash(self.fake))
            self.assertEqual(metadata['capture_tool']['sha256'],
                             baseline.file_hash(Path(baseline.__file__)))
            self.assertEqual(metadata['evidence']['http']['samples'][0]['line'],
                             3)
            self.assertEqual(metadata['evidence']['missing_host_http']['count'],
                             1)

    def test_reject_overrides_before_creating_run(self):
        for argument in ('--storage_root=/live', '--content_root=/live',
                         '--cache_root=/live', '--config=/live',
                         '--log_file=/live', '--target=/live',
                         '--portable=true', '--flagfile=/live',
                         '--system_readonly_source_root=/live',
                         '--allow_game_relative_writes=true', 'title.xex'):
            with self.subTest(argument=argument):
                with contextlib.redirect_stderr(io.StringIO()):
                    with self.assertRaises(SystemExit):
                        self.run_fake('--emulator-arg=' + argument)
                self.assertFalse(self.output.exists())

    def shared_fixture(self):
        system = self.source / 'system'
        (system / 'flash').mkdir(parents=True)
        (system / 'flash' / 'dash.xex').write_bytes(b'fixture dashboard')
        (system / 'sysext' / 'Content').mkdir(parents=True)
        (system / 'sysaux').mkdir()
        (self.source / 'content' / 'system').mkdir(parents=True)
        (self.source / 'content' / 'system' / 'keep').write_bytes(b'owned content')
        self.seed = self.root / 'seed-with-system'
        self.invoke('prepare-seed', '--source', self.source,
                    '--destination', self.seed)
        manifest = self.root / 'shared-system.json'
        self.invoke('prepare-shared-system', '--source', system,
                    '--destination', manifest)
        options = ('--shared-system-manifest', manifest,
                   '--shared-system-manifest-sha256', baseline.file_hash(manifest))
        return system, manifest, options

    def test_shared_source_excludes_only_top_system_and_preserves_isolation(self):
        system, manifest, options = self.shared_fixture()
        source_before = baseline.shared_system_snapshot(system)
        full = baseline.tree_manifest(self.seed)
        self.assertEqual(self.run_fake(*options), 0)
        run = next(self.output.iterdir())
        self.assertFalse((run / 'data' / 'system').exists())
        self.assertEqual((run / 'data/content/system/keep').read_bytes(), b'owned content')
        received = json.loads((run / 'received.json').read_text())
        self.assertIn('--system_readonly_source_root=' + str(system), received['argv'])
        metadata = json.loads((run / 'metadata.json').read_text())
        self.assertTrue(metadata['system_copy_excluded'])
        self.assertTrue(metadata['shared_system_unchanged'])
        self.assertTrue(metadata['seed_unchanged'])
        self.assertEqual(metadata['shared_system_source']['manifest_sha256'],
                         baseline.file_hash(manifest))
        self.assertNotEqual(metadata['seed_copy_sha256'], full['sha256'])
        self.assertEqual(baseline.shared_system_snapshot(system), source_before)
        self.assertEqual(baseline.tree_manifest(self.seed), full)
        full_space = baseline.require_disk_space(self.root, 'full', full,
                                                  self.seed)
        self.assertLess(metadata['disk_space_checks']['before_copy']['estimated_copy_bytes'],
                        full_space['estimated_copy_bytes'])

    def test_shared_manifest_pin_and_changed_assets_refuse_before_output(self):
        system, manifest, options = self.shared_fixture()
        with self.assertRaises(ValueError):
            baseline.load_shared_system(manifest, '0' * 64)
        (system / 'flash' / 'dash.xex').write_bytes(b'changed')
        with contextlib.redirect_stderr(io.StringIO()), self.assertRaises(SystemExit):
            self.run_fake(*options)
        self.assertFalse(self.output.exists())

    def test_shared_source_includes_files_named_like_seed_metadata(self):
        system, unused_manifest, unused_options = self.shared_fixture()
        marker = system / baseline.SEED_MARKER
        marker.write_bytes(b'original system-source metadata')
        manifest = self.root / 'complete-system.json'
        self.invoke('prepare-shared-system', '--source', system,
                    '--destination', manifest)
        snapshot = baseline.load_shared_system(manifest, baseline.file_hash(manifest))
        self.assertIn(baseline.SEED_MARKER,
                      [entry['path'] for entry in snapshot['tree']['files']])
        marker.write_bytes(b'changed system-source metadata')
        with self.assertRaises(ValueError):
            baseline.check_shared_system(snapshot)

    def test_shared_source_same_bytes_replacement_and_empty_dir_change_rejected(self):
        system, manifest, options = self.shared_fixture()
        snapshot = baseline.load_shared_system(manifest, baseline.file_hash(manifest))
        (system / 'sysaux').rmdir()
        with self.assertRaises(ValueError):
            baseline.check_shared_system(snapshot)
        (system / 'sysaux').mkdir()
        retired = self.source / 'retired-system'
        system.rename(retired)
        baseline.shutil.copytree(retired, system)
        with self.assertRaises(ValueError):
            baseline.check_shared_system(snapshot)
        self.assertFalse(self.output.exists())

    def test_shared_source_changed_during_copy_never_launches(self):
        system, manifest, options = self.shared_fixture()
        copy = baseline.shutil.copytree
        def changing_copy(*args, **kwargs):
            result = copy(*args, **kwargs)
            (system / 'flash' / 'dash.xex').write_bytes(b'changed after copy')
            return result
        with mock.patch.object(baseline.shutil, 'copytree', side_effect=changing_copy), \
                mock.patch.object(baseline.subprocess, 'Popen') as launch:
            self.assertEqual(self.run_fake(*options), 2)
            launch.assert_not_called()
        metadata = json.loads((next(self.output.iterdir()) / 'metadata.json').read_text())
        self.assertFalse(metadata['shared_system_unchanged'])
        self.assertTrue(metadata['seed_unchanged'])
        self.assertIn('shared_system_integrity_error', metadata)

    def test_shared_source_mutation_by_child_is_not_success(self):
        system, manifest, options = self.shared_fixture()
        self.write_fake('''
import pathlib, sys
args = dict(arg[2:].split('=', 1) for arg in sys.argv[1:] if arg.startswith('--'))
(pathlib.Path(args['system_readonly_source_root']) / 'flash/dash.xex').write_bytes(b'changed')
''')
        self.assertEqual(self.run_fake(*options), 2)
        metadata = json.loads((next(self.output.iterdir()) / 'metadata.json').read_text())
        self.assertEqual(metadata['exit_code'], 0)
        self.assertFalse(metadata['shared_system_unchanged'])
        self.assertTrue(metadata['seed_unchanged'])

    def test_unpinned_configured_shared_source_refused(self):
        (self.source / 'xenia-edge.config.toml').write_text(
            '[Storage]\nsystem_readonly_source_root = "/outside/system"\n')
        self.seed = self.root / 'configured-seed'
        self.invoke('prepare-seed', '--source', self.source, '--destination', self.seed)
        with contextlib.redirect_stderr(io.StringIO()), self.assertRaises(SystemExit):
            self.run_fake()
        self.assertFalse(self.output.exists())

    def test_run_output_and_manifest_destination_cannot_overlap_shared_source(self):
        system, manifest, options = self.shared_fixture()
        self.output = system / 'captures'
        with contextlib.redirect_stderr(io.StringIO()), self.assertRaises(SystemExit):
            self.run_fake(*options)
        self.assertFalse(self.output.exists())
        with contextlib.redirect_stderr(io.StringIO()), self.assertRaises(SystemExit):
            self.invoke('prepare-shared-system', '--source', system,
                        '--destination', system / 'manifest.json')
        self.assertFalse((system / 'manifest.json').exists())

    def test_manifest_destination_parent_alias_cannot_write_into_source(self):
        system, manifest, options = self.shared_fixture()
        alias = self.root / 'system-alias'
        alias.symlink_to(system, target_is_directory=True)
        with contextlib.redirect_stderr(io.StringIO()), self.assertRaises(SystemExit):
            self.invoke('prepare-shared-system', '--source', system,
                        '--destination', alias / 'unexpected.json')
        self.assertFalse((system / 'unexpected.json').exists())

    def test_malformed_manifest_is_a_recorded_refusal_before_copy(self):
        system, manifest, options = self.shared_fixture()
        manifest.write_text('[]')
        with contextlib.redirect_stderr(io.StringIO()), self.assertRaises(SystemExit):
            self.run_fake('--shared-system-manifest', manifest,
                          '--shared-system-manifest-sha256', baseline.file_hash(manifest))
        self.assertFalse(self.output.exists())

    def test_low_disk_refuses_seed_copy_and_empty_seed_before_creation(self):
        self.disk_usage.return_value.free = baseline.DISK_FREE_FLOOR_BYTES
        for source in (None, self.source):
            destination = self.root / ('empty' if source is None else 'copied')
            arguments = ['prepare-seed', '--destination', destination]
            if source:
                arguments += ['--source', source]
            with self.subTest(source=source), mock.patch.object(
                    baseline.shutil, 'copytree') as copy:
                with contextlib.redirect_stderr(io.StringIO()):
                    with self.assertRaises(SystemExit) as error:
                        self.invoke(*arguments)
                self.assertEqual(error.exception.code, 2)
                copy.assert_not_called()
                self.assertFalse(destination.exists())

    def test_copy_space_rounds_dense_logical_sizes_and_empty_directories(self):
        sparse = self.root / 'sparse-source'
        sparse.mkdir()
        (sparse / 'empty').mkdir()
        with (sparse / 'cache').open('wb') as output:
            output.truncate(8193)
        manifest = baseline.tree_manifest(sparse)
        destination = self.root / 'missing-parent' / 'copy'
        with mock.patch.object(baseline.os, 'statvfs', return_value=
                               SimpleNamespace(f_frsize=4096, f_bsize=4096)):
            check = baseline.require_disk_space(destination, 'test copy',
                                                manifest, sparse)
        # A dense copy needs three data blocks plus root and empty-directory
        # blocks, regardless of the source file's sparse allocation.
        self.assertEqual(check['estimated_copy_bytes'], 5 * 4096)
        self.assertEqual(check['filesystem_path'], str(self.root))
        self.assertFalse(destination.parent.exists())
        self.disk_usage.assert_called_with(self.root)

    def test_capture_copy_refuses_exact_budget_before_copy_or_launch(self):
        before = baseline.tree_manifest(self.seed)
        budget = baseline.require_disk_space(self.output, 'test', before,
                                              self.seed)['required_free_bytes']
        for free in (baseline.DISK_FREE_FLOOR_BYTES, budget - 1, budget):
            self.disk_usage.return_value.free = free
            with self.subTest(free=free), mock.patch.object(
                    baseline.shutil, 'copytree') as copy, mock.patch.object(
                    baseline.subprocess, 'Popen') as launch:
                with contextlib.redirect_stderr(io.StringIO()):
                    with self.assertRaises(SystemExit):
                        self.run_fake()
                copy.assert_not_called()
                launch.assert_not_called()
                self.assertFalse(self.output.exists())

    def test_disk_boundary_accepts_one_byte_above_reservation(self):
        reservation = (baseline.DISK_FREE_FLOOR_BYTES +
                       baseline.DISK_HEADROOM_BYTES)
        self.disk_usage.return_value.free = reservation
        with self.assertRaises(baseline.DiskSpaceError):
            baseline.require_disk_space(self.root, 'test launch')
        self.disk_usage.return_value.free += 1
        check = baseline.require_disk_space(self.root, 'test launch')
        self.assertTrue(check['accepted'])
        self.assertEqual(check['estimated_copy_bytes'], 0)

    def test_falling_disk_space_refuses_launch_and_records_no_process(self):
        previous = {sig: signal.getsignal(sig)
                    for sig in (signal.SIGINT, signal.SIGTERM)}
        self.disk_usage.side_effect = [
            SimpleNamespace(free=100 * 1024 ** 3),
            SimpleNamespace(free=baseline.DISK_FREE_FLOOR_BYTES),
        ]
        with mock.patch.object(baseline.subprocess, 'Popen') as launch:
            self.assertEqual(self.run_fake(), 2)
            launch.assert_not_called()
        run = next(self.output.iterdir())
        self.assertTrue((run / 'data' / 'Account').is_file())
        self.assertFalse((run / 'received.json').exists())
        metadata = json.loads((run / 'metadata.json').read_text())
        self.assertEqual(metadata['completion_reason'], 'low_disk_space')
        self.assertIn('disk_space_error', metadata)
        self.assertNotIn('pid', metadata)
        self.assertIsNone(metadata['exit_code'])
        self.assertTrue(metadata['capture_complete'])
        self.assertTrue(metadata['seed_unchanged'])
        self.assertTrue(metadata['disk_space_checks']['before_copy']['accepted'])
        self.assertFalse(metadata['disk_space_checks']['before_launch']['accepted'])
        self.assertEqual(previous, {sig: signal.getsignal(sig)
                                    for sig in previous})

    def test_completed_copy_not_reserved_again_at_launch(self):
        remaining = SimpleNamespace(free=baseline.DISK_FREE_FLOOR_BYTES +
                                    baseline.DISK_HEADROOM_BYTES + 1)
        readings = iter([
            SimpleNamespace(free=100 * 1024 ** 3),
            remaining,
        ])
        self.disk_usage.side_effect = lambda path: next(readings, remaining)
        self.assertEqual(self.run_fake(), 0)
        run = next(self.output.iterdir())
        metadata = json.loads((run / 'metadata.json').read_text())
        check = metadata['disk_space_checks']['before_launch']
        self.assertTrue(check['accepted'])
        self.assertEqual(check['estimated_copy_bytes'], 0)

    def test_disk_watchdog_stops_owned_fake_process_group(self):
        self.write_fake('''
import pathlib, subprocess, sys, time
child = subprocess.Popen([sys.executable, '-c', 'import time; time.sleep(60)'])
pathlib.Path('child.pid').write_text(str(child.pid))
time.sleep(60)
''')
        self.disk_usage.side_effect = [
            SimpleNamespace(free=100 * 1024 ** 3),
            SimpleNamespace(free=100 * 1024 ** 3),
            SimpleNamespace(free=baseline.DISK_FREE_FLOOR_BYTES +
                            baseline.DISK_WATCHDOG_HEADROOM_BYTES),
        ]
        self.assertEqual(self.run_fake(), 2)
        run = next(self.output.iterdir())
        self.assertTrue((run / 'child.pid').is_file())
        metadata = json.loads((run / 'metadata.json').read_text())
        self.assertEqual(metadata['completion_reason'], 'low_disk_space')
        self.assertIn('disk_space_error', metadata)
        self.assertIn('pid', metadata)
        self.assertFalse(metadata['timed_out'])
        self.assertTrue(metadata['seed_unchanged'])
        self.assertTrue(metadata['cleanup']['complete'])
        self.assertEqual(metadata['cleanup']['remaining_live_pids'], [])
        self.assertEqual(baseline.live_group_members(metadata['pid']), [])
        check = metadata['disk_space_checks']['during_capture']
        self.assertFalse(check['accepted'])
        self.assertEqual(check['headroom_bytes'],
                         baseline.DISK_WATCHDOG_HEADROOM_BYTES)

    def test_disk_watchdog_checks_at_most_once_per_second(self):
        clock = [0.0]
        checks = []

        def disk_usage(path):
            checks.append(clock[0])
            return SimpleNamespace(free=100 * 1024 ** 3)

        def sleep(seconds):
            clock[0] += seconds

        self.disk_usage.side_effect = disk_usage
        metadata = {'run_directory': str(self.root), 'disk_space_checks': {}}
        process = mock.Mock(pid=12345)
        process.poll.return_value = None
        with mock.patch.object(baseline.time, 'monotonic',
                               side_effect=lambda: clock[0]), mock.patch.object(
                baseline.time, 'sleep', side_effect=sleep), mock.patch.object(
                baseline, 'live_group_members', return_value=[12345]):
            self.assertEqual(baseline.watch_group(
                process, 3.5, [None], metadata), 'timeout')
        self.assertEqual(len(checks), 3)
        self.assertGreaterEqual(checks[0], 1.0)
        for first, second in zip(checks, checks[1:]):
            self.assertGreaterEqual(second - first, 1.0)

    def test_reject_changed_seed_and_symlinks(self):
        (self.seed / 'Account').write_bytes(b'changed')
        with contextlib.redirect_stderr(io.StringIO()):
            with self.assertRaises(SystemExit):
                self.run_fake()
        self.assertFalse(self.output.exists())
        (self.source / 'link').symlink_to(self.source / 'Account')
        with contextlib.redirect_stderr(io.StringIO()):
            with self.assertRaises(SystemExit):
                self.invoke('prepare-seed', '--source', self.source,
                            '--destination', self.root / 'linked-seed')
        self.assertFalse((self.root / 'linked-seed').exists())

    def test_timeout_stops_descendants_and_records_evidence(self):
        self.write_fake('''
import pathlib, signal, subprocess, sys, time
signal.signal(signal.SIGINT, signal.SIG_IGN)
signal.signal(signal.SIGTERM, signal.SIG_IGN)
child = subprocess.Popen([sys.executable, '-c',
    'import signal,time; signal.signal(signal.SIGINT,signal.SIG_IGN); '
    'signal.signal(signal.SIGTERM,signal.SIG_IGN); time.sleep(60)'])
pathlib.Path('child.pid').write_text(str(child.pid))
print('XHttp: no host HTTP client on this platform', flush=True)
time.sleep(60)
''')
        started = time.monotonic()
        self.assertEqual(self.run_fake('--timeout', '0.3'), 124)
        self.assertLess(time.monotonic() - started, 4)
        run = next(self.output.iterdir())
        metadata = json.loads((run / 'metadata.json').read_text())
        self.assertTrue(metadata['timed_out'])
        self.assertEqual(metadata['exit_code'], -9)
        self.assertEqual(metadata['evidence']['http']['count'], 1)
        pid = int((run / 'child.pid').read_text())
        if Path('/proc').exists():
            status = Path(f'/proc/{pid}/stat')
            if status.exists():
                self.assertEqual(status.read_text().split()[2], 'Z')
        else:
            completed = baseline.subprocess.run(
                ['ps', '-p', str(pid), '-o', 'state='],
                text=True, capture_output=True)
            self.assertIn(completed.stdout.strip(), ('', 'Z'))

    def test_evidence_samples_are_bounded_and_summary_retains_outcome(self):
        log = self.root / 'synthetic.log'
        log.write_text('Unimplemented export Example\n' * 100)
        evidence = baseline.extract_evidence([log], sample_limit=3)
        self.assertEqual(evidence['exports']['count'], 100)
        self.assertEqual(len(evidence['exports']['samples']), 3)
        self.assertEqual(self.run_fake(), 0)
        summary = self.root / 'summary.json'
        self.assertEqual(self.invoke('summarize', '--runs-root', self.output,
                                     '--output', summary), 0)
        result = json.loads(summary.read_text())
        self.assertEqual(len(result['runs']), 1)
        self.assertTrue(result['runs'][0]['seed_unchanged'])

    def test_runtime_environment_is_recorded_without_unrelated_values(self):
        hints = {
            'SDL_GAMECONTROLLER_IGNORE_DEVICES': '0x10f5/0x7126',
            'SDL_JOYSTICK_ALLOW_BACKGROUND_EVENTS': '1',
            'VK_ICD_FILENAMES': '/research/example-icd.json',
            'DRI_PRIME': '1', 'LIBGL_ALWAYS_SOFTWARE': '1',
            'APPIMAGELAUNCHER_DISABLE': '1', 'APPIMAGE_EXTRACT_AND_RUN': '1',
            'MANGOHUD': '1', 'MANGOHUD_CONFIGFILE': '/research/MangoHud.conf',
            'DISABLE_MANGOHUD': '1',
            'EXAMPLE_API_SECRET': 'must-not-be-captured',
        }
        with mock.patch.dict(os.environ, hints):
            self.assertEqual(self.run_fake(), 0)
        run = next(self.output.iterdir())
        metadata = json.loads((run / 'metadata.json').read_text())
        captured = metadata['inherited_runtime_environment']
        for name, value in hints.items():
            if name == 'EXAMPLE_API_SECRET':
                self.assertNotIn(name, captured)
            else:
                self.assertEqual(captured[name], value)
        self.assertNotIn('must-not-be-captured',
                         (run / 'metadata.json').read_text())

    def test_relaunch_survives_leader_exit_until_capture_deadline(self):
        self.write_fake('''
import os, pathlib, subprocess, sys
child_code = """
import os, pathlib, sys, time
parent = int(sys.argv[1])
while True:
    try:
        os.kill(parent, 0)
    except ProcessLookupError:
        break
    time.sleep(0.01)
time.sleep(1)
pathlib.Path('replacement-ready').write_text('ready after parent reaped')
time.sleep(60)
"""
child = subprocess.Popen([sys.executable, '-c', child_code, str(os.getpid())])
pathlib.Path('child.pid').write_text(str(child.pid))
''')
        self.assertEqual(self.run_fake('--timeout', '1.7'), 124)
        run = next(self.output.iterdir())
        self.assertTrue((run / 'replacement-ready').exists())
        metadata = json.loads((run / 'metadata.json').read_text())
        self.assertTrue(metadata['handoff_observed'])
        self.assertEqual(metadata['leader_exit_code'], 0)
        self.assertIsNone(metadata['exit_code'])
        self.assertIsNone(metadata['descendant_exit_code'])
        self.assertEqual(metadata['exit_code_scope'], 'unknown_after_handoff')
        self.assertEqual(metadata['cleanup']['remaining_live_pids'], [])

    def test_finished_replacement_exit_is_unknown_not_leader_success(self):
        self.write_fake('''
import subprocess, sys
subprocess.Popen([sys.executable, '-c',
    'import time; time.sleep(0.3); raise SystemExit(7)'])
''')
        started = time.monotonic()
        self.assertEqual(self.run_fake(), 3)
        self.assertLess(time.monotonic() - started, 3)
        run = next(self.output.iterdir())
        metadata = json.loads((run / 'metadata.json').read_text())
        self.assertEqual(metadata['leader_exit_code'], 0)
        self.assertIsNone(metadata['exit_code'])
        self.assertFalse(metadata['timed_out'])
        self.assertEqual(metadata['completion_reason'], 'group_finished')

    def test_sigterm_cleans_child_session_and_finishes_metadata(self):
        self.write_fake('''
import pathlib, signal, subprocess, sys, time
signal.signal(signal.SIGINT, signal.SIG_IGN)
signal.signal(signal.SIGTERM, signal.SIG_IGN)
child = subprocess.Popen([sys.executable, '-c',
    'import signal,time; signal.signal(signal.SIGINT,signal.SIG_IGN); '
    'signal.signal(signal.SIGTERM,signal.SIG_IGN); time.sleep(60)'])
pathlib.Path('child.pid').write_text(str(child.pid))
time.sleep(60)
''')
        # Keep this subprocess lifecycle test independent of host free space;
        # there is deliberately no production flag or environment bypass.
        bootstrap = '''
import runpy, sys
from types import SimpleNamespace
from unittest import mock
tool = sys.argv.pop(1)
with mock.patch('shutil.disk_usage',
                return_value=SimpleNamespace(free=100 * 1024 ** 3)):
    runpy.run_path(tool, run_name='__main__')
'''
        command = [sys.executable, '-c', bootstrap,
                   str(Path(baseline.__file__)), 'run',
                   '--binary', str(self.fake), '--seed', str(self.seed),
                   '--output-root', str(self.output), '--scenario', 'term',
                   '--timeout', '20', '--grace', '0.2']
        runner = subprocess.Popen(command, stdout=subprocess.PIPE,
                                  stderr=subprocess.PIPE, text=True)
        run = None
        try:
            run = Path(runner.stdout.readline().strip())
            deadline = time.monotonic() + 4
            while not (run / 'child.pid').exists():
                if time.monotonic() >= deadline or runner.poll() is not None:
                    self.fail('Fake emulator did not become ready')
                time.sleep(0.02)
            runner.send_signal(signal.SIGTERM)
            unused_stdout, stderr = runner.communicate(timeout=5)
            self.assertEqual(runner.returncode, 143, stderr)
            metadata = json.loads((run / 'metadata.json').read_text())
            self.assertTrue(metadata['capture_complete'])
            self.assertEqual(metadata['termination_signal'], 15)
            self.assertEqual(metadata['completion_reason'], 'signal')
            self.assertTrue(metadata['cleanup']['complete'])
            self.assertEqual(baseline.live_group_members(metadata['pid']), [])
        finally:
            if runner.poll() is None:
                runner.kill()
                runner.communicate(timeout=2)
            if run and (run / 'metadata.json').exists():
                metadata = json.loads((run / 'metadata.json').read_text())
                if metadata.get('pid') and baseline.live_group_members(metadata['pid']):
                    os.killpg(metadata['pid'], signal.SIGKILL)

    def test_capture_restores_existing_signal_handlers(self):
        previous = {sig: signal.getsignal(sig)
                    for sig in (signal.SIGINT, signal.SIGTERM)}
        self.assertEqual(self.run_fake(), 0)
        self.assertEqual(previous, {sig: signal.getsignal(sig)
                                    for sig in previous})


if __name__ == '__main__':
    unittest.main()
