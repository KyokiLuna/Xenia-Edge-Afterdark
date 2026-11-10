# Dashboard 17559 controller acceptance checklist

Baseline pin: `b65dd2514a5fdb92a2d4fd7ccfce5fdd4d4647d1`.
This is a UI-only procedure for the next tester. Recorded observations below
come from the baseline session; unchecked cases remain manual and untested.
Writing this checklist did not launch the emulator or perform these tests.

## Preparation and evidence

- [ ] Use an isolated copied seed for reproduction and an isolated fresh seed
  for new-profile cases. Never test changes against preserved original data.
- [ ] Record build, seed type, system-update version, renderer, resolution,
  controller arrangement, visible labels, and screenshots of each result.
- [ ] Start with one controller source only. Remove competing physical/virtual
  inputs; add another controller only for the multiple-user case.
- [ ] Use disposable profiles, saves, and content. Record symbolic names such
  as Profile A/B; omit real gamertags, account identifiers, and private messages.
- [ ] Record PASS, FAIL, or BLOCKED per case. A missing prerequisite is BLOCKED;
  a success dialog without the expected state change does not establish PASS.
  A means select, B means back, and RB means right bumper. Use visible labels
  for paths not recorded below; record missing actions rather than guessing.

## Recorded baseline observations: copied isolated seed

- Dashboard 17559 booted. RB traversed Home, Social, Games, Apps, and Settings.
- From Settings with System focused: A, Down twice, A opened Storage; select
  HDD with A, then Down once, A opened Profiles. The display showed
  **Corrupted Profile, 0 KB**. This is the observed dashboard listing, not proof
  that an original `Account` file or preserved profile was corrupted.
- From the Settings System tile: Right once, A selected Profile and opened
  Xenia's host gamercard editor. This was a host fallback, not a native editor.
- Guide opened the ImGui context menu. B closed it when a sole virtual
  controller supplied input. No native guide was loaded in this session.
- [ ] Reproduce these paths on the copied seed and record any difference before
  attributing it to a new change. Keep host fallback and native UI results separate.

## Guide focus, confirmation and return home

- [ ] With a title running, open Guide, close with Guide, reopen, then close
  with B. Press Start and verify the title receives fresh input afterward.
- [ ] Repeat with an idle controller in slot 0 and the active controller in
  slot 1. Record explicit bindings; a connected idle pad must not mask another.
- [ ] Park the mouse over the first menu item, then navigate with the controller
  to Return to Dashboard/Game List. Verify the selected label and actual action.
  Repeat using keyboard arrows and Enter; deliberate mouse movement must still
  change selection. Returning old focus must not activate an unrelated action.
- [ ] Select Return and cancel with B. The same title must continue and accept
  input. Reopen, select Return again and confirm. Capture the resulting home
  screen and log whether this was an in-process relaunch or a new process.
- [ ] Keep the native HUD lifecycle diagnostic separate: its initialization
  does not open a scene. Native acceptance requires visible HUD pixels over a
  running title, navigation, dismissal and fresh title input in that same run.

## Fresh profile: create, sign in/out, restart

- [ ] In the fresh seed, locate the visible profile/sign-in controls and create
  disposable Profile A. Record whether the UI is native or a host fallback.
- [ ] Sign in A; verify the displayed profile, user slot, and local/Fake Live
  indicator. A local Live marker does not establish network authentication.
- [ ] Change one visible profile field, save it, close the dialog, and reopen it.
- [ ] Sign out; verify the slot clears. Sign in again and verify the same profile.
- [ ] Exit normally through the UI, restart the same isolated seed, and repeat
  sign-in. PASS requires the saved field and profile identity to persist.
- [ ] Revisit Storage > HDD > Profiles using visible labels. Record listing,
  size, and accessibility; compare any corrupted/0 KB display with profile use.

## Multiple users and disposable save ownership

- [ ] Create Profile B; connect a second controller and assign A/B to distinct
  slots. Sign one out; the other must remain assigned correctly.
- [ ] In a title with a visible save/load workflow, create a new disposable save
  as A, change a recognizable value, and save through the title's normal UI.
- [ ] Restart, sign in A, and load it. PASS requires the saved value to survive.
- [ ] Switch to B and inspect the same title's save UI. Record its documented
  shared/per-user behavior; no save may silently change owner or overwrite A's.
- [ ] Restore the single-controller arrangement before the remaining cases.

## Content copy, move, and delete

- [ ] Prerequisite: two independently mounted storage devices and a disposable
  content item. With only HDD available, mark cross-device cases BLOCKED.
- [ ] In Storage, select that item and its visible Copy action; choose device 2.
  PASS: source remains, destination appears, and the copied item opens normally.
- [ ] Select the visible Move action for a disposable copy. PASS: destination
  opens correctly and the moved source disappears only after completion.
- [ ] Delete only a disposable copy through the visible Delete confirmation.
  Reopen both devices and restart; verify the remaining items and save ownership.

## Shared keyboard during the guide phase

These checks cover the named Xenia host fallback. The installed native `vk.xex`
scene requires native XUI/rendering and has separate acceptance.

- [ ] Open an ordinary single-line keyboard request from an isolated guest
  test or title. Record user, flags, default text and output capacity. A held
  opening A must not enter a character or close the keyboard.
- [ ] Use D-pad and A to enter letters/digits; exercise Shift, Symbols, Space
  and Delete, plus X/Y shortcuts. Repeat with slot 0 idle and another slot active.
  Test held-direction repeat and a held A; no unintended repeated character.
- [ ] Press Start to accept. Verify the caller receives big-endian UTF-16,
  a terminating NUL within its capacity, and unchanged surrounding canaries.
  Capacity includes NUL. A zero-capacity request must never copy output.
- [ ] Press B/Back to cancel after editing. Caller output must remain unchanged;
  the overlapped result/extended/length tuple is 1627/1223/0. Acceptance uses
  0/0/0. Forced teardown uses 1223/1223/0. Inspect actual completion fields.
- [ ] Verify asynchronous entry returns 997 before closure. The NULL-overlap
  variant waits for closure internally but its accepted outer return is 997.
- [ ] Enter/paste non-ASCII text with the physical keyboard, edit it, then
  continue with controller entry. Test a supplementary character at the UTF-16
  capacity boundary; no split surrogate or partial output write.
- [ ] Hold the closing button through return. Guest input stays consumed until
  release; a new game button works afterward. Move focus away and back, and
  exercise another modal owner; background input must not edit the keyboard.
- [ ] Close the title while entry is pending. Verify one completion, released
  input ownership, and no shutdown wait. Reopen a request after a normal cancel.

## Avatar editor, assets, and both CastleMiner titles

- [ ] Prerequisite: installed system/editor resources and avatar asset content,
  a disposable profile, and a visible avatar-edit action. Record missing assets,
  unavailable launch, host fallback, and guest editor failure separately.
- [ ] Open the avatar editor; change a visible body/clothing choice and save.
  Reopen the editor and restart. PASS requires the choice to persist and render;
  a save-success message or stored manifest alone is insufficient.
- [ ] Confirm an installed XNA launcher/runtime and the relevant update/content
  prerequisites before launching CastleMiner and CastleMiner Z from installed
  content. If a prerequisite is missing, record BLOCKED rather than gameplay FAIL.
  Install the unchanged type `00000002` containers under host `584E07D2` using
  the supported content layout, then exercise the installed launcher's chooser.
  Opening those containers as normal `default.xex` games is a separate route.
- [ ] For each title, record launch outcome, profile selection, avatar appearance,
  playable input, and a disposable save/reload. Use actual visible menu labels;
  do not substitute opening a raw managed executable for the launcher path.

## Social cases: gated on future implementation

- [ ] Prerequisite: implemented social APIs/UI and a consenting second tester
  on an independent connection. These cases have not passed in this baseline.
- [ ] Share a friend code outside the emulator; enter it in the visible pairing
  UI. Test accept and block separately. Verify both sides agree, blocked requests
  stay blocked, and accepted contacts persist after restarting both endpoints.
- [ ] Verify online/title presence, sign-out, and reconnect after an ordinary
  network interruption. Unreachable peers must become unavailable without losing
  saved contacts. No player account/service configuration or port forwarding.
- [ ] Send a disposable text message between accepted online friends; verify
  sender, recipient, ordering, and block behavior. Record offline behavior only
  if supported; do not assume an offline mailbox exists.
- [ ] Initiate a game invitation from the dashboard guide friends list once
  implemented, then test the named Xenia fallback separately. Accept on the peer;
  verify the intended installed title/session, cancellation, and expired invites.
- [ ] Create/join/leave a party, then test bidirectional voice with microphones,
  mute/unmute, block, and disconnect. Verify membership and mute state on both sides.
- [ ] Record direct-connection diagnostics if exposed. Helpers may carry setup
  metadata only; no relays or player payload through helpers. UI-only results
  cannot certify routing when diagnostics do not expose it; mark that unverified.

## Black Ops 1: Multiplayer and Zombies performance

- [ ] Use the same build, renderer, resolution, settings, and controller setup
  for both modes. Stop background builds and other competing workloads first.
- [ ] Enter Multiplayer through its visible menu, using an available local mode;
  repeat separately for Zombies. Record map/mode and whether a service blocks entry.
- [ ] Warm up each loaded map for two minutes, including shader compilation,
  then measure a reproducible 60-second active-play segment with a calibrated
  counter. Prefer the external guest-present reader for an unstripped Linux
  binary; the initial MangoHud runs have a reported severe slowdown and cannot
  establish the uninjected baseline. Record the method and complete-window
  evidence; monitor refresh rate is not emulation FPS.
- [ ] Target sustained 60 FPS in each measured segment. Record average/minimum
  and frame times where available, plus dips, stalls, and shader-compilation events.
  Do not hide dips in an average or mark 60 FPS PASS without a usable measurement.
  One-second counter intervals do not supply individual-frame percentiles or
  separate CPU/GPU execution times. Readback failures or other correctness
  errors gate healthy-performance acceptance even when frames continue.
- [ ] Save screenshots and observations for each mode. Repeat only after a change
  or unresolved measurement issue; these steps make no current performance claim.
