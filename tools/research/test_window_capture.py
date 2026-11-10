"""Selection and image decoding tests; no X server is contacted."""

import importlib.util
from pathlib import Path
import unittest


SPEC = importlib.util.spec_from_file_location(
    'window_capture', Path(__file__).with_name('window_capture.py'))
capture = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(capture)


class WindowCaptureTest(unittest.TestCase):
    def test_selects_capture_group_instead_of_another_xenia(self):
        windows = [
            {'id': 1, 'pid': 100, 'pgid': 100, 'is_xenia': True,
             'viewable': True, 'hidden': False},
            {'id': 2, 'pid': 200, 'pgid': 200, 'is_xenia': True,
             'viewable': True, 'hidden': False},
            {'id': 3, 'pid': 201, 'pgid': 200, 'is_xenia': False,
             'viewable': True, 'hidden': False},
        ]
        self.assertEqual(capture.select_window(windows, pgid=200)['id'], 2)
        with self.assertRaises(ValueError):
            capture.select_window(windows, pgid=300)
        windows.append(dict(windows[1], id=4))
        with self.assertRaises(ValueError):
            capture.select_window(windows, pgid=200)
        self.assertEqual(capture.select_window(
            windows, pgid=200, window_id=4)['id'], 4)
        windows[-1]['hidden'] = True
        with self.assertRaises(ValueError):
            capture.select_window(windows, pgid=200, window_id=4)

    def test_decodes_padded_truecolor_rows_without_swapping_channels(self):
        try:
            import PIL
        except ImportError:
            self.skipTest('Optional Pillow is unavailable')
        image = capture.decode_image(
            bytes([0, 0, 255, 0, 255, 0, 0, 0]), 2, 1,
            32, 32, 0, (0xff0000, 0xff00, 0xff))
        self.assertEqual(image.getpixel((0, 0)), (255, 0, 0))
        self.assertEqual(image.getpixel((1, 0)), (0, 0, 255))
        image = capture.decode_image(bytes([0, 255, 0, 0]), 1, 1,
                                     24, 32, 0,
                                     (0xff0000, 0xff00, 0xff))
        self.assertEqual(image.getpixel((0, 0)), (0, 255, 0))
        with self.assertRaises(ValueError):
            capture.decode_image(b'\x00', 2, 1, 32, 32, 0,
                                 (0xff0000, 0xff00, 0xff))


if __name__ == '__main__':
    unittest.main()
