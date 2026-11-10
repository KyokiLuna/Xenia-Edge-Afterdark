#!/usr/bin/env python3
"""Capture one process-selected Xenia X11 window, independently of focus."""

import argparse
import datetime
import hashlib
import json
import os
from pathlib import Path
import sys
import time


def select_window(windows, pid=None, pgid=None, window_id=None):
    candidates = [window for window in windows
                  if window['is_xenia']
                  and (pid is None or window['pid'] == pid)
                  and (pgid is None or window['pgid'] == pgid)
                  and (window_id is None or window['id'] == window_id)]
    if len(candidates) != 1:
        raise ValueError(f'Expected exactly one matching Xenia window, found '
                         f'{len(candidates)}; use --list and --window-id')
    selected = candidates[0]
    if not selected['viewable'] or selected['hidden']:
        raise ValueError('Selected window is unmapped, minimized, or hidden')
    return selected


def decode_image(data, width, height, bits_per_pixel, scanline_pad,
                 byte_order, masks):
    from PIL import Image
    modes = {
        (32, 0, (0xff0000, 0xff00, 0xff)): 'BGRX',
        (32, 1, (0xff0000, 0xff00, 0xff)): 'XRGB',
        (24, 0, (0xff0000, 0xff00, 0xff)): 'BGR',
        (24, 1, (0xff0000, 0xff00, 0xff)): 'RGB',
        (32, 0, (0xff, 0xff00, 0xff0000)): 'RGBX',
        (32, 1, (0xff, 0xff00, 0xff0000)): 'XBGR',
    }
    mode = modes.get((bits_per_pixel, byte_order, tuple(masks)))
    if mode is None:
        raise ValueError('Unsupported X11 pixel layout: '
                         f'{bits_per_pixel} bpp, order {byte_order}, {masks}')
    stride = ((width * bits_per_pixel + scanline_pad - 1) //
              scanline_pad) * (scanline_pad // 8)
    if len(data) < stride * height:
        raise ValueError('XGetImage returned a truncated image')
    return Image.frombytes('RGB', (width, height), data,
                           'raw', mode, stride, 1)


def property_value(window, display, name):
    from Xlib import X
    value = window.get_full_property(display.intern_atom(name),
                                     X.AnyPropertyType)
    return value.value if value is not None else None


def describe_window(display, window):
    from Xlib import X
    pid_value = property_value(window, display, '_NET_WM_PID')
    pid = int(pid_value[0]) if pid_value is not None and len(pid_value) else None
    pgid = None
    process_executable = None
    if pid is not None:
        try:
            pgid = os.getpgid(pid)
            process_executable = os.readlink(f'/proc/{pid}/exe')
        except (ProcessLookupError, PermissionError, FileNotFoundError):
            pass
    title_value = property_value(window, display, '_NET_WM_NAME')
    if title_value is not None:
        title = bytes(title_value).decode('utf-8', errors='replace')
    else:
        title = window.get_wm_name() or ''
    classes = list(window.get_wm_class() or ())
    state = property_value(window, display, '_NET_WM_STATE')
    hidden_atom = display.intern_atom('_NET_WM_STATE_HIDDEN')
    attributes = window.get_attributes()
    geometry = window.get_geometry()
    executable_name = Path(process_executable).name if process_executable else ''
    names = ' '.join([*classes, executable_name]).lower()
    is_xenia = ('xenia' in names or
                (not process_executable and title.lower().startswith('xenia')))
    return {
        'id': window.id, 'id_hex': hex(window.id), 'title': title,
        'classes': classes, 'pid': pid, 'pgid': pgid,
        'process_executable': process_executable, 'is_xenia': is_xenia,
        'viewable': attributes.map_state == X.IsViewable,
        'hidden': state is not None and hidden_atom in state,
        'width': geometry.width, 'height': geometry.height,
        'depth': geometry.depth, 'visual': attributes.visual,
    }


def enumerate_windows(display):
    from Xlib import error
    root = display.screen().root
    ids = property_value(root, display, '_NET_CLIENT_LIST')
    if ids is None:
        raise ValueError('X11 window manager does not expose _NET_CLIENT_LIST')
    windows = []
    for window_id in ids:
        window = display.create_resource_object('window', int(window_id))
        try:
            windows.append(describe_window(display, window))
        except error.XError:
            continue  # A client may close between enumeration and inspection.
    return windows


def activate_window(display, window):
    from Xlib import X, protocol
    event = protocol.event.ClientMessage(
        window=window.id,
        client_type=display.intern_atom('_NET_ACTIVE_WINDOW'),
        data=(32, [1, X.CurrentTime, 0, 0, 0]))
    display.screen().root.send_event(
        event, event_mask=X.SubstructureRedirectMask | X.SubstructureNotifyMask)
    display.flush()
    deadline = time.monotonic() + 2
    while time.monotonic() < deadline:
        active = property_value(display.screen().root, display,
                                '_NET_ACTIVE_WINDOW')
        if active is not None and len(active) and active[0] == window.id:
            time.sleep(0.15)
            return
        time.sleep(0.05)
    raise ValueError('Window manager did not activate the selected window')


def capture(display, selected, output, activate, selector=None):
    from Xlib import X
    window = display.create_resource_object('window', selected['id'])
    if activate:
        activate_window(display, window)
    current = describe_window(display, window)
    if (current['pid'], current['pgid']) != (selected['pid'], selected['pgid']):
        raise ValueError('Window process identity changed before capture')
    select_window([current], pid=selected['pid'], pgid=selected['pgid'])
    width, height = current['width'], current['height']
    if not width or not height:
        raise ValueError('Selected window has an empty drawable')
    reply = window.get_image(0, 0, width, height, X.ZPixmap, 0xffffffff)
    if reply is None:
        raise ValueError('XGetImage returned no data')
    formats = [item for item in display.display.info.pixmap_formats
               if item.depth == current['depth']]
    visuals = [visual for depth in display.screen().allowed_depths
               for visual in depth.visuals
               if visual.visual_id == current['visual']]
    if len(formats) != 1 or len(visuals) != 1:
        raise ValueError('Could not resolve the selected window pixel format')
    pixel_format, visual = formats[0], visuals[0]
    masks = [visual.red_mask, visual.green_mask, visual.blue_mask]
    image = decode_image(reply.data, width, height, pixel_format.bits_per_pixel,
                         pixel_format.scanline_pad,
                         display.display.info.image_byte_order, masks)
    output.parent.mkdir(parents=True, exist_ok=True)
    with output.open('xb') as destination:
        image.save(destination, format='PNG')
    info = {
        'schema': 1,
        'captured_utc': datetime.datetime.now(datetime.timezone.utc).isoformat(),
        'display': display.get_display_name(), 'selection': current,
        'selection_requested': selector,
        'activation_requested': activate, 'method': 'XGetImage window drawable',
        'pixel_format': {'bits_per_pixel': pixel_format.bits_per_pixel,
                         'scanline_pad': pixel_format.scanline_pad,
                         'byte_order': display.display.info.image_byte_order,
                         'masks': masks},
        'output': str(output),
        'sha256': hashlib.sha256(output.read_bytes()).hexdigest(),
        'limitation': 'Obscured drawable regions are not guaranteed valid by '
                      'XGetImage; inspect the image before using it as evidence.',
    }
    with Path(str(output) + '.json').open('x', encoding='utf-8') as destination:
        json.dump(info, destination, indent=2)
        destination.write('\n')
    return info


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--list', action='store_true')
    identity = parser.add_mutually_exclusive_group()
    identity.add_argument('--pid', type=int)
    identity.add_argument('--pgid', type=int)
    parser.add_argument('--window-id', type=lambda value: int(value, 0))
    parser.add_argument('--output', type=Path)
    parser.add_argument('--activate', action='store_true',
                        help='Explicitly request focus/raise for this window')
    args = parser.parse_args(argv)
    if not sys.platform.startswith('linux'):
        parser.error('Window capture currently supports Linux X11 only')
    if not args.list and (not args.output or
                          (args.pid is None and args.pgid is None)):
        parser.error('Capture needs --output and --pid or --pgid')
    if args.list and (args.activate or args.output):
        parser.error('--list does not activate or save images')
    output = args.output.expanduser().resolve() if args.output else None
    if output and (output.exists() or Path(str(output) + '.json').exists()):
        parser.error('Image and JSON output paths must both be new')
    display = None
    try:
        from Xlib import display as xdisplay
        display = xdisplay.Display()
        windows = enumerate_windows(display)
        if args.list:
            selected = [window for window in windows
                        if window['is_xenia']
                        and (args.pid is None or window['pid'] == args.pid)
                        and (args.pgid is None or window['pgid'] == args.pgid)]
            print(json.dumps(selected, indent=2))
            return 0
        selected = select_window(windows, args.pid, args.pgid, args.window_id)
        info = capture(display, selected, output, args.activate,
                       {'pid': args.pid, 'pgid': args.pgid,
                        'window_id': args.window_id})
        print(json.dumps(info, indent=2))
        return 0
    except ImportError as error:
        parser.error(f'Optional helper requires python-xlib and Pillow: {error}')
    except Exception as error:
        parser.error(str(error))
    finally:
        if display:
            display.close()


if __name__ == '__main__':
    sys.exit(main())
