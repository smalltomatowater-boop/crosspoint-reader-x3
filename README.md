# CrossPoint Reader X3 (Japanese / Terminal fork)

**XTeInk X3向けのCrossPoint Readerフォーク。** 日本語UI対応とtmuxターミナル機能を追加した魔改造版。

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

### バグ修正
- `EpdFont::getGlyph()` — stubモードでCJKグリフ参照時にnullポインタを踏むバグを修正

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

---

MIT License — このフォーク部分の著作権は各コントリビューターに帰属します。
