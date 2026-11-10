# Isolated baseline captures

`baseline.py` uses the Python standard library on native Linux and macOS. It
copies a prepared data seed for every capture, forces absolute storage,
content, cache and log paths, and disables guest writes beside source titles
and plugin loading. It does not change emulator behavior or implement stubs.

For optional Linux FPS diagnostics without an injected overlay, see
[guest-present sampling](FRAME_SAMPLING.md). It needs an unstripped binary and
measures interval averages, not CPU/GPU execution times or frame pacing.

Prepare an empty seed, or copy a deliberately staged data directory:

```sh
python3 tools/research/baseline.py prepare-seed \
  --destination /absolute/research/seeds/empty
python3 tools/research/baseline.py prepare-seed \
  --source /absolute/research/staged/dashboard-profile-data \
  --destination /absolute/research/seeds/dashboard-profile
```

The source is read only. Destination must be new; symlinks and non-regular
entries are rejected. Each seed contains hashes in `.baseline-seed.json`.
Changes after preparation invalidate it: prepare a new seed rather than edit
an existing one. To install system files or make a profile interactively,
launch a run with a longer timeout, finish setup in that disposable run, close
Xenia, then prepare a new seed from its `data` directory.

```sh
python3 tools/research/baseline.py run \
  --binary /absolute/research/binaries/xenia_edge.AppImage \
  --seed /absolute/research/seeds/dashboard-profile \
  --output-root /absolute/research/captures \
  --scenario dashboard-17559 --timeout 90 \
  --emulator-arg=--boot_animation=true \
  --target /absolute/research/title/default.xex
```

Omit `--target` to start the library. A macOS `.app` bundle is resolved to its
executable. Extra arguments must use `--name=value`; target, config, portable,
flagfile, isolation-root, write-permission and plugin overrides are rejected.
For seed configuration changes, edit a separate staging copy and prepare it
as a new seed. Do not pass a user's live profile directory as run storage.

Seed preparation and capture require **more than 20 GiB free**, plus 1 GiB
of fixed headroom and the estimated seed-copy allocation. This floor cannot
be disabled by a command-line flag or environment variable. The guard checks
the destination filesystem before creating a seed/run, then checks again
immediately before starting the emulator; the launch check reserves headroom
without charging for the already completed copy again. A refusal exits with
status 2. A late refusal preserves the disposable copy and records
`disk_space_error`, `completion_reason: low_disk_space`, and no process PID
in its metadata. The runner never deletes research data automatically.

The estimate rounds each file's **logical size** to destination allocation
blocks and includes directories. Sparse files can become dense during a
normal seed copy, so their small allocated size does not lower the budget.
During capture, a watchdog checks about once per second. At or below 20 GiB
plus a 512 MiB buffer (or if the capacity probe fails), it records
`completion_reason: low_disk_space` and stops the owned process group through
the normal cleanup path, returning status 2. Metadata retains the latest
watchdog check. These checks are not a disk quota: concurrent writers or a
rapid burst of writes can still consume the reserve between checks and
cleanup. Check storage before extending capture duration.

If cleanup removes a disposable `data/` tree, preserve the immutable seed,
run metadata, logs, and action records needed to repeat it. Reconstructing
from that seed restores the initial state, not profile/save changes made
during the removed run. Sparse placeholders are not a recovered run tree.

The command prints its unique run directory immediately. It contains:

- `data/`: disposable seed copy, including the run's generated TOML config.
- `xenia.log` and `stdout.log`: full emulator and combined stdout/stderr logs.
- `metadata.json`: UTC timestamps, duration, exact argv, executable SHA-256,
  seed SHA-256, target path/size, isolated XDG paths, process exit status and
  timeout flag, seed integrity check, and bounded evidence samples with line
  numbers. Evidence counts include both log files and can count duplicate
  stdout/file messages. Disk checks include available bytes, the floor,
  headroom and copy reservation. Pattern matches are observations, not
  diagnoses.

Metadata also records inherited `SDL_*`, `VK_*`, `LIBGL_*`, `MANGOHUD_*`,
`MANGOHUD`, `DISABLE_MANGOHUD`, `DRI_PRIME`,
`APPIMAGELAUNCHER_DISABLE`, and `APPIMAGE_EXTRACT_AND_RUN` settings when set.
It does not dump unrelated environment variables. Set a hint for one research
process using a shell prefix, for example
`SDL_GAMECONTROLLER_IGNORE_DEVICES=0x10f5/0x7126 python3 ...`; this does not
change the controller or global SDL configuration. Device-ignore filters apply
to every controller with the matching vendor/product, so verify the research
process's SDL device-add logs before interpreting input results.

The runner starts a new process session and follows its live process group to
the overall capture deadline. It promptly reaps the initial leader so a
same-group replacement waiting for that PID to disappear can continue; a
normal launcher exit does not stop the replacement. At timeout, SIGINT, or
SIGTERM, cleanup sends SIGINT, SIGTERM, then SIGKILL if needed and records any
remaining live group members. Zombie descendants are excluded from liveness
checks. Processes that detach into another session cannot be controlled by a
portable process-group runner. Original signal handlers are restored after
metadata finalization. XDG paths are isolated; `HOME` is not changed.

Exit 124 means the overall deadline was reached; this is normal for an
interactive emulator and does not prove a stall. Exit 130 means SIGINT and
143 means SIGTERM. After an observed launcher-to-child handoff, an orphaned
child's eventual status is not available to the runner: metadata retains
`leader_exit_code`, sets `exit_code` and `descendant_exit_code` to null, and
marks `exit_code_scope` as `unknown_after_handoff`. If such a group finishes
before its deadline, the CLI returns 3 to distinguish unknown descendant
status from launcher success. Otherwise a known leader status is returned
(signals use 128 + signal). Metadata includes the capture-tool SHA-256,
observed descendant PIDs, completion reason, and cleanup outcome. Captures
made by earlier harness versions did not follow normal leader handoffs and
must not use an initial zero exit to claim replacement success.

```sh
python3 tools/research/baseline.py summarize \
  --runs-root /absolute/research/captures \
  --output /absolute/research/capture-summary.json
python3 -m unittest discover -s tools/research -p 'test_*.py' -v
```

The safety tests use a fake executable and synthetic profile/config files;
they never launch Xenia or read personal emulator data. The capture runner
does not diagnose missing service backends, validate remote app APIs, or prove
title compatibility. Record the action taken and visible result alongside
each capture when producing the baseline report.

## Optional Linux virtual gamepad

`controller.py` needs optional `python-evdev` and access to `/dev/uinput`.
It creates one dedicated Xbox-style virtual pad through `/dev/uinput` and
never opens, grabs, or changes a physical controller. It emits gamepad
buttons/axes only, with no keyboard or pointer events. The virtual pad is
visible to other applications that consume gamepads; focus/capture behavior
still belongs to those applications.

```sh
python3 tools/research/controller.py \
  --record /absolute/research/actions-new.jsonl
```

Wait for the `ready` JSON line, which gives the device name and event node.
Send commands on stdin, one per line:

```text
tap a
tap start 0.2
press right
release right
axis lx 0.5
release_all
quit
```

Buttons are `a`, `b`, `x`, `y`, `lb`, `rb`, `back`, `start`, `guide`, `l3`,
`r3`, `up`, `down`, `left`, and `right`. Axes `lx`, `ly`, `rx`, and `ry` use
-1 through 1; positive Y points down. Triggers `lt` and `rt` use 0 through 1.
JSON lines can instead use `{"tap":"a","duration":0.15}`,
`{"button":"a","value":1}`, `{"axis":"lx","value":0.5}`,
`{"wait":1}`, `{"release_all":true}`, or `{"quit":true}`.

The virtual pad advertises Microsoft vendor `045e` and product `028e`. Its
Xbox layout uses Linux `BTN_X=307` and `BTN_Y=308`, matching SDL's non-Sony
mapping (`x:b2,y:b3` for the advertised sorted button codes). Linux's positional
aliases `BTN_NORTH=307` and `BTN_WEST=308` are misleading for this Xbox mapping.
Ready records include the button-code table and helper SHA-256.
Captures made with the initial helper mapping had X/Y reversed; running helper
processes retain the code loaded at startup, so restart one explicitly before
relying on corrected X/Y. A/B and Guide were unchanged.

For playback, supply `--script /absolute/actions.json`, a JSON array such as:

```json
[
  {"at": 10, "tap": "a", "duration": 0.2},
  {"at": 12, "button": "right", "value": 1},
  {"at": 12.3, "button": "right", "value": 0}
]
```

`at` is seconds from device readiness and must be nondecreasing. Actions run
sequentially; a tap/wait can delay a later action, and actual timestamps are
recorded. Use separate press/release actions for overlapping holds. Without
`--stay-open`, playback releases everything and removes the virtual device
after the final action; with it, stdin remains available. EOF, quit, SIGINT,
and SIGTERM release inputs and close the device. SIGKILL cannot run cleanup,
although the kernel removes the device when its owning process exits.
The explicit action-record path must not already exist.

### Assign the virtual pad to player 0

Xenia identifies SDL devices by their 32-hex GUIDs. In one isolated discovery
run, find the GUID next to `Xenia Baseline Virtual Xbox Pad` in the SDL
device-add log. Device names/event-node numbers are not slot binding IDs;
do not guess the GUID or reuse a physical controller's GUID.

The transient `slot_bindings_passthrough` argument contains four semicolon
separated guest slots, each with a comma-separated GUID list. With the virtual
GUID first and the real controller's GUID second:

```sh
--emulator-arg='--slot_bindings_passthrough=VIRTUAL_GUID;REAL_GUID;;'
```

Replace both placeholders with observed GUIDs. This assigns the virtual pad
to guest slot 0 and the real pad to slot 1 inside that Xenia process; it does
not change the physical controller or desktop input configuration. Existing
controllers can otherwise auto-bind before the virtual one. The Controllers
menu offers manual routing if SDL does not recognize the virtual device or
the observed GUIDs collide. Recheck logs/capabilities after routing and use
one virtual pad at a time. `guide` opens the existing Xenia context menu in
this upstream baseline; it does not establish installed guest-guide support.

Routing behavior comes from `src/xenia/hid/input_system.cc` (GUID reattachment
before auto-binding and the four-slot passthrough parser) and
`src/xenia/hid/sdl/sdl_input_driver.cc` (SDL GUID enumeration). The helper's
fake-sink tests run alongside the baseline tests without opening `/dev/uinput`.

## Optional X11 window captures

`window_capture.py` requires optional `python-xlib` and Pillow on Linux, plus
an X11/XWayland window manager exposing `_NET_CLIENT_LIST` and `_NET_WM_PID`.
It selects an emulator window by OS process identity rather than the currently
focused application. Read the emulator PID from the capture's `metadata.json`;
the baseline runner starts that PID as a new process-group leader.

```sh
python3 tools/research/window_capture.py --list --pgid EMULATOR_PID
python3 tools/research/window_capture.py \
  --pgid EMULATOR_PID --output /absolute/capture/window-new.png
```

Capture requires either `--pgid` or exact `--pid`. No match or multiple matches
fail rather than selecting another application's window. If the emulator
has several main/dialog windows, inspect `--list` and add an observed
`--window-id 0xHEX`. Unmapped/minimized/hidden windows are rejected. The new PNG
and its `.png.json` sidecar record selected window ID/title, PID, process group,
geometry, pixel format, UTC timestamp, selector and image SHA-256. Neither
output may already exist. Current image decoding supports common 24/32-bit
TrueColor X11 formats; unsupported formats fail explicitly.

The helper does not change focus unless `--activate` is explicitly supplied.
That option requests `_NET_ACTIVE_WINDOW`, verifies activation, then captures;
it sends no keyboard or pointer events. `XGetImage` cannot guarantee valid
contents for obscured portions of a window. Keep the selected window visible
and unobscured, or explicitly activate it, and inspect the resulting image
before citing it. Hardware overlays and some GPU/compositor paths can return
black or incomplete pixels. Native Wayland-only windows are unsupported;
XWayland visibility depends on the compositor. A captured image proves the
selected drawable's pixels, not title functionality or responsiveness. The
tests use synthetic window inventories and RGB data without connecting to X.
