# handoff.md — BLE Text Editor Feature

Status: **M2 done and flashed to a real X4** (branch `feature/ble-text-editor`,
started 2026-09-24). Plan of record: see Claude plan "CrossPoint Text Editor —
BLE Keyboard Note-Taking Feature" (approved). Milestones M1-M4.

**On-device pass done 2026-09-24, real X4 + a real BLE keyboard: typing
works.** Editor enters/renders/exits cleanly; the keyboard connects, NimBLE
auto-pairs (bonded, keys in NVS), all 5 HID report characteristics
subscribe, and the owner confirmed text input on screen. Three real bugs
found and fixed in `BleHidClient.cpp` this session (see "Bugs found and fixed
on device" below). **Still open**: BLE costs ~61KB while scanning/connected,
leaving only ~18KB free with the keyboard connected — under the project's
50KB-headroom rule (see "Heap budget"; real measured data, not an estimate).
Verified on device by the owner: ASCII and kana input, Tab mode cycling,
cursor position, arrows, PgUp/PgDn, Home/End, scrolling up past a line break.
Save (auto-named) and reopen verified too; Open/New/discard-confirm not yet exercised.

## Owner decisions from on-device testing (2026-09-24)

- **Font: Migu 1M 12pt → 60 cols × 17 rows** (cell 13×28 px). This overrides
  requirement #2's ~80×24: the owner first set "half-width 40 cols × 25 rows"
  as the minimum, then chose 12pt knowing it only reaches 17 rows — 8pt was
  too small to read. 25 rows would need a ≤20px pitch; 12pt's glyph extent
  (ascender+descender) is 28px, so the pitch can't be tightened. 10pt and
  12pt are both already loaded at boot as UI fonts, so the editor now reuses
  `UI_12_FONT_ID` and no longer loads its own 8pt copy.
- **Kana mode: digits and symbols are full-width** (U+FF01-FF5E), with
  `,`→`、` `.`→`。` `[`→`「` `]`→`」` `/`→`・` (common IME defaults).
  Letters (Shift+letter) and space stay half-width. ASCII mode is unchanged.

## Bugs found and fixed on device (second pass)

- Cursor drifted left toward the right edge: cell width came from
  `getTextWidth("A")` (ink bounding box) instead of the glyph advance. Now
  `getTextAdvanceX`, and the cursor x is the measured advance of the row
  prefix, so it can't drift even if the cell model is off (measured: kana
  advance is 25 px, not 2×13).
- Could not scroll back up after the viewport had scrolled past a newline:
  `findPreviousRowStart()` treated the newline ending the previous row as a
  line start, returning `beforePos` itself.
- Home menu couldn't reach Settings: `HomeActivity::getMenuItemCount()` still
  said 5 after M1 added Text Edit as a 6th item.
- Home/End "not working" was not a bug — tested on empty lines (start ==
  end). Raw HID dump confirmed the keyboard sends standard 0x4A/0x4D.

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

## M2 design decisions (owner hasn't seen these yet — flag, don't assume)

These fill gaps the plan didn't spell out. Reasoned through carefully, but
they're this session's calls, not pre-approved:

- **Input-mode switching**: Tab cycles Hiragana → Katakana → ASCII (direct,
  unconverted typing). JIS Katakana/Hiragana/Henkan/Muhenkan usages
  (~0x87-0x92) are deliberately *not* mapped — couldn't verify their exact
  assignments without a device (CLAUDE.md anti-hallucination rule), and Tab
  needs no such knowledge and works on any keyboard. Status row shows the
  current mode (あ/ア/AA).
- **HID usage → ASCII table**: only the standard Boot Keyboard range (letters,
  digits, common punctuation 0x2D-0x38, arrows, Home/End/PgUp/PgDn,
  Enter/Backspace/Delete/Space/Tab) — high-confidence, well-established USB
  HID values. No JIS-specific usages at all (see above).
- **Backspace while composing** (RomajiKana has a pending prefix) discards the
  whole pending prefix rather than editing it character-by-character. Simpler;
  matches "keyboard-only cursor, not fine-grained IME editing" spirit of v1.
- **Display-row layout is computed locally, not indexed globally**: no
  whole-document line-start table exists anywhere. Cursor Up/Down/Home/End
  rewrap a small window around the cursor on demand (`EditorActivity::
  relayout()`, `findPreviousRowStart()` — bounded backward scan, capped at
  ~4KB, so a single pathologically long line without any `\n` within that
  window may wrap slightly imprecisely when scrolling up into it). This is
  what makes "never load the whole file" actually hold for cursor movement,
  not just for reads.
- **SaveAs target directory**: always the currently-open file's own folder (or
  `/` for a never-saved document) — there's no directory picker in v1, just a
  filename prompt (`KeyboardEntryActivity`, physical-button on-screen
  keyboard, already existed for this exact "no touchscreen" reason).
- **Crash-safe save**: SdFat's `rename()` refuses to overwrite an existing
  destination (`O_CREAT|O_EXCL` internally — see FatFile.cpp:974), so a naive
  "remove old, rename tmp into place" has a real window where *neither* file
  exists if power is lost in between. `EditorDocument::consolidateTo()`
  instead does old→`.bak`, tmp→target, delete `.bak` — recoverable from a
  crash at any point. This is a *new* pattern for this codebase; the existing
  `.bak` files elsewhere (`settings.bin.bak` etc.) are one-time
  bin→json-migration artifacts, not a general atomic-save mechanism — don't
  assume that convention already existed.
- **No crash-recovery of unsaved edits**: every `EditorDocument::open()`
  truncates its scratch `.edit`/`.flat` files fresh, discarding any leftover
  from a previous crashed session. Explicit `save()` *is* crash-safe (above);
  the live in-RAM edit buffer between saves is not — same tradeoff every
  editor without a recovery journal makes.

## BLE facts (verified 2026-09-23/24)

- `BleKeyboard` lifecycle is sound: component-owned FreeRTOS task (4KB) does
  NimBLE init/scan/connect/subscribe; key queue drained by `loop()` on main
  task; `stop()` joins task + `NimBLEDevice::deinit(false)` returns heap.
  Keep this pattern (TerminalActivity precedent vs docs/activity-manager.md
  task rule — task is joined before onExit returns).
- Problems fixed by the `src/ble/BleHidClient.h` extraction (M1): delivers
  raw usage+mods `HidKeyEvent` (not tmux key-name strings), Alt no longer
  discarded, US punctuation table bug at 0x31 fixed, boot-report-only parsing
  (report-ID heuristic). Still **no pairing/bonding** (v1 scope) and no JIS
  usage mapping (M2 punted on this deliberately — see design decisions above).
- **Pairing works (Just Works, bonded).** The earlier "v1 = unencrypted only,
  encrypted keyboards fail the subscribe" diagnosis was **wrong** — the
  subscribe never actually ran (bug 3 below). HOGP requires encryption for
  HID reports, so effectively every real BLE keyboard needs pairing; NimBLE
  handles it on demand when the subscribe write hits an insufficient-
  encryption error (NimBLERemoteValueAttribute.cpp:76-79), and
  `setSecurityAuth(bond=true, mitm=false, sc=true)` in `bleTaskRun()` keeps
  the bond in NVS. Not yet handled: keyboards that insist on MITM/passkey
  entry (would need `setSecurityIOCap(BLE_HS_IO_DISPLAY_ONLY)` plus an
  `onPassKeyDisplay` override that shows the 6-digit code in the status
  row), and a UI to forget bonds (`NimBLEDevice::deleteAllBonds()`).
- `BleHidClient::discardPending()` exists precisely for the push-sub-activity
  case (menu/file-picker/keyboard-entry on top of the editor) — EditorActivity
  calls it before *and* after every such push so stray keystrokes typed while
  a sub-activity had focus never land in the document.
- Existing repo has NO partial-refresh usage; `displayWindow()` takes
  physical un-rotated coords (GfxRenderer.cpp:1144) — use full-screen
  FAST_REFRESH differential (TerminalActivity 150ms rate-limit pattern).
- Fonts: MIGU1M_TERM_08 loaded from flash via `renderer.replaceFont`
  (TerminalActivity.cpp:269-276) — Japanese-capable monospace precedent.
  Confirmed (TerminalActivity's own IP screen) that `drawText` handles mixed
  ASCII/kana-width UTF-8 correctly in one call — the editor's per-row
  rendering relies on this rather than drawing cell-by-cell.

## Bugs found and fixed on device (2026-09-24, this session)

All in `src/ble/BleHidClient.cpp`, all pre-existing from M1 (not introduced
by M2's EditorActivity work), all found by actually connecting a real BLE
keyboard rather than by inspection:

1. **Retry loop despite the "don't retry-loop" comment.** `connectToDevice()`
   set `subscribeFailed_ = true` on a refused HID-report subscribe, but
   `bleTaskRun()`'s loop never checked that flag before rescanning — the
   comment described the intent, the code didn't implement it. Observed on
   device as `Scanning... -> HID device found -> Connecting... -> No
   notifiable HID report -> Scanning...` repeating every ~10s against the
   same keyboard. Fixed: both restart paths in `bleTaskRun()` now also
   require `!subscribeFailed_`. `connectToDevice()` still resets the flag to
   false at its own start, so a fresh `begin()` (re-entering the editor)
   retries cleanly; within one session, a subscribe failure now latches.
2. **Client leak once (1) was fixed.** The old retry loop's *only* purpose in
   practice was also what deleted the stale `NimBLEClient*` (at the top of
   the next `connectToDevice()` call). Stopping the retry loop therefore
   stopped that cleanup too — a subscribe failure now left `client_` (and
   NimBLE's per-connection GATT cache) permanently allocated for the rest of
   the session. Measured on device: free heap dropped from ~84KB (right
   after BLE init) to ~22-24KB after one failed connect+subscribe cycle, and
   *stayed there*, unrecovered, no matter how long the editor sat idle.
   Fixed: `connectToDevice()` now calls `NimBLEDevice::deleteClient(client_)`
   immediately in the subscribe-failure branch (matching the pattern the
   "connection failed" branch right above it already used), instead of
   deferring cleanup to a next attempt that, post-fix-1, might never come.
   Confirmed fixed: free heap now returns to within ~1KB of its
   pre-connection-attempt value after a failed connect, repeatably.

3. **The subscribe loop never ran — the real reason every keyboard
   "failed".** `connectToDevice()` iterated `svc->getCharacteristics(false)`,
   which returns NimBLE's cached vector — empty until characteristics have
   been discovered once (NimBLERemoteService.cpp:118-124). Zero iterations,
   `subscribed` stayed false, and it logged "pairing required?" ~100ms after
   connecting, far too fast for any pairing to have been attempted. Fixed:
   `getCharacteristics(true)`. With that plus bonding enabled, the test
   keyboard paired automatically (~1.8s), subscribed 5 report
   characteristics, and typing worked. Bugs 1 and 2 are still worth keeping
   fixed: a keyboard that genuinely refuses pairing would otherwise hit the
   same retry loop and leak.

Bugs 1-2 are committed in `c1e4f033`; bug 3 in the commit after it. All
flashed to the test X4.

## Heap budget (rule: >50KB headroom at all times) — **measured on device, rule not currently met**

Original estimate was NimBLE ~40KB + editor ~40KB ≈ 78-85KB total. Real
measurements from this session (same X4, `default` build, `LOG_DBG "MEM"`,
after both BLE fixes above):

| Point | Free heap |
|---|---|
| Home screen (baseline, no editor, no BLE) | ~108-127KB |
| `EditorActivity::onEnter()`, right before `bleHid_.begin()` | ~99-102KB |
| Right after `bleHid_.begin()` returns (NimBLE init + task spawn, scan not yet running) | ~84KB |
| Once `NimBLEScan` is actually active (`setActiveScan(true)`, before any device is found) | **~22KB** |
| After a full connect → subscribe-fail → disconnect → `deleteClient` cycle | ~21-22KB (stable, no further drop) |
| Keyboard connected, paired, 5 reports subscribed (after bug 3 fix) | **~18KB** (MaxAlloc ~15KB; MinFree drifted ~130 bytes over 40s — watch for a slow leak over a long session) |

**The dominant cost is active BLE scanning itself (~61KB), not the editor and
not the connection attempt.** This is a real, currently-unmet gap against the
project's own 50KB-headroom rule — it is not the leak that got fixed above
(that leak was a separate, additional problem on top of this baseline). Open
questions for next session, roughly in order of how likely they are to help:
- Is `setActiveScan(true)` (active scan, requests scan-response data from
  every advertiser) meaningfully more expensive than passive scanning here?
  The editor only needs to match on HID-service UUID in the advertisement
  itself (`onResult()` already checks `haveServiceUUID()`/
  `isAdvertisingService()` before doing anything else) — passive scan may be
  sufficient and cheaper.
- `NimBLEScan::setInterval(100)`/`setWindow(99)` — a near-100%-duty-cycle
  scan window; check whether NimBLE's scan result cache size scales with
  how many advertisements it sees, and whether a shorter window (still fine
  for a stationary keyboard, unlike a moving BLE peripheral) reduces it.
- NimBLE-Arduino build-time config (`NIMBLE_MAX_CONNECTIONS`,
  `CONFIG_BT_NIMBLE_MAX_CCCDS`, scan-list size, etc.) — this project already
  vendors NimBLE-Arduino 2.5.0 (see the BLE facts above); check whether its
  current config is tuned for a use case (e.g. multi-connection central)
  that this single-keyboard editor doesn't need.
- Whether ~22KB is actually *survivable* in practice (nothing else running
  needs much heap while the editor has focus) even though it's under the
  formal 50KB rule — worth deciding explicitly rather than by default,
  since "the rule says 50KB" and "does anything actually break at 22KB"
  are different questions with possibly different answers.

The "chunk table ≤3KB" line item from the original plan was dropped:
`PieceTable`'s array is capped at 1536 pieces (~18KB) and a linear scan of
that is trivially fast at 160MHz (sub-millisecond even in the worst case), so
no secondary index was needed — see PieceTable.h's class comment.

The table above measures the editor's own pre-BLE cost only coarsely (Home
baseline ~108-127KB → Editor-before-BLE ~99-102KB, a 6-9KB drop) — that
number is muddied by `Home`'s own deallocation happening in the same
`replaceActivity()` step that constructs `EditorActivity`, so it's not a
clean isolated reading of `EditorDocument`'s ~18.4KB `PieceTable` + 8KB edit
head + the activity's own 4KB `viewportBuf_`. Get an isolated number next
session by reading `ESP.getFreeHeap()` right before and right after
`document_.open()` specifically (not the existing before/after-BLE markers,
which straddle the wrong boundary for this), and again after opening a file
large enough to actually exercise `flatten()`.

## Research conclusions (don't redo)

- BLE HID host libraries: all popular ESP32 ones are device-side (wrong
  direction); esp32beans demo-level. → self-refactor of BleKeyboard.
- arduino_skk (Uno R3 + SD SKK dict): **no license** — not usable as a code
  dependency. Romaji→kana = own constexpr flash table + FSM (~150 rules,
  built and host-tested). For v2 kanji, SD-based SKK dict approach remains
  the candidate (reimplement, don't copy).
- SdFat `rename()` cannot overwrite an existing destination — see the
  crash-safe-save design decision above. Anyone touching save/flatten logic
  needs to know this before "simplifying" it.

## Where things live (M2 outcome, update as built)

- `src/ble/BleHidClient.h/.cpp` — reusable NimBLE HID client (HidKeyEvent). M1.
- `src/activities/terminal/BleKeyboard.*` — thin adapter over BleHidClient,
  same public API (TerminalActivity untouched). M1.
- `lib/Editor/RomajiKana.h/.cpp` — romaji→kana FSM. Host-tested, 15 cases
  green (`test/romaji_kana/RomajiKanaTest.cpp`). M1/M2.
- `lib/Editor/PieceTable.h/.cpp` — storage-agnostic piece-table index (the
  actual "never load the whole file" data structure). Pure logic, zero I/O —
  deliberately factored out of `EditorDocument` specifically so this, the
  fiddliest part, is host-testable without SD hardware. Host-tested, 19
  cases green including a 500-step randomized fuzz test against a
  `std::string` reference (`test/piece_table/PieceTableTest.cpp`). Known,
  documented, unavoidable limitation: "type a char, backspace it, retype"
  splices a second piece instead of re-extending the first, because the
  backing Edit stream is append-only (see the class comment on `insert()`) —
  content is still correct, piece count is just one above the theoretical
  minimum in that one case.
- `lib/Editor/EditorDocument.h/.cpp` — thin SD-I/O glue around `PieceTable`:
  owns the Base (original file, or last flatten/save snapshot) and Edit
  (append-only) file handles plus an 8KB in-RAM write-combining head.
  Crash-safe `save()`/`flatten()` via `consolidateTo()`'s backup-rotate
  rename swap (see design decisions above). **Not host-testable** — depends
  on `HalFile`, which can't be constructed off-device — so this file's
  correctness rests on careful reading plus `pio run`/`pio check` passing
  clean, not on an executed test. Verify on device before trusting it with
  real notes.
- `src/activities/editor/EditorActivity.h/.cpp` — the editor. M1's grid/BLE
  skeleton now wired to real content: BLE keys → HID-usage-to-ASCII table →
  RomajiKana → `EditorDocument`, keyboard-only cursor (arrows/Home/End/PgUp/
  PgDn/Backspace/Delete/Enter), viewport-follow scrolling computed on demand
  (no whole-document index — see design decisions above), Tab-cycled input
  mode, idle-triggered `flushEditHead()`/`flatten()` (2s idle debounce), and
  the Confirm-button Save/SaveAs/Open/New/Exit menu with unsaved-changes
  confirmation on Open/New/Exit-while-dirty. **This is the least-verified
  file in the feature** — see "What needs on-device verification" below.
- `src/activities/util/OptionMenuActivity.h/.cpp` — generic vertical list
  chooser (title + list of (label, action-id), returns `MenuResult` or
  cancelled). Built on the same `GUI.drawList`/`ButtonNavigator` primitives
  every other simple picker activity in this codebase already uses (e.g.
  `NetworkModeSelectionActivity`) — not a new UI pattern, just the first
  *reusable* instance of one that already existed many times over.
- FileBrowser: `Mode::PickTextFile` (mirrors the existing `PickFirmware`
  pattern — filters to `.txt`/`.md`, returns the path via `FilePathResult`,
  Back-at-root cancels instead of going Home). Long-press on a `.txt`/`.md`
  file in `Mode::Books` now opens an `OptionMenuActivity` (Read/Edit/Delete)
  instead of going straight to the delete confirmation — every other
  file/directory type's long-press behavior is unchanged. "Read" reuses the
  pre-existing `onSelectBook()` routing (there's already a dedicated
  `TxtReaderActivity` for plain-text books, untouched by any of this).
- i18n: `STR_EDITOR_MENU/SAVE/SAVE_AS/NEW_FILE/READ/EDIT/SELECT_TEXT_FILE/
  NO_TEXT_FILES/UNSAVED_CHANGES/DISCARD_CHANGES_BODY/UNTITLED/
  ENTER_FILENAME/SAVED/SAVE_FAILED` added to both `english.yaml` and
  `japanese.yaml`, generated files regenerated via `scripts/gen_i18n.py`.
  `SAVED`/`SAVE_FAILED` keys exist but aren't rendered anywhere yet (no
  transient status-message UI was built) — either wire them up or remove
  them before this ships.
- Modified in M1 (unchanged since): ActivityManager (`HomeMenuItem::EDITOR`,
  `goToEditor`), HomeActivity (6th menu item).

## Verification gates

`pio run` (default + gh_release) — **passing**. `pio check` — **passing**
(cppcheck clean except one deliberately-left low-severity style nit, see
`EditorActivity.cpp`'s `hidUsageToAscii`). clang-format — **clean**. Host
tests (`cmake --build test/build && ctest` or run the two test binaries
directly) — **34/34 green** (15 RomajiKana + 19 PieceTable, including the
randomized fuzz test).

**Everything below this line still needs a real device and a real BLE
keyboard — none of it has been exercised:**

## What needs on-device verification (do this first, in this order)

1. Heap: **mostly done** — see "Heap budget" above for the real numbers and
   the open questions on reducing BLE scan cost. `onExit()` confirmed to
   return heap fully to the Home-screen baseline (~108KB) after a full
   enter → BLE-scan → connect-fail → exit cycle — no session-to-session
   leak. Still needed: the isolated `EditorDocument::open()` before/after
   reading described at the end of that section.
2. BLE connect + the HID usage table: type the alphabet, digits, and every
   punctuation key in ASCII mode; confirm each lands as the right character.
   This table was written from memory of the USB HID Boot Keyboard spec, not
   copied from a verified source — it's the single highest-confidence-but-
   unverified piece of this session's work.
3. Romaji→kana in Hiragana/Katakana mode, Tab cycling, a few of the trickier
   RomajiKana cases live (sokuon, "n" before a consonant, "n'") — the FSM
   itself is host-tested, but *feeding it from real BLE key events through
   the ASCII table* is not.
4. Cursor movement: arrows across a wrapped line, across a real newline,
   Up/Down through lines of different lengths (goal-column memory), Home/
   End, PageUp/PageDown, and specifically scrolling *up* past the top of the
   viewport (the bounded-backward-scan path, `findPreviousRowStart()`) — this
   is the least-traced-by-hand piece of logic in the whole feature.
5. Save / Save As / Open / New, including the discard-unsaved-changes
   confirmation, and — critically — a save that's interrupted by yanking
   power (or at least reasoning about it again with the device's actual SD
   card behavior in hand) to sanity-check the `.bak` rotation.
6. Orientation restore on exit, ghosting/refresh behavior over a longer
   editing session, ~80-column rendering at the real font metrics (`maxCols_`/
   `maxRows_` are computed from `renderer.getTextWidth`/`getLineHeight` at
   runtime, not hardcoded, but were never checked against the real display).

## Kanji conversion (done 2026-09-24, verified on device)

Owner chose MS-IME-style single-word conversion (not SKK-style input) and
SKK-JISYO.L.

- **Dictionary**: `scripts/build_skk_dict.py SKK-JISYO.L skk.txt` converts
  EUC-JP → UTF-8, strips annotations/Lisp candidates, keeps hiragana readings
  (+ SKK okuri-ari keys like `かk`), and sorts by UTF-8 bytes. Output (~4.8MB,
  147,500 readings, longest line 970 B) goes on the SD card at
  `/dict/skk.txt`. The dictionary is GPL, so it is **not** in the repo or the
  firmware — users get it from https://github.com/skk-dev/dict.
- **Lookup** (`lib/Editor/SkkDictionary`): binary search directly on the SD
  file (~18 seeks), one 1KB line buffer per lookup, nothing kept in RAM.
- **Candidates** (`lib/Editor/KanaConverter`): whole reading, then the last
  1-3 kana as okurigana (stem + SKK okuri letter, e.g. かんじる → かんz →
  感じる), then katakana, then the reading. No frequency learning; order is
  the dictionary's.
- **Editor**: the composition is kept *in the document* at
  `[compStart_, cursorPos_)` (underlined; thick while converting), so layout,
  scrolling and rendering needed no new path. Space/Shift+Space cycle,
  Enter confirms, Esc/Backspace revert to the reading, any other key
  confirms first. `commitComposition()` is the single commit seam.
- Host tests: `test/kana_converter` (11 cases; `SKK_DICT=path/to/skk.txt`
  also runs lookups against the real converted file).
- Known limits: the dictionary is only opened on entering the editor (copy
  it before entering); compositions over 32 kana aren't converted; no
  bunsetsu segmentation.
- Candidates show on a reserved top row (blank when not converting), so
  the grid is 57×16 text rows; owner accepted losing a row for this.

## Save flow and orientation (done 2026-09-24, verified on device)

- Owner found saving hard: menus and the filename keyboard were rendered in
  the editor's landscape orientation, where the physical buttons don't line
  up with the screen. `EditorActivity::startSubActivity()` now shows every
  sub-activity (menu, file picker, keyboard entry, confirm dialog) in the
  normal UI orientation and restores landscape when it returns. The text
  grid itself stays landscape (owner's choice).
- "Save" on an untitled document no longer asks for a name: it writes
  `/notes/YYYYMMDD-HHMM.txt` (local time, `SETTINGS.clockUtcOffsetQ`), or
  `/notes/memo-NNN.txt` if the RTC has no date yet, adding `-2`, `-3`… on
  collisions. Save As prefills the same name.
- The RTC date: `HalClock` only ever wrote H:M:S, so there was no date to
  read. `syncFromNTP()` now also writes the DS3231 date registers
  (0x04-0x06) and `HalClock::getLocalDateTime()` reads date+time with the
  UTC offset applied (date rollover handled; the day-count math was
  host-checked against gmtime over ~24k dates). Dates only become valid
  after one NTP sync on this firmware — until then names fall back to
  `memo-NNN`.

## Not started (M3/M4)

- Anything in the original plan not called out as done above.
