# Xenia Edge Afterdark artwork

`afterdark-material-v2.png` is the revised application icon for **Xenia Edge
Afterdark**: a machined dark processor and a sculpted, luminous green X inspired
by the original Xbox era. The transparent PNG is the unchanged generated master.

The fork's desktop and AppImage naming base is `xenia-edge-afterdark`. Existing
internal executable and storage paths are retained. The Linux host build embeds
this artwork, and the AppImage workflow selects its icons and desktop entry.
The original icons remain available.

This artwork adapts the chip/X icon distributed by the Xenia contributors and
[Xenia Edge](https://github.com/has207/xenia-edge/tree/edge/assets/icon).
The original icon assets and this adaptation use **CC BY-SA 4.0**; see
[the icon license](../LICENSE). The local upstream assets do not identify an
individual illustrator. Preserve this attribution and license when distributing
the revised artwork.

The icon was created with built-in image generation. `PROMPT.md` contains the
exact prompt; `PROVENANCE.json` records reference and output hashes.

## Editable source and distribution derivatives

The editable design source is this unchanged PNG master plus the exact prompt
and references in `PROMPT.md` and `PROVENANCE.json`. It is generated raster
artwork; the earlier flat SVG is superseded and is not an equivalent source for
this material icon. New design edits must retain their own prompt/reference and
output provenance.

See [DERIVATION.md](DERIVATION.md) for the actual ImageMagick derivation,
output hashes and Root’s small-size review. The nine PNGs and seven-frame ICO
already exist in `generated/`. Linux host embedding has been built; a finished
AppImage still requires the HDD gate's packaging and runtime acceptance.
Original `assets/icon/*.png` and `icon.ico` remain available. Preserve [the original CC BY-SA 4.0 license](../LICENSE), this
attribution, prompt and provenance in the source and release icon notices.
