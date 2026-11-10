#!/usr/bin/env python3
"""Optional Linux virtual gamepad for explicit baseline action scripts."""

import argparse
import datetime
import hashlib
import json
import math
import os
from pathlib import Path
import shlex
import signal
import sys
import time


BUTTONS = {
    # Advertised Microsoft/Xbox device: SDL uses BTN_X/Y, not Sony positions.
    'a': 304, 'b': 305, 'x': 307, 'y': 308,
    'lb': 310, 'rb': 311, 'back': 314, 'start': 315,
    'guide': 316, 'l3': 317, 'r3': 318,
}
DPAD = {'up', 'down', 'left', 'right'}
AXES = {'lx': 0, 'ly': 1, 'lt': 2, 'rx': 3, 'ry': 4, 'rt': 5}
HAT_X, HAT_Y = 16, 17


def number(value, minimum, maximum, name):
    if isinstance(value, bool) or not isinstance(value, (int, float)):
        raise ValueError(f'{name} must be a number')
    if not math.isfinite(value) or not minimum <= value <= maximum:
        raise ValueError(f'{name} must be between {minimum} and {maximum}')
    return value


def validate_action(action):
    if not isinstance(action, dict):
        raise ValueError('An action must be a JSON object')
    keys = set(action)
    if keys == {'button', 'value'}:
        if (not isinstance(action['button'], str) or
                action['button'] not in BUTTONS.keys() | DPAD):
            raise ValueError('Unknown gamepad button')
        number(action['value'], 0, 1, 'button value')
        if action['value'] not in (0, 1):
            raise ValueError('Button value must be 0 or 1')
    elif keys in ({'tap'}, {'tap', 'duration'}):
        if (not isinstance(action['tap'], str) or
                action['tap'] not in BUTTONS.keys() | DPAD):
            raise ValueError('Unknown gamepad button')
        number(action.get('duration', 0.15), 0.01, 30, 'tap duration')
    elif keys == {'axis', 'value'}:
        if not isinstance(action['axis'], str) or action['axis'] not in AXES:
            raise ValueError('Unknown gamepad axis')
        number(action['value'], 0 if action['axis'] in ('lt', 'rt') else -1,
               1, 'axis value')
    elif keys == {'wait'}:
        number(action['wait'], 0, 3600, 'wait duration')
    elif keys in ({'release_all'}, {'quit'}):
        if next(iter(action.values())) is not True:
            raise ValueError('release_all/quit must be true')
    else:
        raise ValueError(f'Unknown action fields: {sorted(keys)}')
    return action


def parse_line(line):
    line = line.strip()
    if not line or line.startswith('#'):
        return None
    if line.startswith('{'):
        return validate_action(json.loads(line))
    words = shlex.split(line)
    if len(words) == 2 and words[0] in ('press', 'release'):
        action = {'button': words[1].lower(),
                  'value': int(words[0] == 'press')}
    elif len(words) in (2, 3) and words[0] == 'tap':
        action = {'tap': words[1].lower(),
                  'duration': float(words[2]) if len(words) == 3 else 0.15}
    elif len(words) == 3 and words[0] == 'axis':
        action = {'axis': words[1].lower(), 'value': float(words[2])}
    elif len(words) == 2 and words[0] == 'wait':
        action = {'wait': float(words[1])}
    elif words == ['release_all']:
        action = {'release_all': True}
    elif words == ['quit']:
        action = {'quit': True}
    else:
        raise ValueError('Use press/release BUTTON, tap BUTTON [SECONDS], '
                         'axis NAME VALUE, wait SECONDS, release_all, or quit')
    return validate_action(action)


def load_script(path):
    value = json.loads(path.read_text(encoding='utf-8'))
    if not isinstance(value, list):
        raise ValueError('Action script must be a JSON array')
    actions = []
    previous = 0
    for item in value:
        if not isinstance(item, dict) or 'at' not in item:
            raise ValueError('Each script action needs an at time in seconds')
        at = number(item['at'], previous, 86400, 'action time')
        action = validate_action({key: value for key, value in item.items()
                                  if key != 'at'})
        actions.append((at, action))
        previous = at
    return actions


class Recorder:
    def __init__(self, path):
        path.parent.mkdir(parents=True, exist_ok=True)
        self.file = path.open('x', encoding='utf-8')
        self.started = time.monotonic()

    def record(self, event, **data):
        item = {
            'event': event,
            'utc': datetime.datetime.now(datetime.timezone.utc).isoformat(),
            'elapsed_seconds': round(time.monotonic() - self.started, 6),
            **data,
        }
        self.file.write(json.dumps(item) + '\n')
        self.file.flush()
        return item

    def close(self):
        self.file.close()


class EvdevSink:
    """Creates only our device; never opens or grabs physical input devices."""
    def __init__(self, name):
        try:
            import evdev
        except ImportError as error:
            raise ValueError(
                'Optional controller helper needs python-evdev') from error
        absolute = []
        for name_axis, code in AXES.items():
            trigger = name_axis in ('lt', 'rt')
            absolute.append((code, evdev.AbsInfo(
                value=0, min=0 if trigger else -32768,
                max=255 if trigger else 32767, fuzz=0, flat=0, resolution=0)))
        for code in (HAT_X, HAT_Y):
            absolute.append((code, evdev.AbsInfo(0, -1, 1, 0, 0, 0)))

        class DedicatedUInput(evdev.UInput):
            def _find_device(self, fd):
                # evdev's old-kernel fallback scans physical event devices.
                # We need only the uinput writer, never an InputDevice reader.
                return None

        phys = f'xenia-baseline/virtual-gamepad/{os.getpid()}'
        self.device = DedicatedUInput(
            {evdev.ecodes.EV_KEY: list(BUTTONS.values()),
             evdev.ecodes.EV_ABS: absolute},
            name=name, vendor=0x045e, product=0x028e, version=0x0114,
            bustype=0x03, phys=phys, max_effects=0)
        self.path = None
        for entry in Path('/sys/devices/virtual/input').glob('input*'):
            try:
                if (entry / 'phys').read_text().strip() == phys:
                    event = next(entry.glob('event*'), None)
                    if event:
                        self.path = '/dev/input/' + event.name
                    break
            except OSError:
                continue

    def key(self, code, value):
        self.device.write(1, code, value)

    def axis(self, code, value):
        self.device.write(3, code, value)

    def sync(self):
        self.device.syn()

    def close(self):
        self.device.close()


class Controller:
    def __init__(self, sink, recorder, sleep=time.sleep):
        self.sink, self.recorder, self.sleep = sink, recorder, sleep
        self.buttons = set()
        self.dpad = set()
        self.closed = False

    def button(self, name, value):
        held = self.buttons if name in BUTTONS else self.dpad
        if value:
            held.add(name)
        else:
            held.discard(name)
        if name in BUTTONS:
            self.sink.key(BUTTONS[name], int(value))
        else:
            self.sink.axis(HAT_X,
                           int('right' in self.dpad) - int('left' in self.dpad))
            self.sink.axis(HAT_Y,
                           int('down' in self.dpad) - int('up' in self.dpad))
        self.sink.sync()
        self.recorder.record('button', button=name, value=int(value))

    def axis(self, name, value):
        maximum = (255 if name in ('lt', 'rt') else
                   (32768 if value < 0 else 32767))
        raw = round(value * maximum)
        self.sink.axis(AXES[name], raw)
        self.sink.sync()
        self.recorder.record('axis', axis=name, value=value, raw=raw)

    def release_all(self):
        for code in BUTTONS.values():
            self.sink.key(code, 0)
        for code in (*AXES.values(), HAT_X, HAT_Y):
            self.sink.axis(code, 0)
        self.sink.sync()
        self.buttons.clear()
        self.dpad.clear()
        self.recorder.record('release_all')

    def execute(self, action):
        validate_action(action)
        self.recorder.record('action', action=action)
        if 'button' in action:
            self.button(action['button'], action['value'])
        elif 'tap' in action:
            self.button(action['tap'], 1)
            try:
                self.sleep(action.get('duration', 0.15))
            finally:
                self.button(action['tap'], 0)
        elif 'axis' in action:
            self.axis(action['axis'], action['value'])
        elif 'wait' in action:
            self.sleep(action['wait'])
        elif 'release_all' in action:
            self.release_all()
        elif 'quit' in action:
            return False
        return True

    def close(self):
        if not self.closed:
            self.closed = True
            try:
                self.release_all()
            finally:
                self.sink.close()
                self.recorder.record('closed')


def interrupt(signum, frame):
    raise KeyboardInterrupt()


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--record', required=True, type=Path)
    parser.add_argument('--script', type=Path)
    parser.add_argument('--stay-open', action='store_true',
                        help='Read stdin after the timed script finishes')
    parser.add_argument('--name', default='Xenia Baseline Virtual Xbox Pad')
    args = parser.parse_args(argv)
    if not sys.platform.startswith('linux'):
        parser.error('Virtual controller requires Linux /dev/uinput')
    recorder = controller = None
    old_handlers = {}
    try:
        actions = load_script(args.script) if args.script else []
        recorder = Recorder(args.record.expanduser().resolve())
        sink = EvdevSink(args.name)
        controller = Controller(sink, recorder)
        for sig in (signal.SIGINT, signal.SIGTERM):
            old_handlers[sig] = signal.signal(sig, interrupt)
        ready = recorder.record('ready', name=args.name, event_node=sink.path,
                                vendor='045e', product='028e', version='0114',
                                button_codes=BUTTONS, axis_codes=AXES,
                                helper_sha256=hashlib.sha256(
                                    Path(__file__).read_bytes()).hexdigest())
        print(json.dumps(ready), flush=True)
        started = time.monotonic()
        keep_running = True
        for at, action in actions:
            time.sleep(max(0, started + at - time.monotonic()))
            keep_running = controller.execute(action)
            if not keep_running:
                break
        if keep_running and (not args.script or args.stay_open):
            for line in sys.stdin:
                action = parse_line(line)
                if action and not controller.execute(action):
                    break
        return 0
    except KeyboardInterrupt:
        return 130
    except (OSError, ValueError) as error:
        print(f'controller: {error}', file=sys.stderr)
        if recorder:
            recorder.record('error', message=str(error))
        return 2
    finally:
        try:
            if controller:
                controller.close()
        finally:
            if recorder:
                recorder.close()
            for sig, handler in old_handlers.items():
                signal.signal(sig, handler)


if __name__ == '__main__':
    sys.exit(main())
