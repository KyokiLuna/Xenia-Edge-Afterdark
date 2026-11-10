"""Test action parsing and releases without creating a virtual device."""

import importlib.util
import json
from pathlib import Path
import tempfile
import unittest


SPEC = importlib.util.spec_from_file_location(
    'controller', Path(__file__).with_name('controller.py'))
controller = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(controller)


class FakeSink:
    def __init__(self):
        self.events = []
        self.closed = False

    def key(self, code, value):
        self.events.append(('key', code, value))

    def axis(self, code, value):
        self.events.append(('axis', code, value))

    def sync(self):
        self.events.append(('sync',))

    def close(self):
        self.closed = True


class FakeRecorder:
    def __init__(self):
        self.events = []

    def record(self, event, **data):
        self.events.append((event, data))


class ControllerTest(unittest.TestCase):
    def setUp(self):
        self.sink = FakeSink()
        self.recorder = FakeRecorder()
        self.pad = controller.Controller(self.sink, self.recorder,
                                          sleep=lambda duration: None)
        self.addCleanup(self.pad.close)

    def test_buttons_dpad_axes_and_cleanup(self):
        self.pad.execute(controller.parse_line('press a'))
        self.assertIn(('key', 304, 1), self.sink.events)
        self.pad.execute(controller.parse_line('press up'))
        self.assertIn(('axis', 17, -1), self.sink.events)
        self.pad.execute(controller.parse_line('press right'))
        self.assertEqual(self.sink.events[-3:],
                         [('axis', 16, 1), ('axis', 17, -1), ('sync',)])
        self.pad.execute({'axis': 'lx', 'value': -1})
        self.assertIn(('axis', 0, -32768), self.sink.events)
        self.pad.execute({'axis': 'rt', 'value': 1})
        self.assertIn(('axis', 5, 255), self.sink.events)
        self.pad.close()
        self.assertTrue(self.sink.closed)
        self.assertFalse(self.pad.buttons)
        self.assertFalse(self.pad.dpad)
        self.assertEqual(self.recorder.events[-1][0], 'closed')
        count = len(self.sink.events)
        self.pad.close()
        self.assertEqual(len(self.sink.events), count)

    def test_interrupted_tap_releases_button(self):
        def interrupted(duration):
            raise KeyboardInterrupt()
        self.pad.sleep = interrupted
        with self.assertRaises(KeyboardInterrupt):
            self.pad.execute({'tap': 'guide'})
        self.assertIn(('key', 316, 0), self.sink.events)
        self.assertNotIn('guide', self.pad.buttons)

    def test_advertised_xbox_xy_match_sdl_btn_x_y_mapping(self):
        self.pad.execute(controller.parse_line('tap x'))
        self.pad.execute(controller.parse_line('tap y'))
        self.assertEqual(
            [event for event in self.sink.events if event[0] == 'key'],
            [('key', 307, 1), ('key', 307, 0),
             ('key', 308, 1), ('key', 308, 0)])

    def test_reject_non_gamepad_and_out_of_range_input(self):
        for line in ('press ctrl', 'press mouse1', 'axis lx 1.1',
                     'axis lt -1', 'tap a -1', 'wait nan',
                     '{"key": 30, "value": 1}',
                     '{"button": [], "value": 1}'):
            with self.subTest(line=line), self.assertRaises(ValueError):
                self.pad.execute(controller.parse_line(line))
        self.assertEqual(self.sink.events, [])

    def test_script_validation_and_action_timestamps(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            script = root / 'script.json'
            script.write_text('[{"at":1,"tap":"a"},'
                              '{"at":2,"axis":"lx","value":0.5}]')
            self.assertEqual(len(controller.load_script(script)), 2)
            script.write_text('[{"at":2,"tap":"a"},'
                              '{"at":1,"tap":"b"}]')
            with self.assertRaises(ValueError):
                controller.load_script(script)
            record = root / 'actions.jsonl'
            recorder = controller.Recorder(record)
            recorder.record('button', button='a', value=1)
            recorder.close()
            value = json.loads(record.read_text())
            self.assertIn('utc', value)
            self.assertGreaterEqual(value['elapsed_seconds'], 0)
            with self.assertRaises(FileExistsError):
                controller.Recorder(record)


if __name__ == '__main__':
    unittest.main()
