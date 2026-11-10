# Material icon derivatives

These derivatives have been generated, visually reviewed by Root and copied to
`generated/`. Linux host embedding has been compiled. Finished AppImage
packaging and runtime acceptance remain pending.
The canonical source is `afterdark-material-v2.png` (1254 × 1254, transparent
sRGBA), SHA-256
`960b1e7c4e71c5b1b1af3bc06d25ce9b41c594d64bb07069302588d02020ee52`.

The exact generation prompt and original reference/license hashes are retained
in `PROMPT.md` and `PROVENANCE.json`. This is raster artwork; the PNG plus prompt
and references are the editable design source. There is no equivalent layered
or vector master. The rejected flat SVG is not the final icon. Prompt reruns
are not deterministic reproduction of the accepted master.

## Actual derivation and copies

Root generated the nine PNG sizes and seven-frame ICO with ImageMagick
7.1.2-34 Q16-HDRI, executable SHA-256
`6c1f52f647ba7e0eb259081faa866c128450a3288abbdbfd6d4075ce92e2cde8`.
PNG commands used Lanczos resize, `-strip`, PNG32, one thread, 128 MiB memory
and 128 MiB map limits; all recorded commands returned 0. ICO frames are
16, 24, 32, 48, 64, 128 and 256 pixels, 32 bits each.

Exact existing output hashes (1,799,572 bytes total):

| File | Bytes | SHA-256 |
| --- | ---: | --- |
| `1024.png` | 1025728 | `739cdbe8dec726af6492c6e13ab1f093404f9875cd263c47fb971393a541ac7a` |
| `128.png` | 26395 | `406fb56ff7253ee3d4d8d19784356c48dd6c277b370b23ac525ad159acc6f3bd` |
| `16.png` | 1031 | `c1d75a6531d9bde0aab86fc4564ec612cc6528753e9f390b4cb1788fb62efd57` |
| `24.png` | 1936 | `95cb5c9f9e258654cb798c4b28201889ba3bc902c5c53501a6f65422be470ae6` |
| `256.png` | 81080 | `303bec2442064d90eb8e07c71a36a2d2a6283a358e86060c2e770914ae6f8efa` |
| `32.png` | 2978 | `93068de341301f54de2ae1e6274b479d0c3ed1f1086e936737669e6666b8026d` |
| `48.png` | 5599 | `ba9eb06c34506f836b47d45e1ea10fe4e88ec1d7ba40f578651e22e296cc2865` |
| `512.png` | 273555 | `d19274a2786304406b0e0c546b3d430edd043d190f60f039e6264c971fbe9eb8` |
| `64.png` | 8744 | `aa6e0b158d853978781aa2674553b5d793cc9edbc18e9fa9bcea47909a84d3f1` |
| `icon.ico` | 372526 | `e8abb43636d1670b4e24e6dd5b84a8cf45b2c515ccf778d7af48809f65c6cde5` |

The private derivation receipt is
`evidence/hdd-gate/afterdark-material-v2-derivatives-v1/result.json`, SHA-256
`ed6dc10e74ca350f58d89ea906dbc9f0125b1ac6982f9bc37f4aad08428abb3c`. Its original status predates visual review. The separate
`root-visual-review.json`, SHA-256
`013ac1fd0d4adcf20e11275f9b3e7f95ff9a16e48bd0550878f84412edc398c4`, records Root’s actual
light/dark 16/32/64-pixel inspection: recognizable luminous X/chip at 32/64,
fine material detail naturally collapsing at 16, transparent edges intact
and no observed clipping. `root-v19-pass-and-icon-assets-checkpoint-v1/`
`icon-assets.json`, SHA-256
`b7ad25a272f42394c8e1a6a34b924b479519d708161eb7589f13425c1d863513`, records the later project
copies. All project derivatives match both records. These private evidence
paths document authoring provenance; distributing the project does not require
running those historical scripts or retaining a developer-specific absolute path.

## Regeneration record

To reproduce the recorded deterministic scaling from the exact master, use
a fresh destination and the pinned ImageMagick version. This is the already
executed operation’s shape, not an outstanding generation prerequisite:

```sh
for size in 16 24 32 48 64 128 256 512 1024; do
  magick -limit thread 1 -limit memory 128MiB -limit map 128MiB \
    afterdark-material-v2.png -filter Lanczos -resize "${size}x${size}" \
    -strip "PNG32:FRESH_OUTPUT/$size.png"
done
magick FRESH_OUTPUT/16.png FRESH_OUTPUT/24.png FRESH_OUTPUT/32.png \
  FRESH_OUTPUT/48.png FRESH_OUTPUT/64.png FRESH_OUTPUT/128.png \
  FRESH_OUTPUT/256.png FRESH_OUTPUT/icon.ico
```

Do not overwrite the accepted master or existing derivatives while reproducing.
Keep `generated/` limited to these ten icon files: the existing CMake helper
embeds every regular file in that directory. Notes and provenance belong here,
outside it. The original top-level `assets/icon/` artwork is preserved.

## Consumers and license

The host window and About dialog use `generated/256.png` through the unchanged
embedded `_256_png_data` symbol. Windows MAINICON uses `generated/icon.ico`.
The existing macOS iconutil build stages the 16–1024 PNGs into AppIcon.icns.
The Linux AppImage workflow stages the PNGs under the desktop icon name
`xenia-edge-afterdark` and includes the icon notices. Windows and macOS resource
execution and finished AppImage acceptance remain unverified.

Distribute [the icon CC BY-SA 4.0 license](../LICENSE), the attribution in
`README.md`, prompt, provenance and this derivation record with adapted icons.
The main project’s separate BSD license and existing credits remain intact.
Host branding is not guest Xbox UI, notification pixel/cue or HDD acceptance.
