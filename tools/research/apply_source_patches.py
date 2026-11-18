#!/usr/bin/env python3
"""Apply the pinned Afterdark FFmpeg source correction before building.

The parent repository pins FFmpeg at c4f44071adecd5e568470e58ab4bdf483598dbc8.
This operation validates the complete before/after file and exact patch bytes.
It works in source archives without Git, and never invokes Git or the network.
"""

import hashlib
import json
import os
from pathlib import Path
import re
import stat
import sys
import tempfile

BASE_REVISION = 'c4f44071adecd5e568470e58ab4bdf483598dbc8'
BEFORE_SHA256 = 'dd6d34004bf820848c384c0e16efa93a56898c96b843141f35b96d5aa40fe6a6'
AFTER_SHA256 = '79f5bb89f74f17eee65117ea896df888f91d8f46ae2b006b810afaaaa2cb62b3'
PATCH_SHA256 = 'b95c67702987e8f15bee29f236a55380f1c2824dc02de2ddc67b1923ee955a77'
TARGET = Path('third_party/FFmpeg/libavcodec/wmaprodec.c')
PATCH = Path('third_party/patches/FFmpeg/xmaframes-overlap-tail.patch')


def digest(data):
    return hashlib.sha256(data).hexdigest()


def checked_native_path(root, relative):
    path = root
    for part in relative.parts:
        path /= part
        if path.is_symlink():
            raise RuntimeError('Refusing a symlink in source patch path: ' + str(relative))
    return path


def patched_bytes(original, patch):
    """Consume only the authenticated patch, validating every old hunk byte."""
    if digest(patch) != PATCH_SHA256:
        raise RuntimeError('Source patch bytes do not match the published patch')
    if digest(original) != BEFORE_SHA256:
        raise RuntimeError('FFmpeg source is neither the pinned original nor corrected version')
    lines = original.splitlines(keepends=True)
    hunks = patch.splitlines(keepends=True)
    cursor = 0
    output = []
    index = 0
    seen = 0
    while index < len(hunks):
        row = hunks[index]
        if not row.startswith(b'@@ '):
            if seen:
                raise RuntimeError('Unexpected patch data after a hunk')
            index += 1
            continue
        match = re.fullmatch(rb'@@ -(\d+),(\d+) \+(\d+),(\d+) @@[^\n]*\n', row)
        if not match:
            raise RuntimeError('Invalid authenticated patch hunk header')
        old_start, old_count, new_start, new_count = map(int, match.groups())
        start = old_start - 1
        if start < cursor or start > len(lines):
            raise RuntimeError('Patch hunk falls outside the pinned file')
        output.extend(lines[cursor:start])
        cursor = start
        if len(output) != new_start - 1:
            raise RuntimeError('Patch new-file offset does not match')
        removed = added = 0
        index += 1
        while index < len(hunks) and not hunks[index].startswith(b'@@ '):
            row = hunks[index]
            if row[:1] not in (b' ', b'-', b'+'):
                raise RuntimeError('Unsupported authenticated patch row')
            body = row[1:]
            if row[:1] in (b' ', b'-'):
                if cursor >= len(lines) or lines[cursor] != body:
                    raise RuntimeError('Patch context differs from the pinned file')
                cursor += 1
                removed += 1
            if row[:1] in (b' ', b'+'):
                output.append(body)
                added += 1
            index += 1
        if (removed, added) != (old_count, new_count):
            raise RuntimeError('Patch hunk counts do not match')
        seen += 1
    output.extend(lines[cursor:])
    result = b''.join(output)
    if not seen or digest(result) != AFTER_SHA256:
        raise RuntimeError('Patched source does not match the published corrected file')
    return result


def apply(root):
    root = Path(root).resolve(strict=True)
    target = checked_native_path(root, TARGET)
    patch_path = checked_native_path(root, PATCH)
    patch = patch_path.read_bytes()
    if digest(patch) != PATCH_SHA256:
        raise RuntimeError('Source patch bytes do not match the published patch')
    before_stat = target.stat()
    if not stat.S_ISREG(before_stat.st_mode):
        raise RuntimeError('FFmpeg patch target is not a regular file')
    original = target.read_bytes()
    current_hash = digest(original)
    if current_hash == AFTER_SHA256:
        return dict(status='already_applied', file=str(TARGET), sha256=current_hash,
                    ffmpeg_base_revision=BASE_REVISION)
    updated = patched_bytes(original, patch)
    descriptor, spelling = tempfile.mkstemp(prefix='.afterdark-wmaprodec-', dir=target.parent)
    temporary = Path(spelling)
    try:
        with os.fdopen(descriptor, 'wb') as stream:
            stream.write(updated)
            os.fchmod(stream.fileno(), stat.S_IMODE(before_stat.st_mode))
            stream.flush()
            os.fsync(stream.fileno())
        current_stat = target.stat()
        if target.is_symlink() or (current_stat.st_dev, current_stat.st_ino) != (before_stat.st_dev, before_stat.st_ino) or target.read_bytes() != original:
            raise RuntimeError('FFmpeg source changed while preparing the correction')
        os.replace(temporary, target)
        directory = os.open(target.parent, os.O_RDONLY | os.O_DIRECTORY)
        try:
            os.fsync(directory)
        finally:
            os.close(directory)
    finally:
        temporary.unlink(missing_ok=True)
    return dict(status='applied', file=str(TARGET), sha256=AFTER_SHA256,
                ffmpeg_base_revision=BASE_REVISION)


def main():
    if len(sys.argv) != 1:
        raise RuntimeError('Run without arguments from the Afterdark source checkout')
    root = Path(__file__).resolve().parents[2]
    print(json.dumps(apply(root)))


if __name__ == '__main__':
    try:
        main()
    except (OSError, RuntimeError) as error:
        print('Afterdark source patch failed: ' + str(error), file=sys.stderr)
        raise SystemExit(1)
