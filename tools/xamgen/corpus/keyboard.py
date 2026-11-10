"""Interactive public-API keyboard corpus; no console golden results.

Build with tools/xamgen/xamgen.py build tools/xamgen/corpus/keyboard.py OUT
--only CASE (for example async-accept). A controller must
close the keyboard within xamgen's overlapped wait budget. The external runner
records its actions and checks observed output against the supplied-binary ABI
analysis and the chosen input, not against a claimed hardware capture.
"""

import struct


CASES = {
    'async-accept': dict(default=None, capacity=8, overlap=True, action='type'),
    'async-cancel': dict(default='keep', capacity=8, overlap=True, action='cancel'),
    'null-overlap': dict(default='\u65e5\U0001f642', capacity=8,
                         overlap=False, action='done'),
    'zero-capacity': dict(default='not copied', capacity=0,
                          overlap=True, action='type'),
    'zero-unmapped-output': dict(default='', capacity=0, overlap=True,
                                 action='type', unmapped=True),
    'held-opener': dict(default=None, capacity=8, overlap=True, action='type',
                        held=True),
}


def utf16(scenario, text, name):
    if text is None:
        return 0
    return scenario.string(text.encode('utf-16-be') + b'\0\0', name)


def build(corpus):
    for case, options in CASES.items():
        s = corpus.scenario(case)
        capacity = options['capacity']
        output = s.alloc(8 + max(2 * capacity, 16) + 8, 'guarded output')
        overlap = s.alloc(0x1c, 'overlapped', fill=0)
        default = utf16(s, options['default'], 'default')
        title = utf16(s, 'Keyboard probe: ' + case, 'title')
        description = utf16(s, 'Xenia host fallback export regression', 'description')
        out_pointer = 0xffffffff if options.get('unmapped') else output + 8
        s.call('XamShowKeyboardUI', 0, 0, default, title, description,
               out_pointer, capacity, overlap if options['overlap'] else 0,
               name='keyboard request',
               capture=[output, overlap])
        if options['overlap']:
            # A separate marker proves the export returned while the keyboard
            # was still open, before the external controller closes it.
            s.call('XamIsUIActive', name='async returned before user close',
                   wait=overlap, capture=[output, overlap])
        s.call('XamIsUIActive', name='UI released')
        state = s.alloc(16, 'held close state')
        s.call('XamInputGetState', 0, 1, state, name='held close input',
               capture=[state])
        delay = s.alloc(8, 'release interval', struct.pack('>q', -2000 * 10000))
        s.call('KeDelayExecutionThread', 1, 0, delay, name='release input wait')
        state = s.alloc(16, 'fresh game state')
        s.call('XamInputGetState', 0, 1, state, name='fresh game input',
               capture=[state])
