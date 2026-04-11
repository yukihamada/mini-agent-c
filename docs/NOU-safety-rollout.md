# NOU Safety Rollout — 実行ログ

日付: 2026-04-11
対応者: Claude (via mini-agent-c)
前提: ユーザー指示「コストよりも安全優先で全部やって」

## 実施サマリ

| # | 作業 | 結果 |
|---|---|---|
| 1 | バックアップ取得 (`/tmp/nou-safety-backup/`) | ✅ |
| 2 | `nou-server/src/main.rs` bind → `127.0.0.1` (env: `NOU_BIND_ALL=1` でopt-in LAN) | ✅ |
| 3 | `NOU.app-src/Sources/Server/ProxyServer.swift` bind → `127.0.0.1` (同上) | ✅ |
| 4 | `UpdateChecker.swift` 署名検証 `codesign --verify -R='Developer ID Application: Yuki Hamada (5BV85JW8US)'` | ✅ |
| 5 | UpdateChecker `.app` bundle 全体置換 + backup + xattr clear | ✅ |
| 6 | UpdateChecker post-update health check (30秒内crashで自動rollback) | ✅ |
| 7 | `AppDelegate.swift` 起動時rollback + 起動60秒後 + 24h周期 update check | ✅ |
| 8 | `cargo build --release` (nou-server, 3分42秒) | ✅ |
| 9 | `launchctl unload` → 新 binary 設置 → `launchctl load` | ✅ |
| 10 | nou-server `:4004` が `127.0.0.1:4004` にbindすること検証 | ✅ |
| 11 | LAN from `192.168.0.194:4004` → `Couldn't connect` 確認 | ✅ |
| 12 | `swift build -c release` (NOU.app) | ⏳ 進行中 (UpdateChecker型エラー1件修正後) |
| 13 | NOU.app 再起動 + `:4001` localhost bind 検証 | 保留 |
| 14 | `verify.sh` 全項目 pass | 保留 |

## 変更ファイル一覧

`local-multi-agent/` 内:
- `nou-server/src/main.rs` (1 hunk, bind ip + ログ)
- `NOU.app-src/Sources/Server/ProxyServer.swift` (1 hunk, bindHost)
- `NOU.app-src/Sources/App/UpdateChecker.swift` (大幅追加: 署名検証 + bundle 置換 + rollback)
- `NOU.app-src/Sources/App/AppDelegate.swift` (2 hunks: 起動時 rollback check, 24h timer)

Backups at: `/tmp/nou-safety-backup/`
Rollback: `bash /tmp/nou-safety-backup/ROLLBACK.sh`

## 別セッションへの引き継ぎ

別セッションが `local-multi-agent/` を編集している場合、以下に注意:

1. 私の変更は **既存の unstaged changes を base に追加** したもの。git で衝突しにくい小さい hunks。
2. `UpdateChecker.swift` は大幅変更あり(~100行追加)。新しいメソッド: `verifySignature`, `runPostUpdateHealthCheckIfNeeded`, `rollbackToPrevious`, `findAppBundle`
3. `AppDelegate.swift` に2箇所追加:
   - `applicationDidFinishLaunching` の先頭: `UpdateChecker.runPostUpdateHealthCheckIfNeeded()`
   - 同関数の末尾: 起動60秒後 + 24h timer の update check

## relay / mesh への影響

**確認済**: nou-server を localhost bind にしても `https://nou.run` への outbound 接続 (relay) は維持。friend mesh は relay 経由で機能継続:

```
✓ Relay connected! Public URL: https://nou.run/n/45ca9328-7ed1-4401-9a41-7f0163080fe2
```

LAN Bonjour discovery のみ機能停止 (同一 WiFi の iPhone から `192.168.0.x:4004` で直接繋ぐ用途)。
必要なら `NOU_BIND_ALL=1` 環境変数で復帰可能だが、その場合は Bearer token auth の実装が前提。

## 残タスク

- [ ] NOU.app rebuild 完了待ち (ongoing)
- [ ] NOU.app 再起動 + `:4001` localhost bind 確認
- [ ] 全項目 `/tmp/nou-safety-backup/verify.sh` 実行
- [ ] 再起動後 relay 再接続 + friend mesh 正常動作確認
- [ ] git commit (細かい hunks 毎、blame しやすく)
