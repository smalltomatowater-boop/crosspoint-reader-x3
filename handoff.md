# handoff.md — BLE Text Editor Feature

Status: **in progress** (branch `feature/ble-text-editor`, started 2026-09-24)
Plan of record: see Claude plan "CrossPoint Text Editor — BLE Keyboard
Note-Taking Feature" (approved). Milestones M1-M4.

## Why this feature (context for future sessions)

The X4 becomes a note-taking text editor driven by a BLE HID keyboard.
A BLE HID host already existed for the X3 terminal feature
(`src/activities/terminal/BleKeyboard.*`, commit `a622e73c`, 2026-05-29) but
was **disabled** in TerminalActivity (`TerminalActivity.cpp:301-305`)
because NimBLE's ~40KB heap cost collided with the WiFi WebServer
(MaxAlloc → ~2KB). The editor does **not use WiFi**, so that conflict
disappears — this is the key insight that unblocks the feature.

## Confirmed requirements (owner decisions — do not re-litigate)

1. `.txt`/`.md` openable into the editor from FileBrowser (long-press menu:
   Read / Edit / Delete); new-file creation; crash-safe save.
2. Terminal-style grid: ~80 cols × 24 text rows + 1 status row, kana = 2
   cells. **Landscape-only** (force in onEnter, restore in onExit).
3. Romaji→kana (hira/kata) in v1. Kanji conversion is v2 — all kana emission
   must go through a single `commit` seam in EditorActivity so an SKK-style
   layer can be inserted later without touching RomajiKana/EditorDocument.
4. Effectively unlimited text: piece table over immutable base file + SD
   append store (`/.crosspoint/edit/<hash>.*`); flatten on idle when pieces
   > 1536. Never load whole file.
5. **Keyboard-only cursor** (arrows/Home/End/PgUp/PgDn). Physical buttons do
   NOT move the cursor: Confirm = menu (Save/SaveAs/Open/New/Exit), Back =
   exit with unsaved-changes check. PageBack/PageForward = nothing in v1.
   BLE state must be visible on the status bar (no keyboard ⇒ view-only).
6. Home menu: 「テキストエディット」/ "Text Edit" between Terminal and Settings.
7. Cursor semantics: viewport-follow (cursor always visible; scroll 1 row at
   the edge), wrap-crossing Left/Right (newline costs one cell), Up/Down by
   display row with goal-column memory, Home/End on display row.

## BLE facts (verified 2026-09-23/24)

- `BleKeyboard` lifecycle is sound: component-owned FreeRTOS task (4KB) does
  NimBLE init/scan/connect/subscribe; key queue drained by `loop()` on main
  task; `stop()` joins task + `NimBLEDevice::deinit(false)` returns heap.
  Keep this pattern (TerminalActivity precedent vs docs/activity-manager.md
  task rule — task is joined before onExit returns).
- Problems being fixed by the `src/ble/BleHidClient.h` extraction:
  delivers tmux key-name strings (want raw usage+mods `HidKeyEvent`),
  no JIS keys (Henkan/Muhenkan/Katakana/Eisu intl usages 0x85-0x8A), Alt
  discarded, US punctuation table bug at 0x31, boot-report-only parsing
  (report-ID heuristic needed), **no pairing/bonding**.
- **v1 pairing = unencrypted connections, documented.** Keyboards requiring
  encryption fail the 2A4D subscribe → surface in status, don't retry-loop.
  v2 path exists in vendored NimBLE-Arduino 2.5.0:
  `NimBLEDevice::setSecurityAuth` (NimBLEDevice.h:148),
  `NimBLEClient::secureConnection()` (NimBLEClient.h:72), bond address in
  `/.crosspoint/ble_keyboard.json` (WifiCredentialStore pattern).
- Existing repo has NO partial-refresh usage; `displayWindow()` takes
  physical un-rotated coords (GfxRenderer.cpp:1144) — use full-screen
  FAST_REFRESH differential (TerminalActivity 150ms rate-limit pattern).
- Fonts: MIGU1M_TERM_08 loaded from flash via `renderer.replaceFont`
  (TerminalActivity.cpp:269-276) — Japanese-capable monospace precedent.

## Heap budget (rule: >50KB headroom at all times)

NimBLE ~40KB + editor ~40KB (pieces 18KB reserved, edit head 8KB, windows
4KB, chunk table ≤3KB, misc) ≈ 80-87KB total. Font data stays in flash.
Measure baseline on device (LOG_DBG "MEM") in M1; verify exit returns heap.

## Research conclusions (don't redo)

- BLE HID host libraries: all popular ESP32 ones are device-side (wrong
  direction); esp32beans demo-level. → self-refactor of BleKeyboard.
- arduino_skk (Uno R3 + SD SKK dict): **no license** — not usable as a code
  dependency. Romaji→kana = own constexpr flash table + FSM (~140 rules);
  romaji algorithm refs: zenn.dev「SKK実装入門」. For v2 kanji, SD-based SKK
  dict approach remains the candidate (reimplement, don't copy).

## Where things live (M1 outcome, update as built)

- `src/ble/BleHidClient.h/.cpp` — reusable NimBLE HID client (HidKeyEvent).
- `src/activities/terminal/BleKeyboard.*` — thin adapter over BleHidClient,
  same public API (TerminalActivity untouched).
- `src/activities/editor/EditorActivity.h/.cpp` — the editor.
- `lib/Editor/EditorDocument.*` — piece table + SD stores.
- `lib/Editor/RomajiKana.*` — pure C++, host-testable (test/romaji_kana).
- `src/activities/util/OptionMenuActivity.*` — generic button chooser.
- Modified: ActivityManager (HomeMenuItem::EDITOR, goToEditor), HomeActivity
  (6th menu item), FileBrowserActivity (PickTextFile mode + long-press
  menu), i18n yamls (~15 keys).

## Verification gates

`pio run` (default + gh_release), `pio check`, clang-format clean, romaji
ctest green on host BEFORE flashing. On device: heap before/after BLE, BLE
connect + JIS keys in logs, save/reload roundtrip, orientation restore,
cursor rules, ghosting. Human tester = owner.
