# Ghosts Raven freeze and the current-user read contract

Status: the shared correction is implemented and all seven required direct
runs pass. Packaged runtime validation remains pending; no corrected AppImage
or game acceptance is established by this report.

Two isolated runs of the actual V156 AppImage stop presenting during the
opening logos. The user identified the Raven logo. Both logs record a host
read fault at address `0x10` on guest thread 5 before presentation stops.
Afterdark halts that fiber while retaining the dispatcher and window. Normal
window closure and process exit zero therefore do not establish successful
game execution.

## Reproduction identity

- AppImage SHA256: `32ec2aaf31426106b94489a78fd63427d4478ac22427ff463c318cb8b023dcfe`.
- Packaged executable SHA256: `4571d382e5dcc6be5ca9da1fa62cea1a16bef8efdb30592ad20fb31ee9d4ad84`.
- Matching unstripped executable SHA256: `f71c18991cda7bfdddc071436dc0a51b6ffb5b37d1639d722e710cdb75824b8c`.
- ELF build ID: `1a816d6ef1f1c5180c366cdea862385f8ae350a5`.
- Ghosts Disc 1: title `415608FC`, media `3CC5A411`, raw version `5`.
- ISO SHA256: `edd69dc09862de9a373f3b79d6916aa21552b4301893da0479ebd9300af74d41`.

Launch Disc 1 with native UI, MangoHud disabled, public installed System files
and an isolated profile created through the ordinary UI. Observe the opening
logos without opening Guide. The second run reuses only that isolated state.
Neither run uses the user's populated profile or library data. Installed title
updates and the user's original failing milestone are separate identities.

## Exact failure boundary

The second run captures its owned executable mapping once. Its actual PIE
load bias, matching ELF symbol table and instruction disassembly resolve the
fault to `UserProfile::name() const`, ELF address `0x8c3841`, function offset
`0x21`. The faulting instruction reads the atomic Account pointer through a
null `UserProfile` receiver.

Ghosts' recorded guest LR `825B5AC8` follows the call at `825B5AC4` to import
thunk `8266B494`. The decoded original thunk and both runtime import tables
identify ordinal `0x227`, `XamUserGetSigninInfo`. The HLE implementation checks
`IsUserSignedIn`, then independently looks up the profile and dereferences it.
Each lookup can refuse transient metadata contention. The first check does
not own a successful second lookup or keep the selected identity stable.

The matching 17559 XAM scalar getters read the current per-slot cache under
its existing publication contract. This is distinct from acquiring Account,
GPD, storage or a fixed native data-read scope. The correction must preserve
that distinction across related scalar consumers, publication, cancellation,
rename and shutdown; a null check in this one export is insufficient.

The runtime arguments identifying the requested slot and flags were not
captured. Later scheduler/Bink waits follow the host fault. Earlier Vulkan
memexport warnings are retained as a separate finding; they are not evidence
of this null-receiver fault's cause. Neither run hit the RAM or disk guard.

Private local evidence records are `root-v156-ghosts-raven-symbolization-20261127-v1`,
`root-v156-ghosts-raven-fiber-fault-readonly-20261127-v1` and
`root-ghosts-raven-guest-callsite-readonly-v1` under the research HDD evidence
directory. Retail executables, raw diagnostics and profile payloads are not
included in this source report.

## Shared correction and engineering validation

Current scalar queries capture one owned identity from the selected roster and
one immutable Account projection. They do not acquire a storage/model read
lease or use a successful sign-in check as proof that a later profile lookup
will succeed. Related locale, child, content-XUID and gamercard consumers use
the same ownership distinction. Native cached flags whose producer is still
unresolved retain their previous policy.

Host sign-in uses the existing concurrent host backend for its entire
wait/run/post lifecycle. A failed selection remains visible and can be retried;
cancellation cannot overwrite a completed roster commit with fabricated
success. Queued overlapped completion retains the original event and APC
thread objects, so closing and reusing a numeric handle cannot redirect it.
An exited APC target refuses insertion. Teardown joins the queued work before
its captured state is destroyed.

The current Release build passes these executables directly with no failures:

| Run | Cases | Assertions |
| --- | ---: | ---: |
| Base | 334 | 28,711 |
| Network | 125 | 11,212 |
| Kernel | 1,526 | 114,713 |
| VFS | 136 | 9,736 |
| UI | 124 | 4,863 |
| Focused CPU selection | 5 | 130 |
| Kernel with installed System fixtures | 257 | 44,877 |

The installed-System selection overlaps the kernel run. The complete CPU
corpus was not run. Seventeen new cases cover scalar ABI/current-identity
semantics and host UI result, cancellation, lifetime and handle reuse. The
three-mode overlap test separately passes 90 assertions. Its initial test
setup errors and failed runs remain in local evidence; no production success
was inferred from them.

Engineering receipts are under
`root-shared-profile-build-direct-20261127-v157-j2-r5`. Their build-entry source
base is `7b6df7b2f3d65bb9245c7d66e071c2b4983ec6e1`; the cached generated
version header has an earlier origin, recorded separately in provenance. The
subsequent public source commit must match every tested source blob. Actual
AppImage playback past Raven and visible Guide close/reopen remain required.
The combined HDD gate remains incomplete.
