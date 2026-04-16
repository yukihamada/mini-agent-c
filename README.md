# Evo — 自己進化するローカルAIエージェント

[![Build](https://github.com/yukihamada/mini-agent-c/actions/workflows/build.yml/badge.svg)](https://github.com/yukihamada/mini-agent-c/actions/workflows/build.yml)
[![GitHub](https://img.shields.io/badge/github-mini--agent--c-181717?logo=github)](https://github.com/yukihamada/mini-agent-c)
[![Web UI](https://img.shields.io/badge/web-mini--agent.yukihamada.jp-10b981?logo=safari)](https://mini-agent.yukihamada.jp)
[![C](https://img.shields.io/badge/core-C99-A8B9CC?logo=c)](agent.v12.c)
[![Swift](https://img.shields.io/badge/server-Swift-fa7343?logo=swift)](web/server.swift)
[![iOS](https://img.shields.io/badge/iOS-Evo_App-000?logo=apple)](ios/)
[![MLX](https://img.shields.io/badge/LLM-MLX_Qwen3--blueviolet)](https://github.com/ml-explore/mlx)
[![License](https://img.shields.io/badge/license-MIT-blue)](LICENSE)

**C言語で書かれた、自分自身を改善し続けるAIエージェント。**  
ローカルLLM（Apple Silicon上のMLX）をフル活用し、クラウド依存ゼロで動作。  
Web UI・iOSアプリ・CLI、すべてをひとつのバイナリが束ねる。

---

## なぜ Evo なのか

- **Cバイナリ1本**で動く。Dockerも仮想環境も不要
- **ローカルLLM優先**。Qwen3.5 を MLX で Apple Silicon 上に直接展開
- **自己進化**。エージェントが自分のCソースを読み、改善版を書いてコンパイル・評価する
- **16+ ツール搭載**。ファイル操作からFly.io・GitHub・Gmail・SSH操作まで対応
- **リソース保護付き**。CPU/RAM閾値を超えたら自動で古いエージェントを停止

---

## アーキテクチャ

```
iPhone / Browser
      │ HTTPS
      ▼
Cloudflare Tunnel (mini-agent.yukihamada.jp)
      │
      ▼
Swift HTTPサーバー  :7878          ← web/server.swift
      │ SSE ストリーミング
      ▼
agent バイナリ (C99)               ← agent.v12.c
      │ OpenAI互換 API
      ▼
MLX LLMサーバー    :5001           ← mlx_lm.server (Apple Silicon)
  Qwen3.5-122B / 27B / 9B / 4bit
```

---

## クイックスタート

### 1. ビルド

```bash
git clone https://github.com/yukihamada/mini-agent-c
cd mini-agent-c

# セットアップ（依存チェック + ビルド）
./setup.sh

# または直接 make
make agent.v12 server
```

依存: `cc`（Xcode CLT）、`swiftc`、`curl`、`sqlite3`

### 2. MLX LLMサーバーを起動（Apple Silicon必須）

```bash
pip install mlx-lm

# 軽量 4bit モデル（推奨スタート）
mlx_lm.server --model mlx-community/Qwen3.5-9B-4bit --port 5001

# ハイエンド（M2 Ultra / M3 Max 以上推奨）
mlx_lm.server --model mlx-community/Qwen3.5-122B-4bit --port 5001
```

### 3. Webサーバーを起動

```bash
MINI_AGENT_WEB_TOKEN=your_secret_token \
CPU_REFUSE_PCT=85 CPU_KILL_PCT=92 \
make run
```

ブラウザで `http://localhost:7878` を開く。  
外部公開には [Cloudflare Tunnel](https://developers.cloudflare.com/cloudflare-one/connections/connect-networks/) を推奨。

### 4. CLIとして使う

```bash
# Anthropic API（クラウド）
ANTHROPIC_API_KEY=sk-... ./agent.v12 "Fly.io の全アプリ一覧を出して"

# ローカルMLX（クラウドなし）
./agent.v12 \
  --backend openai \
  --api-base http://127.0.0.1:5001 \
  "ディスク使用量とCPU上位プロセスを確認して"

# 対話モード（REPL）
./agent.v12 -i
```

---

## evo — ローカルLLX専用ラッパー

`evo` はローカルMLXサーバーへの接続を自動化するシェルラッパーです。  
環境変数を毎回指定する必要がなく、MLXサーバーが未起動なら案内してくれます。

```bash
# 基本的な使い方
./evo "Gitのコミット一覧を見せて"

# 対話モード
./evo -i

# MLXサーバーのホスト・ポートをカスタマイズ
MLX_HOST=192.168.0.5 MLX_PORT=5001 ./evo "M5 Macのプロセス一覧"
```

**内部動作:**

```bash
# evo が実行するコマンドのイメージ
./agent.v12 \
  --backend openai \
  --api-base "http://${MLX_HOST}:${MLX_PORT}" \
  "$@"
```

MLXサーバーが見つからない場合:
```
[evo] WARNING: MLX server not responding at http://127.0.0.1:5001
[evo] Start it with: mlx_lm.server --model mlx-community/Qwen3.5-9B-4bit --port 5001
```

---

## 機能一覧

| 機能 | 詳細 |
|------|------|
| **ローカルLLM** | Qwen3.5 122B / 27B / 9B / 4bit via MLX（Apple Silicon） |
| **Web UI** | SSEリアルタイムストリーミング、PWA対応、どこからでもアクセス可 |
| **iOS アプリ** | WKWebViewラッパー「Evo」— App Store更新なしでUI変更可能 |
| **自己進化** | エージェントが自分のCソースを改善→コンパイル→評価 |
| **16+ ツール** | ファイル操作・bash・Fly.io・Telegram・Gmail・GitHub・SSH |
| **リソース保護** | CPU >92% / RAM >90% で古いエージェントを自動停止 |
| **リクエストキュー** | 処理中でも新リクエストを失敗させずにキューイング |
| **自動再起動** | クラッシュ後にサーバーが自動復旧 |
| **フォールバック** | MLX失敗時 → Claude Haiku（Anthropic）へ自動切替 |
| **Bearer認証** | トークン認証 + モバイルセーフエリア対応UI |
| **監査ログ** | 全ツール呼び出しを `.agent/audit.log` にJSONLで記録 |

---

## ツール一覧

### 組み込みツール（Cバイナリ内蔵）

| ツール名 | 説明 |
|---------|------|
| `read_file` | ファイルを読み込む |
| `write_file` | ファイルを作成・上書き |
| `bash` | シェルコマンドを実行 |
| `list_dir` | ディレクトリ一覧 |
| `grep_files` | ファイル内容を正規表現検索 |
| `glob_files` | パターンでファイルを検索 |
| `diff_files` | ファイル差分を表示 |
| `diff_apply` | unified diffをファイルに適用 |
| `http_request` | HTTP GET/POST/PUT/DELETE |
| `save_memory` | メモを `~/.mini-agent/memory.md` に保存 |
| `recall_memory` | 保存メモを読み込む |
| `spawn_agent` | 子エージェントを生成（深さ制限付き） |
| `checkpoint` | 変更ファイルのスナップショット保存 |
| `undo` | 最新チェックポイントから復元 |
| `clipboard_get` | macOSクリップボードを読む |
| `clipboard_set` | macOSクリップボードに書き込む |
| `preview_edit` | diffプレビュー（ファイル変更なし） |
| `run_tests` | テストフレームワークを自動検出・実行 |
| `sleep` | N秒待機（ポーリング・リトライに便利） |

### 動的ツール（`.agent/tools/*.sh`）

| ツール名 | 説明 |
|---------|------|
| `fly_ops` | Fly.ioアプリのデプロイ・ログ・secrets操作 |
| `telegram_send` | Telegramへメッセージ送信 |
| `gmail_ops` | Gmail検索・送受信（gog CLI経由） |
| `github_ops` | PR・issue・CI管理（gh CLI経由） |
| `m5_exec` | M5 MacへのSSHコマンド実行 |
| `self_improve` | 進化状況・eval履歴の確認 |
| `power_info` | macOSバッテリー・電源情報 |
| `http_fetch` | HTTP GET/POSTリクエスト |

`.agent/tools/` に `.sh` ファイルを置くだけで新ツールを追加できます。

---

## 自己進化の仕組み

エージェントは自分自身のCソースコードを読んで改善案を書き、コンパイル・評価します。

```
POST /evolve
      │
      ▼
agent が agent.vN.c を読む
      │
      ▼
改善点を特定・実装 → agent.v(N+1).c を書き出す
      │
      ▼
cc -O2 agent.v(N+1).c cJSON.c -lcurl -lm
      │
      ▼
eval.sh でスコアリング
      │
      ▼
.agent/eval_history.jsonl に記録
```

これまでの進化ログ:

| バージョン | 主な追加機能 |
|-----------|------------|
| v1 | 基本ツールループ |
| v6 | 動的ツール、spawn_agent、永続メモリ |
| v8 | grep_files、glob_files、ストリーミングbash |
| v9 | 並列ツール実行、git、diff_files、通知 |
| v10 | HTTPリクエスト、checkpoint/undo、クリップボード |
| v11 | 自己進化、リソース保護、MLXサポート |
| v12 | diff_apply、sleep ツール、進化コンテキスト改善 |

---

## プロジェクト構成

```
mini-agent-c/
├── agent.v12.c          # 現行エージェントソース（C99）
├── agent.v{N}.c         # 過去バージョンの履歴
├── cJSON.c / cJSON.h    # JSONライブラリ
├── eval.sh              # エージェント評価スクリプト
├── evolve.sh            # 進化トリガースクリプト
├── evo                  # ローカルMLXラッパー（推奨CLI）
├── setup.sh             # セットアップスクリプト
├── Makefile             # ビルドルール
├── web/
│   ├── server.swift     # Swift HTTPサーバー（メイン）
│   ├── index.html       # Web UI（PWA）
│   └── server.py        # Pythonサーバー（レガシー）
├── ios/
│   ├── Sources/App/     # SwiftUI WKWebViewアプリ
│   └── project.yml      # XcodeGen仕様
├── examples/            # ユースケースサンプル集
│   ├── 1-openclaw-fleet/
│   ├── 2-solana-watchdog/
│   ├── 3-fly-deploy-verify/
│   ├── 4-rust-subprocess/
│   └── 5-evolve-any-target/
└── .agent/
    ├── tools/           # 動的シェルツール
    ├── audit.log        # ツール呼び出し監査ログ
    ├── web_history.jsonl
    └── eval_history.jsonl
```

---

## 環境変数

| 変数名 | デフォルト | 説明 |
|--------|----------|------|
| `MINI_AGENT_WEB_TOKEN` | `` | Bearer認証トークン |
| `PORT` | `7878` | HTTPサーバーポート |
| `MLX_HOST` | `127.0.0.1` | MLXサーバーホスト |
| `MLX_PORT` | `5001` | MLXサーバーポート |
| `MAX_CONCURRENT` | `2` | 最大並列エージェント数 |
| `CPU_REFUSE_PCT` | `85` | この%超でリクエスト拒否 |
| `CPU_KILL_PCT` | `92` | この%超で最古エージェントを停止 |
| `MEM_KILL_PCT` | `90` | この%超で最古エージェントを停止 |
| `AUTO_EVOLVE_HOURS` | `0` | N時間ごとに自動進化（0=無効） |
| `ANTHROPIC_API_KEY` | `` | Claude APIキー（クラウドフォールバック用） |

---

## iOSアプリ（Evo）

`ios/` — Web UIをWKWebViewでラップしたネイティブアプリ。

```
Bundle ID:   com.enablerdao.evo
アプリ名:     Evo
最低バージョン: iOS 17.0+
```

認証トークンはJavaScript経由でインジェクションされるため、  
Web UI側を変更してもApp Storeへの再提出不要。

```bash
cd ios
xcodegen generate
open Evo.xcodeproj
```

---

## セキュリティ

- Bearer tokenによるリクエスト認証
- パストラバーサル防止（`..` / `~` 禁止、CWD内に限定）
- 危険なbashコマンドのデニーリスト
- `spawn_agent` の再帰深さ上限（`MINI_AGENT_DEPTH`）
- `.agent/STOP` ファイルで全エージェントを即時停止
- 全ツール呼び出しを `.agent/audit.log` にJSONL記録
- APIキーは全出力から自動マスキング

詳細は [SECURITY.md](SECURITY.md) を参照。

---

## コントリビューション

[CONTRIBUTING.md](CONTRIBUTING.md) を参照。  
新ツールの追加は `.agent/tools/` に `.sh` ファイルを置くだけです。

```bash
# 新ツールの例: .agent/tools/my_tool.sh
#!/bin/bash
# TOOL_DESCRIPTION: 何かを実行するツール
# TOOL_ARGS: {"action": "実行するアクション"}
echo "実行結果"
```

---

## ライセンス

[MIT](LICENSE) — built by [yukihamada](https://github.com/yukihamada)
