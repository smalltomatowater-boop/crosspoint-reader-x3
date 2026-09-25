# CrossPoint Reader X3 (Japanese / Terminal fork)

**日本語** | [English](README.en.md)

**XTeInk X3向けのCrossPoint Readerフォーク。** 日本語UI対応、tmuxターミナル、BLEキーボードで書けるテキストエディタ(かな漢字変換つき)を追加した魔改造版。

Based on [crosspoint-reader](https://github.com/crosspoint-reader/crosspoint-reader) by Dave Allie (MIT License).

---

## このフォークで追加したもの

### 日本語UI (Migu1M フォント)
- UIフォントをMigu1M（ひらがな・カタカナ・JIS第1水準漢字 2965字）に置き換え
- フォントデータはフラッシュに埋め込み済み（SDカード不要）
- 設定 → 言語 → 日本語 で切り替え可能

### tmuxターミナル (`TerminalActivity`)
- ホーム画面から「ターミナル」を選択して起動
- Mac/Linux側で `x3_server.py` を起動するとtmuxペインをX3に転送
- BLEキーボードで入力（Wi-Fiとの共存実装は未完）
- フォントはMigu1Mを使用（Migu1Mはモノスペースフォント）

### テキストエディタ (`EditorActivity`)
- ホーム画面の「テキストエディット」から起動(新規)、またはファイル一覧で `.txt` / `.md` を長押し →「編集」
- BLEキーボードで入力(エディタを開くとキーボードを探して自動でペアリング)
- ローマ字かな入力と漢字変換(MS-IME風の単語変換、辞書はSDカードに置く — 下記参照)
- 横向き・12pt・57桁×16行。ファイル全体をメモリに読み込まないので長い文書も扱える
- 保存は電源断に強い方式(一時ファイルに書いてから入れ替え)

### バグ修正
- `EpdFont::getGlyph()` — stubモードでCJKグリフ参照時にnullポインタを踏むバグを修正

---

## テキストエディタの使い方

| キー | 動作 |
|---|---|
| CapsLock | 入力モード切替: ひらがな(あ) → カタカナ(ア) → 英数(AA) |
| Tab | 空白4つ |
| Space | 変換 / 次の候補(ひらがなモードで入力中のとき) |
| Shift+Space | 前の候補 |
| Enter | 確定(入力中のとき)/ 改行 |
| Esc / Backspace | 変換を取り消して読みに戻す |
| 矢印・Home/End・PgUp/PgDn | カーソル移動 |
| Ctrl+S | 保存 |
| Ctrl+Z / Ctrl+Y | 取り消し / やり直し(何段でも。改行・カーソル移動ごとに1回分) |

本体ボタン: 「メニュー」ボタンで 保存 / 名前を付けて保存 / 開く / 新規 / 終了、「« ホーム」ボタンで終了(未保存なら確認あり)。

- 新規文書の「保存」はファイル名を聞かずに `/notes/YYYYMMDD-HHMM.txt` に保存します。時計の日付が未設定のときは `/notes/memo-001.txt` のような連番になります(設定のWi-Fi時刻同期を一度行うと日付入りになります)。
- 一度ペアリングしたキーボードは、次からエディタを開くと自動で再接続します(ペアリングモードは不要)。
- 変換候補には、表示フォント(JIS第1水準)に無い漢字は出ません。
- かなモードでは数字・記号は全角になり、`,` `.` `[` `]` `/` は `、` `。` `「` `」` `・` になります。

### VIモード

設定 →「操作」→「エディタのVIモード」をオンにすると、vi風のモード編集になります(オフなら通常のエディタのまま)。エディタはノーマルモードで始まります。

- 移動: `h` `j` `k` `l`、`w` `b` `e`、`0` `^` `$`、`gg` `G`、Enter、Ctrl-F/B(1画面)、Ctrl-D/U(半画面)。`5j` のように回数も付けられます
- 編集: `x`、`dd`、`D`、`yy`、`p` `P`。コピーした行はSDカード(`/.crosspoint/edit/yank.txt`)に置くので、RAMを使わず、電源を切っても残ります
- 入力モードへ: `i` `a` `I` `A` `o` `O`。入力モードではローマ字かな入力・漢字変換がそのまま使えます
- Esc: 入力中のかなを確定してノーマルモードへ(変換候補の表示中は読みに戻す)。Escを2回続けると英数入力になります
- コマンド: `:w`(保存)、`:w 名前`(名前を付けて保存)、`:e`(ファイル一覧から開く)、`:e 名前`(開く。無ければその名前で新規)、`:e! 名前`(変更を捨てて開く)、`:q` `:q!` `:wq` `:x`。名前に拡張子が無ければ `.txt` が付き、`/` で始まらない名前は今のファイルと同じフォルダ(無題なら `/notes`)になります。コマンド行は英数字のみ
- 取り消し: `u`、やり直し: `Ctrl-R`(何段でも。ノーマルモードのコマンド1つ、または入力モードでの1行が1回分)。消した文字はSDカード(`/.crosspoint/edit/undo.bin`)に置き、保存しても履歴は残ります
- 未対応: ビジュアルモード、`dw` `cw` のようなオペレーターとモーションの組み合わせ

## 漢字変換辞書のセットアップ

辞書(SKK-JISYO.L)はGPLのため、ファームウェアには含めていません。以下の手順でSDカードに置いてください。

```bash
# 1. 辞書を取得
curl -LO https://raw.githubusercontent.com/skk-dev/dict/master/SKK-JISYO.L

# 2. エディタ用の形式に変換(UTF-8化・並べ替え、約4.8MB)
python3 scripts/build_skk_dict.py SKK-JISYO.L skk.txt
```

3. できた `skk.txt` を SDカードの **`/dict/skk.txt`** にコピー(`dict` フォルダは自分で作る)。SDカードを直接コピーしても、ホームの「ファイル転送」からでもOK。

辞書はエディタを開いた時点で読み込みます。コピーしたらエディタを開き直してください。辞書が無いときは、候補欄に「(辞書なし)」と出てカタカナ・ひらがなだけが候補になります。

### 既知の問題
- BLE接続中は空きメモリが少なめです(約18KB)。

---

## Mac側のセットアップ（ターミナル機能）

```bash
cd mac-bridge
pip install flask
python3 x3_server.py
```

ブラウザで `http://localhost:3333/` を開いて設定。

tmuxブリッジだけ使う場合:

```bash
python3 x4_tmux_bridge.py --x4 http://X3_IP_ADDRESS --target x3-terminal:
```

---

## フォントについて

Migu1Mフォントを使用しています。

- **Migu1M**: [itouhiro](https://mix-mplus-ipa.osdn.jp/) 作、M+ FONT LICENSE
- フォントデータは `lib/EpdFont/builtinFonts/migu1m_term_{08,10,12}.h` に埋め込み済み(`scripts/build_migu_ui_fonts.py` で再生成)

M+ FONT LICENSEの条件に従い、本プロジェクトでのMigu1M使用を表記します。

---

## ビルド方法

```bash
# 依存関係
pip install freetype-py pyyaml

# ファームウェアビルド & 書き込み
pio run -t upload
```

---

## ベースプロジェクト

- [crosspoint-reader](https://github.com/crosspoint-reader/crosspoint-reader) — MIT License, Copyright (c) 2025 Dave Allie
- [xteink-terminal](https://github.com/maddiedreese/xteink-terminal) — MIT License, Copyright (c) 2026 Maddie D. Reese
- [Migu1M Font](https://mix-mplus-ipa.osdn.jp/) — M+ FONT LICENSE, Copyright (c) itouhiro
- [SKK-JISYO.L](https://github.com/skk-dev/dict) — GPL-2.0-or-later, SKK Development Team(本リポジトリには含まず、利用者が各自取得。変換スクリプト `scripts/build_skk_dict.py` のみ同梱)

---

MIT License — このフォーク部分の著作権は各コントリビューターに帰属します。
