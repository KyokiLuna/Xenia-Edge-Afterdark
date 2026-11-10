#!/usr/bin/env python3
"""Sample only Xenia's four-byte guest-present counter on Linux, without a layer.

FPS values are interval/whole-sample guest-present averages, not CPU/GPU timing
or individual-frame durations/percentiles. Use frame_wrapper.py to opt the
research process into sibling process_vm_readv access under Yama.
"""
import argparse
import ctypes
import datetime
import hashlib
import json
import math
import os
from pathlib import Path
import re
import signal
import struct
import subprocess
import sys
import time

SYMBOL = '_ZN2xe20global_frame_number_E'


class IdentityError(RuntimeError):
    pass


class IOVec(ctypes.Structure):
    _fields_ = [('base', ctypes.c_void_p), ('length', ctypes.c_size_t)]


def digest(path):
    h = hashlib.sha256()
    with path.open('rb') as source:
        for chunk in iter(lambda: source.read(1024 * 1024), b''):
            h.update(chunk)
    return h.hexdigest()


def identity(pid, binary):
    actual = os.readlink(f'/proc/{pid}/exe')
    if actual != str(binary):
        raise IdentityError('Target executable changed or does not match expected binary')
    fields = Path(f'/proc/{pid}/stat').read_text().rsplit(')', 1)[1].split()
    if fields[0] in ('Z', 'X'):
        raise ProcessLookupError('Target is no longer running')
    return int(fields[19])  # Linux stat field22, starttime in clock ticks.


def verify_identity(pid, binary, starttime):
    if identity(pid, binary) != starttime:
        raise IdentityError('Target starttime changed; refusing PID reuse')


def parse_maps(text):
    result = []
    for line in text.splitlines():
        parts = line.split(None, 5)
        start, end = [int(x, 16) for x in parts[0].split('-')]
        name = parts[5] if len(parts) == 6 else ''
        name = re.sub(r'\\([0-7]{3})', lambda m: chr(int(m[1], 8)), name)
        result.append({'start': start, 'end': end, 'permissions': parts[1],
                       'offset': int(parts[2], 16), 'path': name})
    return result


def symbol_value(binary):
    value = subprocess.run(['nm', '-S', '--defined-only', '--format=posix', '--', str(binary)],
                           capture_output=True, text=True, check=True, timeout=20)
    matches = [s.split() for s in value.stdout.splitlines()
               if s.strip() and s.split(maxsplit=1)[0] == SYMBOL]
    if len(matches) != 1 or len(matches[0]) != 4:
        raise ValueError('Need the single unstripped guest-present counter symbol')
    _, kind, address, size = matches[0]
    if kind not in ('B', 'b', 'D', 'd') or int(size, 16) != 4:
        raise ValueError('Guest-present symbol must be one four-byte data object')
    return int(address, 16)


def elf_loads(binary):
    with binary.open('rb') as source:
        header = source.read(64)
        if header[:6] != b'\x7fELF\x02\x01':
            raise ValueError('Only little-endian ELF64 binaries are supported')
        kind = struct.unpack_from('<H', header, 16)[0]
        if kind not in (2, 3):
            raise ValueError('Need an executable or PIE ELF binary')
        position = struct.unpack_from('<Q', header, 32)[0]
        size, count = struct.unpack_from('<HH', header, 54)
        loads = []
        for index in range(count):
            source.seek(position + index * size)
            raw = source.read(size)
            if len(raw) < 56:
                raise ValueError('Truncated ELF program headers')
            entry = struct.unpack_from('<IIQQQQQQ', raw)
            if entry[0] == 1:
                loads.append({'flags': entry[1], 'offset': entry[2],
                              'vaddr': entry[3], 'filesz': entry[5], 'memsz': entry[6]})
    return loads


def counter_address(binary, maps, value):
    page = os.sysconf('SC_PAGE_SIZE')
    loads = elf_loads(binary)
    # The ELF header's PT_LOAD is a unique, unambiguous bias anchor.
    anchors = [s for s in loads if s['offset'] == 0 and s['filesz'] >= 64]
    file_maps = [m for m in maps if m['path'] == str(binary) and m['offset'] == 0]
    if len(anchors) != 1 or len(file_maps) != 1:
        raise ValueError('Cannot uniquely resolve the ELF header load bias')
    bias = file_maps[0]['start'] - (anchors[0]['vaddr'] // page) * page
    address = bias + value
    if address % 4:
        raise ValueError('Counter is not four-byte aligned')
    if not any(s['flags'] & 4 and s['vaddr'] <= value and value + 4 <= s['vaddr'] + s['memsz']
               for s in loads):
        raise ValueError('Counter is outside readable ELF load segments')
    if not any('r' in m['permissions'] and m['start'] <= address and address + 4 <= m['end']
               for m in maps):
        raise ValueError('Counter is outside readable process mappings')
    return address, bias


def read_counter(pid, address):
    value = ctypes.c_uint32()
    local = IOVec(ctypes.addressof(value), 4)
    remote = IOVec(address, 4)
    libc = ctypes.CDLL(None, use_errno=True)
    function = libc.process_vm_readv
    function.argtypes = [ctypes.c_int, ctypes.POINTER(IOVec), ctypes.c_ulong,
                         ctypes.POINTER(IOVec), ctypes.c_ulong, ctypes.c_ulong]
    function.restype = ctypes.c_ssize_t
    count = function(pid, ctypes.byref(local), 1, ctypes.byref(remote), 1, 0)
    if count != 4:
        error = ctypes.get_errno() if count < 0 else 5
        raise OSError(error, f'Four-byte process_vm_readv failed: {os.strerror(error)}')
    return value.value


def utc():
    return datetime.datetime.now(datetime.timezone.utc).isoformat()


def sample(args):
    if not sys.platform.startswith('linux'):
        raise ValueError('This sampler requires Linux')
    if not args.binary.is_absolute() or not args.output.is_absolute() or not args.wrapper_record.is_absolute():
        raise ValueError('Binary, wrapper record and new output paths must be absolute')
    if args.pid <= 0 or not math.isfinite(args.interval) or not math.isfinite(args.duration) or not 0.05 <= args.interval <= 60 or args.duration <= 0:
        raise ValueError('Need positive PID/duration and interval0.05..60 seconds')
    binary = args.binary.resolve(strict=True)
    starttime = identity(args.pid, binary)
    record = json.loads(args.wrapper_record.read_text())
    native_hash = digest(binary)
    if record.get('pid') != args.pid or record.get('starttime_ticks') != starttime or record.get('binary') != str(binary) or record.get('binary_sha256') != native_hash or record.get('prctl_succeeded') is not True:
        raise IdentityError('Wrapper provenance does not match this native process')
    wrapper = Path(record['wrapper'])
    if digest(wrapper) != record['wrapper_sha256']:
        raise IdentityError('Wrapper changed after launch')
    value = symbol_value(binary)
    maps = parse_maps(Path(f'/proc/{args.pid}/maps').read_text())
    address, bias = counter_address(binary, maps, value)
    verify_identity(args.pid, binary, starttime)
    initial = read_counter(args.pid, address)
    verify_identity(args.pid, binary, starttime)
    args.output.parent.mkdir(parents=True, exist_ok=True)
    stop = [None]
    old = {}
    for sig in (signal.SIGINT, signal.SIGTERM):
        old[sig] = signal.signal(sig, lambda number, frame: stop.__setitem__(0, number))
    reason, code = 'duration', 0
    count, total, valid_seconds = 0, 0, 0.0
    began = previous_time = time.monotonic()
    previous = initial
    try:
        with args.output.open('x', encoding='utf-8', buffering=1) as output:
            def emit(event):
                output.write(json.dumps({'utc': utc(), 'monotonic': time.monotonic(), **event}) + '\n')
            emit({'event': 'start', 'pid': args.pid, 'starttime_ticks': starttime,
                  'binary': str(binary), 'native_binary_sha256': native_hash,
                  'wrapper': str(wrapper), 'wrapper_sha256': record['wrapper_sha256'],
                  'sampler_sha256': digest(Path(__file__).resolve()),
                  'symbol': SYMBOL, 'symbol_value': hex(value), 'load_bias': hex(bias),
                  'counter_address': hex(address), 'read_size': 4, 'initial_counter': initial,
                  'interval_seconds': args.interval, 'duration_seconds': args.duration,
                  'scope': 'Guest-present averages only; no CPU/GPU times or per-frame percentiles.'})
            next_sample = began + args.interval
            while not stop[0] and next_sample <= began + args.duration:
                time.sleep(max(0, next_sample - time.monotonic()))
                if stop[0]:
                    break
                try:
                    verify_identity(args.pid, binary, starttime)
                    current = read_counter(args.pid, address)
                    verify_identity(args.pid, binary, starttime)
                except (FileNotFoundError, ProcessLookupError):
                    reason = 'process_ended'
                    break
                except (IdentityError, OSError) as error:
                    reason, code = str(error), 1
                    break
                now = time.monotonic()
                seconds = now - previous_time
                delta = (current - previous) & 0xFFFFFFFF
                anomalous = delta > max(10000, seconds * 10000)
                emit({'event': 'sample', 'counter': current, 'elapsed_seconds': seconds,
                      'present_delta': None if anomalous else delta,
                      'guest_present_fps': None if anomalous else delta / seconds,
                      'counter_discontinuity': anomalous})
                count += 1
                if not anomalous:
                    total += delta
                    valid_seconds += seconds
                previous, previous_time = current, now
                next_sample += args.interval
                if next_sample < now:
                    next_sample = now + args.interval
            if stop[0]:
                reason, code = f'signal{stop[0]}', 128 + stop[0]
            emit({'event': 'stop', 'reason': reason, 'samples': count,
                  'present_delta': total, 'valid_sample_seconds': valid_seconds,
                  'average_guest_present_fps': total / valid_seconds if valid_seconds else None})
    finally:
        for sig, handler in old.items():
            signal.signal(sig, handler)
    return code


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--pid', type=int, required=True)
    parser.add_argument('--binary', type=Path, required=True)
    parser.add_argument('--wrapper-record', type=Path, required=True)
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--interval', type=float, default=1.0)
    parser.add_argument('--duration', type=float, default=30.0)
    args = parser.parse_args()
    try:
        return sample(args)
    except (OSError, ValueError, IdentityError, subprocess.SubprocessError) as error:
        print(f'frame sampler: {error}', file=sys.stderr)
        return 1


if __name__ == '__main__':
    sys.exit(main())
