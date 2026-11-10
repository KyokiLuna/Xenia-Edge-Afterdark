# Afterdark HDD profile checkpoint V59

This checkpoint repairs local profile creation and the shared storage paths it exercises. It does not close the combined HDD gate. The accepted Guide and previous V53 checkpoint remain rollback artifacts with their original data.

Profile creation writes genuine Account, dashboard GPD, stock gamer pictures and stock avatar appearance/preview data from the installed 17559 resources. The native Create/GetStatus and Get/SetManifest paths share those services. Full native profile-creation task ownership, Avatar Editor geometry/animation and regenerated avatar tiles remain unfinished.

Two shared ownership defects were corrected: profile storage admission now happens before manager/GCRC publication locks, and cross-root immutable GPD journals carry their actual retained directory coordinates while Account protection remains active. Persistent snapshot ownership and native space-admission fixture mistakes were also corrected. No unknown-origin denial or project disk floor was weakened.

## Actual direct results

The Release build and all 15 direct runs passed. Base, network, kernel, VFS and UI total **1,545 cases / 81,284 assertions**. Focused runs cover canonical GPD data, genuine installed audio/fonts, native child/bind aliases, Account CAS, installed defaults and full initial-profile persistence.

The complete-profile case passed **6,516 assertions**: three distinct profiles; genuine Account/GPD/picture/appearance data; opposite stock body types; native creation/GetStatus and manifest calls; preserved opaque INFO2; separate saves; logout; actual Emulator reconstruction; and matching data after restart. Appearance updates do not claim newly rendered avatar tiles: the existing stock preview remains until the renderer work is complete.

Exact build manifest: `/home/kyoki/.local/state/xenia-edge-research/evidence/native-runtime/build/hdd-foundation-v59-profile-create-j1/compiled-manifest.json`, SHA256 `c608722068f2db9fc0e76aefeef9bed862d84c991f7743d65bcbfcba629d9255`. Independently checked native ELF: `2a4d3ba35173cc5c5edf327152019e2def3b8e5f8f9d1c985a392073df73dfd4`. Source/configuration counts: 1608/785; compiler database: 2085 rows; original index and accepted Guide preserved.

## Runtime and remaining gate

Finished-AppImage desktop creation/restart, automatic selected-metadata backups and current Guide/OSK regressions are **pending at this source snapshot**. Their actual later evidence belongs in the delivered checkpoint's acceptance report; direct tests are not a substitute. QA uses fresh/disposable research data, with MangoHud disabled and the 20 GiB project-filesystem floor intact. Populated user roots are never QA seeds.

The larger gate still requires complete device/content mutations and recovery, native selection/Home scenarios, identity-preserving profile repair, full avatar editing/rendering and Doritos consumption, media playback/import, both libraries and complete runtime acceptance. Native identity/listener work remains private and unapplied. Live/P2P, XNA and unrelated apps remain later work. macOS execution and other Linux distributions remain unverified.

## Upstream snapshot

The final read-only refresh observed `edge` at [30d2dc99](https://github.com/has207/xenia-edge/commit/30d2dc99df0c7a805be88dca53825e5698300d8e), unchanged from the preceding refresh. Its package-volume HDD correction is useful and **not equivalent** to this checkout's deployment-based Package_ branch; it is deferred from this finite creator checkpoint to the common registry/device-query cohort. It is not excluded as inferior. The preceding nine deferred changes through ae424604 remain recorded in `upstream-ae424604-independent-source-review-v1` and must be resolved before HDD gate closure, including namespace, missing-file, memory and XMA changes. The broader midgate ledger remains authoritative for earlier included/equivalent/deferred decisions. No PR, publishing or upstream merge was performed.
