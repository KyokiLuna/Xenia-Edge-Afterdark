#!/usr/bin/env python3
"""Linux-only baseline wrapper; exec absolute XENIA_RESEARCH_BINARY unchanged."""
import ctypes
import datetime
import hashlib
import json
import os
from pathlib import Path
import sys
import time

PR_SET_PTRACER = 0x59616D61


def digest(path):
    value = hashlib.sha256()
    with path.open('rb') as source:
        for chunk in iter(lambda: source.read(1024 * 1024), b''):
            value.update(chunk)
    return value.hexdigest()


def main():
    if not sys.platform.startswith('linux'):
        raise ValueError('This wrapper requires Linux')
    raw = os.environ.get('XENIA_RESEARCH_BINARY', '')
    if not raw or not Path(raw).is_absolute():
        raise ValueError('XENIA_RESEARCH_BINARY must name an absolute executable')
    binary = Path(raw).resolve(strict=True)
    if not binary.is_file() or not os.access(binary, os.X_OK) or binary == Path(__file__).resolve():
        raise ValueError('Native binary must be an executable other than this wrapper')
    record_name = os.environ.get('XENIA_RESEARCH_WRAPPER_RECORD')
    if record_name:
        record = Path(record_name)
    else:
        logs = [a.split('=', 1)[1] for a in sys.argv[1:] if a.startswith('--log_file=')]
        if len(logs) != 1 or not Path(logs[0]).is_absolute():
            raise ValueError('Need one absolute --log_file or XENIA_RESEARCH_WRAPPER_RECORD')
        record = Path(logs[0]).parent / 'frame-wrapper.json'
    if not record.is_absolute():
        raise ValueError('Wrapper record must be an absolute new output path')
    record.parent.mkdir(parents=True, exist_ok=True)
    # Reserve the provenance file before changing this process's ptracer policy.
    with record.open('x', encoding='utf-8') as output:
        libc = ctypes.CDLL(None, use_errno=True)
        libc.prctl.argtypes = [ctypes.c_int] + [ctypes.c_ulong] * 4
        libc.prctl.restype = ctypes.c_int
        if libc.prctl(PR_SET_PTRACER, ctypes.c_ulong(-1).value, 0, 0, 0) != 0:
            error = ctypes.get_errno()
            raise OSError(error, os.strerror(error))
        value = {
            'schema': 1, 'pid': os.getpid(),
            'starttime_ticks': int(Path('/proc/self/stat').read_text().rsplit(')', 1)[1].split()[19]),
            'utc': datetime.datetime.now(datetime.timezone.utc).isoformat(),
            'monotonic': time.monotonic(), 'binary': str(binary),
            'binary_sha256': digest(binary),
            'wrapper': str(Path(__file__).resolve()),
            'wrapper_sha256': digest(Path(__file__).resolve()),
            'ptracer_policy': 'PR_SET_PTRACER_ANY for this process only',
            'prctl_succeeded': True, 'exec_requested': True,
            'argv': [str(binary), *sys.argv[1:]],
        }
        json.dump(value, output, indent=2)
        output.write('\n')
    os.execv(str(binary), [str(binary), *sys.argv[1:]])


if __name__ == '__main__':
    try:
        main()
    except (OSError, ValueError) as error:
        print(f'frame wrapper: {error}', file=sys.stderr)
        sys.exit(127)
