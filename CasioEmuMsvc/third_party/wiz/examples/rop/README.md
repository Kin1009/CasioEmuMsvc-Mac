# Casio fx-580VN X ROP counterparts

All `.cwiz` files here compile through RAC. There are two fidelity levels:

* **High-level**: `arrow_keys.cwiz`, `arrow_keys_loop.cwiz`, `keycode_loop.cwiz`
  and `key_switch.cwiz` use CWiz statements and the emitter's keyboard ABI.
* **Reference**: `basic_draw.cwiz` uses
  `#raw-rsc` because their exact payload depends on hand-written stack words,
  `strcpy`, `load_table`, or model-specific gadgets. RAC still compiles these
  files and verifies the original chain; they are not claimed to be
  high-level translations.

For a keyboard loop, put `setlr_pc`/`setsfr` before the first blocking read and
restore the clean payload before pivoting back to the input loop. The
`getscankey()` family is only for the 580 target here; `getkey()` is the
nonblocking community chain and has different stack behaviour.

The remaining community programs are catalogued in
`tools/rac-compiler/rsc_ropchain`. Large programs such as 2048, Minesweeper,
GoL and the neural-network demos contain bespoke memory and gadget chains; they
should be added as reference counterparts only after their RAC output is
validated, rather than silently replaced by a different high-level program.

The local notes in `docs/rop-notes/rop-580-guide.txt`, `rop-basics.txt`, and
`rop-ez.txt`, together with the annotated `~/Desktop/45_rop_guide.asm`, are the
ABI references used here. In particular, they confirm little-endian KI/KO
words, the `-2` table target bias, the `E9E0 -> D730` restore loop, and the
distinction between blocking `getscancode()` and nonblocking `getkey()`.

The `community/rsc/` directory contains the original RAC `.rsc` sources. In
CasioEmuMsvc, open them in **Wiz Compiler → File → Load** or the Examples
menu, choose **RSC / RAC (.rsc)**, and press **Build**. RSC mode passes the
file unchanged to the external RAC compiler; it does not translate it through
CWiz.

`community/hexd.cwiz` is a true CWiz port: it uses the compiler's named
`hexd_*` ROP operations and contains no inline RAC or `#raw-rsc` block. The
compiler owns the 580 firmware-specific implementation behind those APIs.
