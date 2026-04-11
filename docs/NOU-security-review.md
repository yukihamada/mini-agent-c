# NOU × mini-agent-c — セキュリティレビュー

実測日: 2026-04-11
対象: NOU v2.5.0 (このMBAで稼働), mini-agent-c v5
レビュアー: Claude (Opus 4.6) via mini-agent-c自体のprobing

## TL;DR

| 項目 | 状態 | 緊急度 |
|---|---|---|
| nou-server `:4004` が `*:4004` バインド (LAN公開) | 認証なし | 🟠 中 |
| NOU.app `:4001` が `*:4001` バインド (admin API LAN公開) | 認証なし | 🔴 **高** |
| `:4001 /api/settings` LAN から読める (nodeID / relay config / wallet field 漏洩) | active | 🟠 中 |
| `:4001 /api/cloud/runpod/pods` LAN から RunPod状態読める | active | 🟠 中 |
| `:4004 /v1/chat/completions` LAN から無料で推論使える (resource drain) | active | 🟠 中 |
| `:4001 /v1/chat/completions` は現在500で壊れてる — ただし latent risk: もし直れば Anthropic API key 経由で claude proxying できる可能性 | latent | 🟡 低 (要監視) |
| llama-server `:5021` は `127.0.0.1` バインド | 安全 | ✅ |
| mini-agent-c の bash tool が LAN経由で使える可能性 (NOU routing 次第) | 未確認 | 🔴 **高** (要調査) |

## 実測の証拠

### 1. バインドアドレス
```
$ netstat -an | grep -E '\*\.(4001|4004|5021)'
tcp4  *.4004  *.*  LISTEN       ← nou-server, any interface
tcp4  *.4001  *.*  LISTEN       ← NOU.app, any interface
                                  (5021 は localhost only, OK)
```

### 2. LAN から無認証で到達
LAN IP `192.168.0.194` (このMBA) に他デバイスからアクセス可能:
```
$ curl http://192.168.0.194:4004/v1/models    → 200 OK (モデル一覧)
$ curl http://192.168.0.194:4004/v1/chat/completions -d '{"model":"qwen2.5:0.5b",...}'
  → 200 OK, 推論実行、ollama がCPU燃やす
$ curl http://192.168.0.194:4001/api/settings → 200 OK
  → nodeID, relay config, wallet field 漏洩
$ curl http://192.168.0.194:4001/api/cloud/runpod/pods → 200 OK
  → RunPod デプロイ状態漏洩
$ curl http://192.168.0.194:4001/api/cloud/lambda/instances → 200 OK
$ curl http://192.168.0.194:4001/api/metrics → 200 OK
```

### 3. active な LAN 接続
```
$ netstat -anv -f inet | grep 192.168
192.168.0.194.4001  192.168.0.47.63424  ESTABLISHED
```
`192.168.0.47` から NOU.app :4001 に active 接続あり。mesh mate と想定されるが、LAN 内の誰でも同じことができる状態。

## 脅威モデル

### Adversary class
1. **同居人 / 来客** (自宅 WiFi にログイン可能)
2. **友達 (mesh mate)** — 信頼するけど 100% は置かない (端末が紛失 / マルウェア / 意図せぬアクセス)
3. **近隣** (router 弱い設定 / guest WiFi 開放)
4. **LAN 内 pwn'd device** (IoT / Mac 内マルウェア / ブラウザ経由の DNS rebinding 攻撃)

### 刺さる攻撃

**A. 無料 LLM リソース利用 (medium)**
- LAN の任意デバイスが `:4004` で好きなだけ推論できる
- **影響**: ollama + CPU/GPU 使用率 100%、バッテリー消耗、他の作業が詰まる、電気代
- **実測**: 今の MBA (16GB, load avg 72) で既に限界、LAN 攻撃されたら完全に詰む

**B. 設定・メタデータ漏洩 (medium)**
- `:4001/api/settings` で nodeID, relay config, installed models, wallet field
- **影響**: 個別追跡、friends list / relay endpoint の把握、ユーザー判別

**C. RunPod / Lambda 状態漏洩 (medium)**
- `:4001/api/cloud/runpod/pods` で自分が持ってる GPU pod のID, 状態, サイズ
- **影響**: コスト状況の推測、外部 GPU attack vector の把握

**D. Latent: Anthropic API key proxying (high if activated)**
- `:4001 /v1/chat/completions` は現在 500 で壊れてる
- もし直った瞬間、LAN から `model=claude-opus-4-6` リクエストで Anthropic API key 経由の請求発生
- **影響**: 月$100〜1000レベル abuse の可能性
- **対策**: 直すときに auth を先に入れる、または localhost bind にする

**E. mini-agent-c bash tool 経由の任意コード実行 (high, 未確認)**
- NOU に mini-agent-c を plug した場合、NOU :4004 経由で mini-agent-c を呼び出せるかは未検証
- **もし呼べるなら**: LAN の誰でも `bash` ツール経由で任意コマンド実行 → RCE
- **即対策**: mini-agent-c を NOU 配下で動かすなら **必ず `--sandbox` + `--max-turns` 低め + `--budget` 低め**

## 推奨対策 (優先順)

### 🔴 即時 (今日やるべき)

**1. `:4001` を `127.0.0.1` バインドに変更**
   - 理由: admin 相当の endpoint (`/api/settings`, `/api/cloud/*`) を持ってる
   - mesh は relay (nou.run) 経由で既に動くので LAN bind 不要
   - NOU 本体 (`local-multi-agent/` 内) の変更が必要 — 別セッションが編集中なので **そちらに投げる**
   
   提案 diff (未適用、別セッションに渡す用):
   ```rust
   // before
   let addr = format!("0.0.0.0:{}", port);
   // after
   let addr = format!("127.0.0.1:{}", port);
   // mesh は relay client が outbound で nou.run に繋ぐので LAN listen 不要
   ```

**2. `:4004` に Bearer token 必須化**
   - `~/.nou/config.toml` に `api_token = "<ランダム>"` を追加
   - nou-server は起動時にロード、`authorization: Bearer <token>` ヘッダを検証
   - mini-agent-c 側: `OPENAI_API_KEY=<token>` で渡す (既に対応済み)
   - 別セッションに実装依頼

### 🟠 今週 (やるべき)

**3. Rate limit on `:4004`**
   - 単純版: IP あたり 10 req/min
   - 目的: bursty abuse の緩和

**4. mini-agent-c のデフォルト hardening**
   - `--backend openai --api-base http://192.168.*` を指定したら警告 + 明示的 `--allow-remote-backend` 必須
   - NOU backend 使うときは `--sandbox` をデフォルト ON に
   - 実装: agent.v5.c に 10 行追加

**5. 監査ログ**
   - nou-server に access log (IP, timestamp, endpoint, status)
   - `/Applications/NOU.app/Contents/Resources/access.log` に
   - 別セッションに提案

### 🟡 長期 (すべき)

**6. Relay 経由 mutual TLS**
   - nou.run relay で端末証明書、mesh mate だけが接続可能
   - NOU 既に実装中?

**7. `:4001` admin endpoint に CSRF 保護**
   - Origin check, or double-submit token
   - ブラウザからアクセスする dashboard なので CSRF 重要

**8. mini-agent-c の bash tool に explicit allow-list**
   - デフォ deny、設定で `allowed_commands = [...]` を通ったものだけ
   - 現在は deny list 方式 (dangerous patterns block) だが LAN exposure 下では不十分

## 今日すぐできる暫定対策

NOU 本体を触らずに安全度を上げる3つ:

```bash
# 1. macOS の Application Firewall で NOU のネットワーク到達を localhost のみに
#    (ただし macOS ALF は強くない、pfctl の方が確実)

# 2. pfctl で :4001 / :4004 を外部遮断 (要 sudo)
sudo pfctl -ef /dev/stdin <<'RULES'
block in quick on en0 proto tcp from any to any port { 4001 4004 }
pass in quick on en0 proto tcp from 127.0.0.1 to any port { 4001 4004 }
RULES
#    → LAN から :4001/:4004 見えなくなる、mesh 機能は relay (outbound) なので生きる

# 3. mini-agent-c を NOU backend で使うときは必ず --sandbox 付ける
#    (既にv5でflag有り)
./agent.v5 --backend openai --sandbox --api-base http://localhost:4004 ...
```

## 残項目 (要追加調査)

- [ ] NOU の mesh 機能が LAN bind に依存してるか、relay 経由だけで動くか確認
- [ ] `:4001 /api/cloud/keys` が 404 なのは削除済みか、method違いか確認
- [ ] mini-agent-c を NOU plugin として登録可能か (dynamic tools 経由 RCE リスク)
- [ ] nou-server の auth 実装状況 (`--auth-token` flag 存在するか source 読む)

## Security を守ったままテストしたいとき

pfctl ルール入れた状態で動作確認:

```bash
# pfctl で :4004 を closed にしてから localhost 経由のみで v5 テスト
./agent.v5 --backend openai --sandbox --api-base http://127.0.0.1:4004 \
    --max-turns 5 --budget 20000 --no-memory "your task"
# LAN exposure なしで機能動作確認
```
