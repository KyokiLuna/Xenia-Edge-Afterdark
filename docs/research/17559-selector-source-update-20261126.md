# Shared storage-selector generation — 2026-11-26

A pending automatic or host-fallback storage selection now stays tied to the
exact registered folder generation originally selected. Removing and
reinserting a folder under the same public device ID cannot redirect that
pending result. Availability, write permission and capacity are checked again
before publishing the device ID; refusal preserves the guest output.

The retained selection contains registry metadata only. It does not keep a
backing file or a managed-storage admission open while the caller chooses.
The existing source-generation admission covers the actual guest store. This
is required host translation: a stable console device ID can outlive an
individual host folder registration.

Automatic selection and both existing host fallback completions use the same
publication rule. The native SYSTEM selector branch and its table/device-ID
translation are unchanged. Full native selection, remembered-device and
ambiguity policy still need their own acceptance.

## Validation

All five full direct suites and both focused selections pass with zero
failures and normal owned process cleanup. Focused selections overlap the
full suites; the CPU focus is not the full CPU corpus.

| Run | Cases | Assertions |
| --- | ---: | ---: |
| Base | 334 | 28711 |
| Network | 125 | 11212 |
| Kernel | 1492 | 101034 |
| VFS | 136 | 9736 |
| UI | 124 | 4863 |
| CPU focus | 5 | 130 |
| Installed-system kernel focus | 223 | 31200 |

Build receipt: `87702d26f0665a368c698079a3ac2a47119fae949823b9cd17fb46a2dd3f0400`.
Direct-suite receipt: `4dd23da6b729a047c591839d1fcca2914c080b8adb8a270d7fe17951b7f09e6e`.
Source/configuration, the actual products and the 174 allowlisted installed
system inputs remain unchanged during validation. Minimum observed available
project-filesystem space is 24.56 GiB; the 20 GiB floor is preserved.
MangoHud is disabled.

The two added real-export cases exercise removal/replacement under the same
ID, a fresh successful replacement selection, a read-only secondary device
beside a writable folder, an oversized 64-bit request, and withdrawal of the
only writable choice. Refusals preserve sentinel outputs. These are shared
selector component tests; they do not execute a dashboard selector.

The first V150 run retains a failed capacity fixture in both the full and
installed-system kernel runs. That fixture treated the filesystem-wide free
space reserve as a per-folder quota. The corrected V151 fixture keeps the
shared reserve intact and issues a genuinely oversized request for the
disk-full case. The production executable is unchanged by that test fix.

## Source identity and remaining work

The V151 source manifest is
`c8c15fa0b810057f2be4683ca74d58b52f28462e94a61d71840fd92a3763e5e2`
(2,430 source files). The actual Linux Release executable is
`3b9e2a137d61c81bc092aca1f264b54afef9155e13f0af7876c2a79107e54b5b`.
The existing generated version header remains preserved; its compiled base
is `2418ae0aa308cb718d80bc6713a6ae2b078424d4`. A later source-backup commit
does not change that compiled identity.

The latest actual native game test remains V149, described in the
[profile lifecycle comparison](17559-profile-lifecycle-comparison-20261125.md).
It renders the genuine sign-in error but refuses the subsequent native
20B/flags-0x63 request before entry. Its test supervisor then receives SIGTERM
from an unidentified sender. This selector change does not settle that
request, establish Guide/game continuation, or replace the published AppImage.

Queued request ownership, native serial selection, current-user metadata,
profile data handoff and retained UI/resource cleanup are being implemented
as one lifecycle change. Native Save/Flush durability and the complete
Header/map/model/cache successor remain separate unfinished work. Profile,
avatar, media and full combined-HDD acceptance remain open.

This is a tested source backup, with no new release, tag, upstream merge or PR.
