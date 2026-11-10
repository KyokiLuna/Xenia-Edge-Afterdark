#!/usr/bin/env python3
"""Capture isolated, repeatable Xenia baseline runs using only the stdlib."""

import argparse
import datetime
import hashlib
import json
import os
from pathlib import Path
import plistlib
import re
import shutil
import signal
import stat
import subprocess
import sys
import tempfile
import time


SEED_MARKER = '.baseline-seed.json'
SHARED_SYSTEM_KIND = 'xenia-readonly-installed-system-source'
SHARED_SYSTEM_OPTION = 'system_readonly_source_root'
PROTECTED_STORAGE_OPTION = 'host_storage_protected_root'
DISK_FREE_FLOOR_BYTES = 20 * 1024 ** 3
DISK_HEADROOM_BYTES = 1024 ** 3
DISK_WATCHDOG_HEADROOM_BYTES = 512 * 1024 ** 2
DISK_WATCHDOG_INTERVAL_SECONDS = 1.0
RESERVED = {
    'storage_root', 'content_root', 'cache_root', 'log_file', 'log_append',
    'config', 'target', 'portable', 'allow_game_relative_writes',
    'allow_plugins', 'flagfile', 'help',
    SHARED_SYSTEM_OPTION, PROTECTED_STORAGE_OPTION,
}
EVIDENCE = {
    'title': r'title.?id|title name|launching|launchpath|loaded.*\.xex',
    'exports': r'unimplemented.*(?:export|import)|unresolved.*(?:export|import)|'
               r'undefined extern|function.*not implemented',
    'stubs': r'\bstub\b|unimplemented|not implemented',
    'http': r'xhttp|winhttp|https|no host HTTP client|cannot.connect',
    'missing_host_http': r'no host HTTP client on this platform',
    'failure': r'\b(?:error|failed|failure|assert|exception|crash|fatal)\b',
}
ENVIRONMENT_PREFIXES = ('SDL_', 'VK_', 'LIBGL_', 'MANGOHUD_')
ENVIRONMENT_NAMES = {
    'DRI_PRIME', 'APPIMAGELAUNCHER_DISABLE', 'APPIMAGE_EXTRACT_AND_RUN',
    'MANGOHUD', 'DISABLE_MANGOHUD',
}


def runtime_environment(environment):
    """Capture relevant graphics/input hints without dumping the environment."""
    return {name: environment[name] for name in sorted(environment)
            if name.startswith(ENVIRONMENT_PREFIXES) or
            name in ENVIRONMENT_NAMES}


def utc_now():
    return datetime.datetime.now(datetime.timezone.utc).isoformat()


def write_json(path, value):
    path.write_text(json.dumps(value, indent=2) + '\n', encoding='utf-8')


def file_hash(path):
    digest = hashlib.sha256()
    with path.open('rb') as source:
        for chunk in iter(lambda: source.read(1024 * 1024), b''):
            digest.update(chunk)
    return digest.hexdigest()


def tree_manifest(root, *, exclude_system=False, include_seed_marker=False):
    """Reject links so a copied seed cannot alias personal data."""
    entries = []
    for path in sorted(root.rglob('*')):
        if exclude_system and path.relative_to(root).parts[0] == 'system':
            continue
        if path.is_symlink():
            raise ValueError(f'Seed contains a symbolic link: {path}')
        if path.is_file() and (include_seed_marker or path.name != SEED_MARKER):
            entries.append({
                'path': path.relative_to(root).as_posix(),
                'size': path.stat().st_size,
                'sha256': file_hash(path),
            })
        elif not path.is_dir() and not path.is_file():
            raise ValueError(f'Seed contains a non-regular entry: {path}')
    digest = hashlib.sha256(json.dumps(entries, sort_keys=True).encode())
    return {'sha256': digest.hexdigest(), 'files': entries}


def is_within(path, parent):
    return path == parent or parent in path.parents


def shared_system_snapshot(root):
    """Record a canonical source, including empty partition directories."""
    if not root.is_absolute() or root != root.resolve(strict=True):
        raise ValueError('Shared system root must be a canonical absolute path')
    identity = root.lstat()
    if not stat.S_ISDIR(identity.st_mode):
        raise ValueError('Shared system root must be a real directory')
    tree = tree_manifest(root, include_seed_marker=True)
    directories = ['.'] + [path.relative_to(root).as_posix()
                           for path in sorted(root.rglob('*')) if path.is_dir()]
    after = root.lstat()
    if (identity.st_dev, identity.st_ino) != (after.st_dev, after.st_ino):
        raise ValueError('Shared system root changed during inventory')
    return {'root': str(root),
            'root_identity': {'dev': identity.st_dev, 'ino': identity.st_ino},
            'tree': tree, 'directories': directories}


def load_shared_system(path, expected_sha256):
    if not re.fullmatch(r'[0-9a-f]{64}', expected_sha256 or ''):
        raise ValueError('Shared system manifest requires an exact SHA-256 pin')
    path = path.expanduser().absolute()
    if path.is_symlink() or not path.is_file():
        raise ValueError('Shared system manifest must be a regular file')
    with path.open('rb') as source:
        raw = source.read(4 * 1024 ** 2 + 1)
    if len(raw) > 4 * 1024 ** 2 or hashlib.sha256(raw).hexdigest() != expected_sha256:
        raise ValueError('Shared system manifest size or SHA-256 pin mismatch')
    manifest = json.loads(raw)
    if (not isinstance(manifest, dict) or
            type(manifest.get('schema')) is not int or manifest['schema'] != 1 or
            manifest.get('kind') != SHARED_SYSTEM_KIND or
            not isinstance(manifest.get('root'), str)):
        raise ValueError('Unsupported shared system manifest')
    snapshot = shared_system_snapshot(Path(manifest['root']))
    if any(manifest.get(key) != value for key, value in snapshot.items()):
        raise ValueError('Shared system source no longer matches its manifest')
    return {'manifest': str(path.resolve()), 'manifest_sha256': expected_sha256,
            **snapshot}


def check_shared_system(snapshot):
    current = load_shared_system(Path(snapshot['manifest']),
                                 snapshot['manifest_sha256'])
    if current != snapshot:
        raise ValueError('Shared system source generation changed')


def prepare_shared_system(args):
    root = args.source.expanduser().resolve(strict=True)
    requested = args.destination.expanduser().absolute()
    destination = requested.resolve()
    if destination.exists() or requested.is_symlink() or is_within(destination, root):
        raise ValueError('Manifest must be a new file outside its system source')
    snapshot = shared_system_snapshot(root)
    require_disk_space(destination.parent, 'recording shared system source')
    destination.parent.mkdir(parents=True, exist_ok=True)
    with destination.open('x', encoding='utf-8') as output:
        json.dump({'schema': 1, 'kind': SHARED_SYSTEM_KIND, **snapshot}, output,
                  indent=2)
        output.write('\n')
    print(destination)
    return 0


def reject_unpinned_system_config(seed):
    # Defaults must keep working with older official/Guide binaries which do
    # not recognize the new CLI option. Reject an unpinned configured source
    # rather than sending an unknown empty override to those binaries.
    for name in ('xenia-edge.config.toml', 'xenia.config.toml',
                 'xenia_edge.config.toml', 'config.toml'):
        path = seed / name
        if not path.is_file():
            continue
        raw = path.read_bytes()
        if SHARED_SYSTEM_OPTION.encode() not in raw:
            continue
        try:
            import tomllib
        except ImportError as error:
            raise ValueError('Python 3.11+ is required to validate shared-source configuration') from error
        pending = [tomllib.loads(raw.decode('utf-8'))]
        while pending:
            item = pending.pop()
            if isinstance(item, dict):
                if SHARED_SYSTEM_OPTION in item and item[SHARED_SYSTEM_OPTION] != '':
                    raise ValueError('Configured shared system source requires a pinned manifest')
                pending.extend(item.values())
            elif isinstance(item, list):
                pending.extend(item)


class DiskSpaceError(OSError):
    def __init__(self, check):
        self.check = check
        super().__init__(
            f'Insufficient free disk space before {check["operation"]}: '
            f'{check["free_bytes"]} bytes available on '
            f'{check["filesystem_path"]}; must exceed '
            f'{check["required_free_bytes"]} bytes '
            '(20 GiB floor + '
            f'{check["headroom_bytes"] // 1024 ** 2} MiB headroom '
            '+ estimated copy allocation)')


def require_disk_space(destination, operation, copy_manifest=None,
                       copy_source=None, headroom_bytes=DISK_HEADROOM_BYTES,
                       exclude_system=False):
    """Reserve a strict floor, run headroom and a potentially dense seed copy."""
    probe = destination
    while not probe.exists():
        probe = probe.parent
    estimated_copy_bytes = 0
    if copy_manifest is not None:
        filesystem = os.statvfs(probe)
        block = filesystem.f_frsize or filesystem.f_bsize or 4096
        directories = {Path('.')}
        for entry in copy_manifest['files']:
            size = entry['size']
            estimated_copy_bytes += ((size + block - 1) // block) * block
            directories.update(Path(entry['path']).parents)
        directory_count = (1 + sum(path.is_dir() for path in copy_source.rglob('*')
                                   if not exclude_system or
                                   path.relative_to(copy_source).parts[0] != 'system')
                           if copy_source is not None else len(directories))
        estimated_copy_bytes += directory_count * block
    required = DISK_FREE_FLOOR_BYTES + headroom_bytes + estimated_copy_bytes
    free = shutil.disk_usage(probe).free
    check = {
        'operation': operation, 'filesystem_path': str(probe),
        'free_bytes': free, 'floor_bytes': DISK_FREE_FLOOR_BYTES,
        'headroom_bytes': headroom_bytes, 'checked_utc': utc_now(),
        'estimated_copy_bytes': estimated_copy_bytes,
        'required_free_bytes': required, 'accepted': free > required,
    }
    if not check['accepted']:
        raise DiskSpaceError(check)
    return check


def prepare_seed(args):
    destination = args.destination.expanduser().resolve()
    if destination.exists():
        raise ValueError(f'Seed destination already exists: {destination}')
    source = args.source.expanduser().resolve() if args.source else None
    if source:
        if not source.is_dir():
            raise ValueError(f'Seed source is not a directory: {source}')
        if is_within(destination, source):
            raise ValueError('Seed destination must be outside its source')
        manifest = tree_manifest(source)
        require_disk_space(destination, 'copying prepared seed', manifest,
                           source)
        shutil.copytree(source, destination,
                        ignore=shutil.ignore_patterns(SEED_MARKER))
    else:
        require_disk_space(destination, 'creating empty seed')
        destination.mkdir(parents=True)
    manifest = tree_manifest(destination)
    write_json(destination / SEED_MARKER, {
        'schema': 1, 'created_utc': utc_now(),
        'source': str(source) if source else None, **manifest,
    })
    print(destination)
    return 0


def validate_args(arguments):
    for argument in arguments:
        match = re.fullmatch(r'--([a-z][a-z0-9_]*)=([^\x00]*)', argument)
        if not match or match.group(1) in RESERVED:
            raise ValueError('Extra arguments must use --name=value and '
                             f'cannot override isolation: {argument!r}')
    return arguments


def executable(path):
    path = path.expanduser().resolve()
    if path.suffix == '.app' and path.is_dir():
        with (path / 'Contents' / 'Info.plist').open('rb') as source:
            name = plistlib.load(source)['CFBundleExecutable']
        path = (path / 'Contents' / 'MacOS' / name).resolve()
    if not path.is_file() or not os.access(path, os.X_OK):
        raise ValueError(f'Binary is not an executable file: {path}')
    return path


def live_group_members(pgid):
    """Find running group members, ignoring unreapable descendant zombies."""
    members = []
    if sys.platform.startswith('linux') and Path('/proc').is_dir():
        for entry in Path('/proc').iterdir():
            if not entry.name.isdecimal():
                continue
            try:
                # comm may itself contain spaces or parentheses.
                fields = (entry / 'stat').read_text().rsplit(')', 1)[1].split()
                if int(fields[2]) == pgid and fields[0] not in ('Z', 'X'):
                    members.append(int(entry.name))
            except (OSError, ValueError, IndexError):
                continue  # The process can exit during enumeration.
    else:
        result = subprocess.run(['ps', '-axo', 'pid=,pgid=,stat='],
                                capture_output=True, text=True,
                                check=True, timeout=2)
        for line in result.stdout.splitlines():
            fields = line.split()
            if (len(fields) >= 3 and int(fields[1]) == pgid and
                    fields[2][0] not in ('Z', 'X')):
                members.append(int(fields[0]))
    return sorted(members)


def watch_group(process, deadline, requested_signal, metadata):
    """Reap the launcher promptly, then keep its same-group handoff alive."""
    descendants = set()
    next_disk_check = time.monotonic() + DISK_WATCHDOG_INTERVAL_SECONDS
    while True:
        # A replacement may wait for the original PID to disappear, so poll
        # must reap the leader before inspecting the remaining group.
        leader_exit = process.poll()
        members = live_group_members(process.pid)
        descendants.update(pid for pid in members if pid != process.pid)
        metadata['observed_descendant_pids'] = sorted(descendants)
        if leader_exit is not None:
            metadata['leader_exit_code'] = leader_exit
            if members:
                metadata['handoff_observed'] = True
        if requested_signal[0] is not None:
            return 'signal'
        if leader_exit is not None and not members:
            return 'group_finished'
        now = time.monotonic()
        if now >= deadline:
            return 'timeout'
        if now >= next_disk_check:
            try:
                check = require_disk_space(
                    Path(metadata['run_directory']), 'continuing capture',
                    headroom_bytes=DISK_WATCHDOG_HEADROOM_BYTES)
            except DiskSpaceError as error:
                metadata['disk_space_checks']['during_capture'] = error.check
                metadata['disk_space_error'] = str(error)
                return 'low_disk_space'
            except OSError as error:
                # A failed capacity probe cannot authorize continued writes.
                metadata['disk_space_checks']['during_capture'] = {
                    'accepted': False, 'error': str(error),
                    'checked_utc': utc_now(),
                }
                metadata['disk_space_error'] = (
                    f'Cannot check capture disk space: {error}')
                return 'low_disk_space'
            metadata['disk_space_checks']['during_capture'] = check
            next_disk_check = now + DISK_WATCHDOG_INTERVAL_SECONDS
        time.sleep(min(0.1, max(0, deadline - time.monotonic())))


def stop_group(process, grace):
    """Stop the whole session, including title relaunches and child workers."""
    signals_sent = []
    for sig in (signal.SIGINT, signal.SIGTERM, signal.SIGKILL):
        process.poll()
        if not live_group_members(process.pid):
            break
        try:
            os.killpg(process.pid, sig)
        except ProcessLookupError:
            break
        signals_sent.append(int(sig))
        deadline = time.monotonic() + (grace if sig != signal.SIGKILL else 1)
        while time.monotonic() < deadline:
            process.poll()
            if not live_group_members(process.pid):
                break
            time.sleep(0.05)
    try:
        process.wait(timeout=1)
    except subprocess.TimeoutExpired:
        pass
    remaining = live_group_members(process.pid)
    return {'signals_sent': signals_sent, 'remaining_live_pids': remaining,
            'complete': not remaining, 'leader_reaped': process.poll() is not None}


def extract_evidence(paths, sample_limit=12):
    patterns = {key: re.compile(value, re.I)
                for key, value in EVIDENCE.items()}
    result = {key: {'count': 0, 'samples': []} for key in patterns}
    for path in paths:
        if not path.is_file():
            continue
        with path.open('r', encoding='utf-8', errors='replace') as source:
            for line_number, text in enumerate(source, 1):
                for category, pattern in patterns.items():
                    if pattern.search(text):
                        result[category]['count'] += 1
                        samples = result[category]['samples']
                        if len(samples) < sample_limit:
                            samples.append({'file': path.name,
                                            'line': line_number,
                                            'text': text.rstrip()[:1000]})
    return result


def run_capture(args):
    if os.name != 'posix':
        raise ValueError('Capture runner supports native Linux/macOS only')
    if not re.fullmatch(r'[a-zA-Z0-9][a-zA-Z0-9_.-]{0,79}', args.scenario):
        raise ValueError('Scenario must be a short name without path separators')
    if args.timeout <= 0 or args.grace <= 0:
        raise ValueError('Timeout and grace must be positive')
    extra_args = validate_args(args.emulator_arg)
    binary = executable(args.binary)
    seed = args.seed.expanduser().resolve()
    marker = seed / SEED_MARKER
    if not marker.is_file() or marker.is_symlink():
        raise ValueError('Seed must be created with prepare-seed first')
    seed_info = json.loads(marker.read_text(encoding='utf-8'))
    before = tree_manifest(seed)
    if before['sha256'] != seed_info['sha256']:
        raise ValueError('Seed changed after preparation; prepare a new seed')
    shared = None
    shared_path = getattr(args, 'shared_system_manifest', None)
    shared_pin = getattr(args, 'shared_system_manifest_sha256', None)
    if shared_path is not None:
        shared = load_shared_system(shared_path, shared_pin)
        if (seed / 'system').exists() and not (seed / 'system').is_dir():
            raise ValueError('Only a real seed system directory may be excluded')
    elif shared_pin:
        raise ValueError('Shared system SHA-256 pin requires its manifest')
    else:
        reject_unpinned_system_config(seed)
    protected_root = getattr(args, 'protected_filesystem_root', None)
    protected_identity = None
    if protected_root is not None:
        protected_root = protected_root.expanduser().resolve(strict=True)
        st = protected_root.stat()
        if not stat.S_ISDIR(st.st_mode):
            raise ValueError('Protected filesystem root must be an existing directory')
        if shutil.disk_usage(protected_root).free < DISK_FREE_FLOOR_BYTES + 65536:
            raise ValueError('Explicit protected filesystem has insufficient reserve')
        protected_identity = {'root': str(protected_root),
                              'device': st.st_dev, 'inode': st.st_ino,
                              'reserve_bytes': DISK_FREE_FLOOR_BYTES}
    copy_before = tree_manifest(seed, exclude_system=shared is not None)
    output_root = args.output_root.expanduser().resolve()
    if is_within(output_root, seed):
        raise ValueError('Run output must be outside the seed')
    if shared and (is_within(output_root, Path(shared['root'])) or
                   is_within(Path(shared['root']), output_root)):
        raise ValueError('Run output and shared system source must not overlap')
    target = args.target.expanduser().resolve() if args.target else None
    if target and (not target.is_file() or is_within(target, seed)):
        raise ValueError('Target must be a file outside the seed')
    copy_space = require_disk_space(output_root, 'copying capture seed', copy_before,
                                    seed, exclude_system=shared is not None)
    output_root.mkdir(parents=True, exist_ok=True)
    stamp = datetime.datetime.now(datetime.timezone.utc).strftime('%Y%m%dT%H%M%SZ')
    run = Path(tempfile.mkdtemp(prefix=f'{stamp}-{args.scenario}-',
                                dir=output_root))
    data = run / 'data'
    def ignore_seed(parent, names):
        return [name for name in names if name == SEED_MARKER or
                (shared is not None and Path(parent) == seed and name == 'system')]
    shutil.copytree(seed, data, ignore=ignore_seed)
    if tree_manifest(data) != copy_before:
        raise ValueError(f'Seed copy verification failed: {run}')
    stdout = run / 'stdout.log'
    log = run / 'xenia.log'
    argv = [str(binary),
            f'--storage_root={data}',
            f'--content_root={data / "content"}',
            f'--cache_root={data / "cache_host"}',
            f'--log_file={log}', '--log_to_stdout=true', '--flush_log=true',
            '--allow_game_relative_writes=false', '--allow_plugins=false',
            *([f"--{SHARED_SYSTEM_OPTION}={shared['root']}"] if shared else []),
            *([f'--{PROTECTED_STORAGE_OPTION}={protected_root}']
              if protected_root is not None else []),
            *extra_args]
    if target:
        argv.append(str(target))
    environment = dict(os.environ)
    isolated_environment = {
        key: str(run / 'xdg' / suffix)
        for key, suffix in [('XDG_DATA_HOME', 'data'),
                            ('XDG_CONFIG_HOME', 'config'),
                            ('XDG_CACHE_HOME', 'cache')]
    }
    environment.update(isolated_environment)
    for path in isolated_environment.values():
        Path(path).mkdir(parents=True)
    metadata = {
        'schema': 1, 'scenario': args.scenario, 'started_utc': utc_now(),
        'run_directory': str(run), 'binary': str(binary),
        'binary_sha256': file_hash(binary), 'seed': str(seed),
        'capture_tool': {'path': str(Path(__file__).resolve()),
                         'sha256': file_hash(Path(__file__))},
        'seed_sha256': before['sha256'], 'target': str(target) if target else None,
        'seed_copy_sha256': copy_before['sha256'],
        'system_copy_excluded': shared is not None,
        'shared_system_source': shared,
        'protected_filesystem_policy': protected_identity,
        'target_size': target.stat().st_size if target else None,
        'argv': argv, 'cwd': str(run),
        'environment_overrides': isolated_environment,
        'inherited_runtime_environment': runtime_environment(os.environ),
        'timeout_seconds': args.timeout, 'timed_out': False,
        'interrupted': False, 'termination_signal': None,
        'exit_code': None, 'exit_code_scope': 'leader_only',
        'leader_exit_code': None, 'descendant_exit_code': None,
        'handoff_observed': False, 'observed_descendant_pids': [],
        'capture_complete': False,
        'disk_space_checks': {'before_copy': copy_space},
    }
    write_json(run / 'metadata.json', metadata)
    print(run, flush=True)
    started = time.monotonic()
    process = None
    requested_signal = [None]
    old_handlers = {}

    def request_stop(signum, frame):
        # Do not raise during Popen or cleanup; finish the metadata either way.
        if requested_signal[0] is None:
            requested_signal[0] = signum

    try:
        for signum in (signal.SIGINT, signal.SIGTERM):
            old_handlers[signum] = signal.signal(signum, request_stop)
        with stdout.open('wb') as output:
            if requested_signal[0] is None:
                if shared:
                    check_shared_system(shared)
                metadata['disk_space_checks']['before_launch'] = (
                    require_disk_space(run, 'launching emulator'))
                process = subprocess.Popen(
                    argv, cwd=run, env=environment, stdout=output,
                    stderr=subprocess.STDOUT, start_new_session=True)
                metadata['pid'] = process.pid
            try:
                write_json(run / 'metadata.json', metadata)
                metadata['completion_reason'] = (
                    watch_group(process, started + args.timeout,
                                requested_signal, metadata)
                    if process else 'signal')
            except KeyboardInterrupt:
                requested_signal[0] = signal.SIGINT
                metadata['completion_reason'] = 'signal'
            finally:
                if process:
                    metadata['cleanup'] = stop_group(process, args.grace)
                    metadata['leader_exit_code'] = process.returncode
                    if metadata['handoff_observed']:
                        # Orphaned replacements are not waitable children of
                        # this runner. Their eventual exit status is unknown.
                        metadata['exit_code_scope'] = 'unknown_after_handoff'
                    else:
                        metadata['exit_code'] = process.returncode
    except DiskSpaceError as error:
        metadata['disk_space_checks']['before_launch'] = error.check
        metadata['disk_space_error'] = str(error)
        metadata['completion_reason'] = 'low_disk_space'
    except OSError as error:
        metadata['launch_error'] = str(error)
    except ValueError as error:
        if shared is None:
            raise
        metadata['shared_system_integrity_error'] = str(error)
    finally:
        try:
            metadata['timed_out'] = metadata.get('completion_reason') == 'timeout'
            metadata['termination_signal'] = requested_signal[0]
            metadata['interrupted'] = requested_signal[0] is not None
            metadata['duration_seconds'] = round(time.monotonic() - started, 3)
            metadata['finished_utc'] = utc_now()
            try:
                metadata['seed_unchanged'] = tree_manifest(seed) == before
            except (OSError, ValueError) as error:
                metadata['seed_unchanged'] = False
                metadata['seed_integrity_error'] = str(error)
            if shared:
                try:
                    check_shared_system(shared)
                    metadata['shared_system_unchanged'] = True
                except (OSError, ValueError, KeyError) as error:
                    metadata['shared_system_unchanged'] = False
                    metadata['shared_system_integrity_error'] = str(error)
            metadata['evidence'] = extract_evidence([log, stdout])
            metadata['log_files'] = {path.name: path.stat().st_size
                                     for path in (log, stdout) if path.exists()}
            metadata['capture_complete'] = True
            write_json(run / 'metadata.json', metadata)
        finally:
            for signum, handler in old_handlers.items():
                signal.signal(signum, handler)
    if not metadata['seed_unchanged']:
        return 2
    if metadata.get('shared_system_integrity_error'):
        return 2
    if metadata.get('disk_space_error'):
        return 2
    if metadata.get('launch_error'):
        return 127
    if metadata['interrupted']:
        return 128 + metadata['termination_signal']
    if metadata['timed_out']:
        return 124
    if metadata['exit_code_scope'] == 'unknown_after_handoff':
        return 3
    exit_code = metadata['exit_code']
    if exit_code is None:
        return 2
    return exit_code if exit_code >= 0 else 128 - exit_code


def summarize(args):
    runs = []
    runs_root = args.runs_root.expanduser().resolve()
    for path in sorted(runs_root.glob('*/metadata.json')):
        capture = json.loads(path.read_text(encoding='utf-8'))
        runs.append(capture)
    result = {'schema': 1, 'created_utc': utc_now(), 'runs': runs,
              'note': 'Pattern matches are observations, not diagnoses; '
                      'a timeout does not prove the title was stalled.'}
    if args.output:
        output = args.output.expanduser().resolve()
        output.parent.mkdir(parents=True, exist_ok=True)
        write_json(output, result)
    else:
        print(json.dumps(result, indent=2))
    return 0


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    commands = parser.add_subparsers(dest='command', required=True)
    prepare = commands.add_parser('prepare-seed')
    prepare.add_argument('--source', type=Path)
    prepare.add_argument('--destination', required=True, type=Path)
    prepare.set_defaults(function=prepare_seed)
    shared = commands.add_parser('prepare-shared-system')
    shared.add_argument('--source', required=True, type=Path)
    shared.add_argument('--destination', required=True, type=Path)
    shared.set_defaults(function=prepare_shared_system)
    run = commands.add_parser('run')
    run.add_argument('--binary', required=True, type=Path)
    run.add_argument('--seed', required=True, type=Path)
    run.add_argument('--output-root', required=True, type=Path)
    run.add_argument('--scenario', required=True)
    run.add_argument('--target', type=Path)
    run.add_argument('--timeout', type=float, default=60)
    run.add_argument('--grace', type=float, default=3)
    run.add_argument('--emulator-arg', action='append', default=[])
    run.add_argument('--protected-filesystem-root', type=Path,
                     help='Explicit existing research-project filesystem root; '
                          'passes a process-only 20GiB native storage policy')
    run.add_argument('--shared-system-manifest', type=Path)
    run.add_argument('--shared-system-manifest-sha256')
    run.set_defaults(function=run_capture)
    summary = commands.add_parser('summarize')
    summary.add_argument('--runs-root', required=True, type=Path)
    summary.add_argument('--output', type=Path)
    summary.set_defaults(function=summarize)
    args = parser.parse_args(argv)
    try:
        return args.function(args)
    except (ValueError, OSError, KeyError) as error:
        parser.error(str(error))


if __name__ == '__main__':
    sys.exit(main())
