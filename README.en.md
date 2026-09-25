# CrossPoint Reader X3 (Japanese / Terminal fork)

[日本語](README.md) | **English**

**A CrossPoint Reader fork for the XTeInk X3.** Adds a Japanese UI, a tmux terminal, and a text editor you type into with a BLE keyboard (with Japanese kana-kanji conversion).

Based on [crosspoint-reader](https://github.com/crosspoint-reader/crosspoint-reader) by Dave Allie (MIT License).

---

## What this fork adds

### Japanese UI (Migu1M font)
- The UI font is replaced with Migu1M (hiragana, katakana, and the 2,965 JIS X 0208 level-1 kanji)
- The font data is embedded in flash (no SD card needed)
- Switch with Settings → Language → 日本語

### tmux terminal (`TerminalActivity`)
- Start it from "Terminal" on the home screen
- Run `x3_server.py` on a Mac/Linux machine to mirror a tmux pane to the X3
- Input from a BLE keyboard (running alongside Wi-Fi is not finished yet)
- Uses the Migu1M font (Migu1M is monospaced)

### Text editor (`EditorActivity`)
- Start it from "Text Edit" on the home screen (new document), or long-press a `.txt` / `.md` file in the file browser → "Edit"
- Type with a BLE keyboard. Opening the editor finds the keyboard and pairs automatically
- Romaji-to-kana input and kanji conversion (MS-IME-style word conversion; the dictionary goes on the SD card, see below)
- Landscape, 12pt, 57 columns × 16 rows. The file is never loaded into memory whole, so long documents work
- Power-loss-safe saving (the new file is written to a temporary file, then swapped in)
- Optional vi mode (see below)

### Bug fixes
- `EpdFont::getGlyph()`: fixed a null-pointer dereference when looking up a CJK glyph in stub mode

---

## Using the text editor

| Key | Action |
|---|---|
| Caps Lock | Switch input mode: hiragana (あ) → katakana (ア) → direct/ASCII (AA) |
| Tab | Four spaces |
| Space | Convert / next candidate (while typing in hiragana mode) |
| Shift+Space | Previous candidate |
| Enter | Confirm (while typing) / new line |
| Esc / Backspace | Cancel conversion and go back to the reading |
| Arrows, Home/End, PgUp/PgDn | Move the cursor |
| Ctrl+S | Save |
| Ctrl+Z / Ctrl+Y | Undo / redo (multi-level; each line or cursor move starts a new step) |

Device buttons: "Menu" opens Save / Save As / Open / New / Exit. "« Home" leaves the editor (asks first if there are unsaved changes).

- "Save" on a new document doesn't ask for a name. It saves to `/notes/YYYYMMDD-HHMM.txt`. If the clock has no date yet, the name is a number such as `/notes/memo-001.txt`. Sync the clock over Wi-Fi once ("Sync clock now" in Settings) to get dated names.
- A keyboard you've paired once reconnects automatically the next time you open the editor. No pairing mode needed.
- Kanji the display font lacks (anything outside JIS level 1) are not offered as candidates.
- In kana mode, digits and symbols are full-width, and `,` `.` `[` `]` `/` become `、` `。` `「` `」` `・`.

### Vi mode

Turn on Settings → "Controls" → "Editor Vi Mode" for vi-style modal editing. With it off, the editor works as described above. With it on, the editor starts in Normal mode.

- Motion: `h` `j` `k` `l`, `w` `b` `e`, `0` `^` `$`, `gg` `G`, Enter, Ctrl-F/B (one screen), Ctrl-D/U (half a screen). Counts work too, e.g. `5j`. In Japanese text, `w` / `b` stop where the script changes (hiragana, katakana, kanji).
- Editing: `x`, `dd`, `D`, `yy`, `p` `P`. Yanked text is kept on the SD card (`/.crosspoint/edit/yank.bin`), so it uses no RAM and survives power-off.
- Visual mode: `v` (characters), `V` (lines), `Ctrl-V` (block). Extend the selection with motions, then `d`/`x` delete, `y` yank, `c` change, `o` jump to the other end; Esc leaves. The arrow keys work too.
- Block: columns are display cells (full-width characters are 2), and `j`/`k` move by logical line. `I`/`i` and `A`/`a` insert what you type on the first line into every line when you press Esc (Japanese works too). `p` pastes a yanked block as a column.
- Into Insert mode: `i` `a` `I` `A` `o` `O`. Romaji-kana input and kanji conversion work as usual in Insert mode.
- Esc: commits the kana being typed and goes to Normal mode (while a candidate is shown, it goes back to the reading first). Two Esc presses in a row switch to direct/ASCII input.
- Commands:
  - `:w` saves; `:w name` saves under a new name.
  - `:e` opens the file browser. `:e name` opens that file, or starts a new document with that name if it doesn't exist. `:e! name` opens it and discards unsaved changes.
  - `:q`, `:q!`, `:wq`, `:x` quit, as in vi.
  - A name with no extension gets `.txt`. A name that doesn't start with `/` goes in the current file's folder, or `/notes` for an untitled document.
  - The command line takes ASCII only.
- Caps Lock does nothing in Normal mode.
- Undo: `u`; redo: `Ctrl-R`. Multi-level; one Normal-mode command, or one line typed in Insert mode, is one step. Deleted text is kept on the SD card (`/.crosspoint/edit/undo.bin`), and saving keeps the history.
- Not supported: operator + motion combinations such as `dw` or `cw`.

## Setting up the kanji dictionary

The dictionary (SKK-JISYO.L) is GPL, so it isn't included in the firmware. Put it on the SD card like this:

```bash
# 1. Get the dictionary
curl -LO https://raw.githubusercontent.com/skk-dev/dict/master/SKK-JISYO.L

# 2. Convert it for the editor (to UTF-8, sorted; about 4.8MB)
python3 scripts/build_skk_dict.py SKK-JISYO.L skk.txt
```

3. Copy the resulting `skk.txt` to **`/dict/skk.txt`** on the SD card (create the `dict` folder yourself). Copy it to the card directly or use "File Transfer" on the home screen.

The dictionary is loaded when the editor opens, so reopen the editor after copying it. Without a dictionary, the candidate row shows "(no dict)" and only katakana and hiragana are offered.

### Known issues
- Free memory is low while a BLE keyboard is connected (about 18KB).

---

## Mac setup (terminal)

```bash
cd mac-bridge
pip install flask
python3 x3_server.py
```

Open `http://localhost:3333/` in a browser to configure it.

To use only the tmux bridge:

```bash
python3 x4_tmux_bridge.py --x4 http://X3_IP_ADDRESS --target x3-terminal:
```

---

## Fonts

This fork uses the Migu1M font.

- **Migu1M**: by [itouhiro](https://mix-mplus-ipa.osdn.jp/), M+ FONT LICENSE
- The font data is embedded in `lib/EpdFont/builtinFonts/migu1m_term_{08,10,12}.h` (regenerate with `scripts/build_migu_ui_fonts.py`)

As the M+ FONT LICENSE requires, this notes that the project uses Migu1M.

---

## Building

```bash
# Dependencies
pip install freetype-py pyyaml

# Build and flash the firmware
pio run -t upload
```

---

## Upstream projects

- [crosspoint-reader](https://github.com/crosspoint-reader/crosspoint-reader): MIT License, Copyright (c) 2025 Dave Allie
- [xteink-terminal](https://github.com/maddiedreese/xteink-terminal): MIT License, Copyright (c) 2026 Maddie D. Reese
- [Migu1M Font](https://mix-mplus-ipa.osdn.jp/): M+ FONT LICENSE, Copyright (c) itouhiro
- [SKK-JISYO.L](https://github.com/skk-dev/dict): GPL-2.0-or-later, SKK Development Team. Not included in this repository; each user downloads it. Only the conversion script `scripts/build_skk_dict.py` is included.

---

MIT License. Copyright in this fork's additions belongs to each contributor.
