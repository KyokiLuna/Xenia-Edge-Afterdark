# Linux guest-present sampling

These optional tools measure the existing `xe::global_frame_number_` counter.
The pinned source defines it as `std::atomic<uint32_t>` in `logging.cc`; the
counter advances once per guest present. Samples give average guest presents
per second over each interval and over the valid sampling window. They do not
measure CPU/GPU execution time, frame pacing or individual-frame percentiles.

Use an unstripped Linux ELF64 binary with the exact four-byte
`_ZN2xe20global_frame_number_E` symbol. No symbol address is hardcoded. The
sampler resolves it using `nm`, the ELF load segment and `/proc/PID/maps`.
Symbol/hash setup happens before the measured sampling window.

Launch a fresh baseline without MangoHud or another injected measurement layer:

```sh
XENIA_RESEARCH_BINARY=/absolute/pinned/native/xenia_edge \
  python3 /absolute/checkout/tools/research/baseline.py run \
  --binary /absolute/checkout/tools/research/frame_wrapper.py \
  --seed /absolute/prepared/seed --output-root /absolute/research/runs \
  --scenario native-guest-presents --target /absolute/owned/game.iso \
  --timeout 180 --emulator-arg=--boot_to_dashboard=false
```

The wrapper requires an absolute executable in `XENIA_RESEARCH_BINARY`; it does
not choose or build one. It sets `PR_SET_PTRACER_ANY` for its own process and
then `exec`s that executable, preserving the PID and forwarding every emulator
argument unchanged. It changes no global kernel/sysctl policy and injects no
library, layer, debugger or code into the emulator. The opt-in permits other
same-user processes to inspect this research process while it lives.

The wrapper writes a new `RUN/frame-wrapper.json` beside the baseline's explicit
log path, recording PID/starttime, native binary hash and wrapper hash. An
absolute new `XENIA_RESEARCH_WRAPPER_RECORD` path may override that sidecar
location. The baseline's own binary hash refers to the wrapper, so retain both
records. Do not replace/rebuild the native executable during a capture.

After the wrapper has executed the native binary, use the emulator PID from
`RUN/metadata.json` (the wrapper keeps the same PID):

```sh
python3 /absolute/checkout/tools/research/frame_sample.py \
  --pid EMULATOR_PID --binary /absolute/pinned/native/xenia_edge \
  --wrapper-record /absolute/RUN/frame-wrapper.json \
  --output /absolute/RUN/guest-presents.jsonl \
  --interval 1 --duration 60
```

An interval of `0.1` seconds is also supported. Each sample verifies the exact
executable and Linux process starttime before and after `process_vm_readv`,
which is configured to read only the four-byte counter. The wrapper's recorded
starttime rejects a stale record even if the PID is reused. No game memory dump
is produced. An executable/starttime mismatch stops sampling; an unavailable
symbol, permission failure or invalid mapping fails rather than guessing.
Output paths are exclusive and existing files are preserved.

The JSONL contains start provenance, interval deltas/FPS, and a final average.
Large counter discontinuities are recorded without an FPS value and excluded
from the average. Zero deltas mean no guest presents during that interval; they
do not identify which subsystem was waiting. A title exit/relaunch may end the
sample; do not silently switch to a new PID. The sampler can be stopped with
SIGINT/SIGTERM and does not terminate the emulator. A zero sampler exit code can
also mean the target ended early. For a requested complete window, inspect the
final `reason` (expected `duration`) and `valid_sample_seconds`; an early
`process_ended` result is not a complete 60-second performance capture.

Synthetic verification (no Xenia or input device involved):

```sh
python3 -m unittest discover -s tools/research -p test_frame_sampling.py -v
```

The integration test builds a small PIE counter and launches the sampler as
its sibling process. It exercises the real Linux ptracer/read path and checks
counter advancement, provenance, wrong-executable/stale-starttime rejection and
preservation of existing output files. A C compiler and `nm` are only required
for this synthetic test; Python, Linux libc and `nm` are needed by the tools.
