# CrossPoint Reader X3 (Japanese / Terminal fork)

**XTeInk X3向けのCrossPoint Readerフォーク。** 日本語UI対応、tmuxターミナル、BLEキーボードで書けるテキストエディタ(かな漢字変換つき)を追加した魔改造版。

Based on [crosspoint-reader](https://github.com/crosspoint-reader/crosspoint-reader) by Dave Allie (MIT License).

---

## このフォークで追加したもの

### 日本語UI (Migu1M フォント)
- UIフォントをMigu1M（ひらがな・カタカナ・常用漢字サブセット）に置き換え
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
| Tab | 入力モード切替: ひらがな(あ) → カタカナ(ア) → 英数(AA) |
| Space | 変換 / 次の候補(ひらがなモードで入力中のとき) |
| Shift+Space | 前の候補 |
| Enter | 確定(入力中のとき)/ 改行 |
| Esc / Backspace | 変換を取り消して読みに戻す |
| 矢印・Home/End・PgUp/PgDn | カーソル移動 |

本体ボタン: 「メニュー」ボタンで 保存 / 名前を付けて保存 / 開く / 新規 / 終了、「« ホーム」ボタンで終了(未保存なら確認あり)。

- 新規文書の「保存」はファイル名を聞かずに `/notes/YYYYMMDD-HHMM.txt` に保存します。時計の日付が未設定のときは `/notes/memo-001.txt` のような連番になります(設定のWi-Fi時刻同期を一度行うと日付入りになります)。
- かなモードでは数字・記号は全角になり、`,` `.` `[` `]` `/` は `、` `。` `「` `」` `・` になります。

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
- フォントデータは `lib/EpdFont/builtinFonts/migu1m_ui_*.h` に埋め込み済み

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
