# Community 580 keyboard matrix

These are the keyboard-bearing references in the cloned
`tools/rac-compiler/rsc_ropchain` checkout.

| Reference | Keyboard primitive | Counterpart status |
|---|---|---|
| `arrow_keys.rsc` | `getscancode`, arrow/AC table | `community/arrow_keys.cwiz`, exact RAC output |
| `basic_draw.rsc` | `getscancode`, arrows/1/0 | `community/basic_draw.cwiz`, exact RAC output |
| `key_switch.rsc` | `getscancode`, 1/2/3/AC | `community/key_switch.cwiz`, exact RAC output |
| `ea_switchcase.rsc` | `getscancode`, 1/2 | `community/ea_switchcase.cwiz`, exact RAC output |
| `2048_lite_991cnx.rsc` | `getscancode` | `community/2048_lite_991cnx.cwiz`, exact RAC output |
| `minesweeper.rsc` | `getscancode`, arrows/SHIFT | `community/minesweeper.cwiz`, exact RAC output |
| `neural_network.rsc` | `getscancode`, arrows/1/0/SHIFT | `community/neural_network.cwiz`, exact RAC output |
| `nn_991cnx_vf.rsc` | `getscancode`, arrows/1/0/SHIFT | `community/nn_991cnx_vf.cwiz`, exact RAC output |
| `gol.rsc` | `getkey` | `community/gol.cwiz`, exact RAC output |
| `rpg.rsc` | `getkeycode` | `community/rpg.cwiz`, exact RAC output |
| `hexd.rsc` | `getkey` | `community/hexd.cwiz`, exact RAC output |
| `text_editor_font_custom.rsc` | `getkeycode` | `community/text_editor_font_custom.cwiz`, exact RAC output |

`rpg_v1.rsc`, `ram_editor.rsc`, and `2048.rsc` are intentionally not in this
launchable counterpart set: their upstream files do not provide the required
`@section.launcher at 0xd180` section (or use a separate injection layout).
They need a launcher design before they can be exact CWiz build inputs.
