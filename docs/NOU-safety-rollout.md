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

## 最終結果 (2026-04-11 19:04)

```
$ bash /tmp/nou-safety-backup/verify.sh
=== nou-server :4004 ===
  bind = 127.0.0.1:4004      ✅ localhost only
  LAN probe = 000 (Couldn't connect)  ✅ LAN closed

=== NOU.app :4001 ===
  bind = 127.0.0.1:4001      ✅ localhost only
  LAN probe = 000 (Couldn't connect)  ✅ LAN closed

=== localhost still works ===
  http://localhost:4004/health = 200  ✅
  http://localhost:4001/api/settings = 200  ✅

🎉 ALL LAN EXPOSURE CLOSED — safety hardening verified
```

### Relay (friend mesh) 動作確認

```
$ curl https://nou.run/n/45ca9328-7ed1-4401-9a41-7f0163080fe2/health
{"service":"nou-server","status":"ok","version":"0.2.0"}
```

friend/iPhone からは **relay 経由** で到達可能。LAN 直結はできなくなったが、
mesh 機能は破壊ゼロ。

### 生成された成果物

`/tmp/nou-safety-backup/` 内:
- `ROLLBACK.sh` — 3ファイル一発 revert + 再build 案内
- `verify.sh` — LAN閉じてること確認 (再利用可)
- `apply-pfctl.sh` — 補助的PF firewall (要sudo、optional)
- `block-lan.pf` — PF rule file
- `nou-server-bind.patch` — nou-server/src/main.rs の unified diff (23行)
- `ProxyServer-bind.patch` — ProxyServer.swift の unified diff (23行)
- `UpdateChecker-hardening.patch` — UpdateChecker.swift 大幅 hunks (262行)
- `AppDelegate.swift.new` — 私が編集した全体 (別セッションと merge 用)
- `*.bak` — 元ファイルのコピー

### 別セッションへの引き継ぎ

私が編集した4ファイル全部 **git unstaged** のまま残してあります:
```
NOU.app-src/Sources/App/AppDelegate.swift   (+25 lines)
NOU.app-src/Sources/App/UpdateChecker.swift (+~150 lines)
NOU.app-src/Sources/Server/ProxyServer.swift (+7 lines)
nou-server/src/main.rs                       (+10 lines)
```

別セッションが作業再開したら:
1. `git diff` で私の変更を確認
2. 自分の変更とconflict ないか見る
3. commit するか、patch を revert して自分の変更をrebaseするか判断

私のpatchは全部 `/tmp/nou-safety-backup/*.patch` として独立保存済なので、
cherry-pick で別ブランチに移すのも可能です。

### Phase 2: Bearer Token Auth + Menubar UI (2026-04-11 19:20)

**追加実装**:

| # | 内容 | 結果 |
|---|---|---|
| 15 | nou-server (Rust) に bearer token middleware 追加 | ✅ |
| 16 | `load_or_create_auth_token()` — `~/.nou/auth_token` 自動生成 (0600, 48 hex chars) | ✅ |
| 17 | `auth_middleware` — localhost bypass + /health public + Bearer token 検証 | ✅ |
| 18 | `ConnectInfo<SocketAddr>` 経由で peer IP 取得 (spoof 不可) | ✅ |
| 19 | NOU.app (Swift) `AuthMiddleware` 新規作成 — 既存 AuthCheck と連動 | ✅ |
| 20 | ProxyServer.swift に `router.add(middleware: AuthMiddleware())` | ✅ |
| 21 | MenubarController.swift `updateIconBadge()` — 赤丸 badge + tooltip | ✅ |
| 22 | `@objc func installUpdate()` — 確認 dialog → `UpdateChecker.downloadAndInstall()` 直接実行 (署名検証込み) | ✅ |
| 23 | 両バイナリ再ビルド (`cargo build --release` + `swift build -c release`) | ✅ |
| 24 | Launch daemon reload + NOU.app 再起動 | ✅ |
| 25 | `verify.sh` 拡張: bearer token 5項目自動テスト | ✅ |

**ベンチ結果** (一時的 `NOU_BIND_ALL=1` で 4006 検証):
```
token file: exists (48 chars, 0600)
permissions: 0600 ✅
LAN no-token    = 401 ✅
LAN wrong-token = 401 ✅
LAN correct     = 200 ✅
LAN /health     = 200 ✅
```

**最終状態**:
- nou-server :4004 は localhost bind 維持 (NOU_BIND_ALL=1 で opt-in LAN)
- NOU.app :4001 は localhost bind 維持 (同上)
- LAN bind を有効化しても Bearer token 必須 → 不正アクセス 401
- mesh/friend は relay.nou.run 経由で引き続き動作
- Menubar に赤丸 badge → update 検知時自動表示
- Update install は menubar メニュー → 確認 → 署名検証 → .app bundle 置換 → rollback フラグ → 30秒 crash 自動復旧

**新規ファイル**:
- `NOU.app-src/Sources/Server/AuthMiddleware.swift` (新規、80行)

**変更ファイル** (全て unstaged):
- `nou-server/src/main.rs` (+134行: token 生成、middleware、ConnectInfo 配線)
- `NOU.app-src/Sources/Server/ProxyServer.swift` (+4行: middleware 追加)
- `NOU.app-src/Sources/Menubar/MenubarController.swift` (+70行: badge + installUpdate)
- `NOU.app-src/Sources/App/UpdateChecker.swift` (Phase 1 から変更なし)
- `NOU.app-src/Sources/App/AppDelegate.swift` (Phase 1 から変更なし)

**patches 更新済** (`/tmp/nou-safety-backup/`):
- `nou-server-full.patch` (152行、bind + auth 両方)
- `ProxyServer-full.patch` (34行、bind + middleware 両方)
- `AuthMiddleware.swift.new` (新規ファイル全文)
- `MenubarController.swift.new` (全体、3MB 近いので cherry-pick 推奨)

### ✅ 完了したタスク

- [x] nou-server `:4004` localhost bind (source + rebuild + install + verify)
- [x] NOU.app `:4001` localhost bind (source + swift build + install + verify)
- [x] UpdateChecker 署名検証 (codesign --verify Developer ID)
- [x] UpdateChecker .app bundle 全体置換 + xattr 除去
- [x] UpdateChecker post-update health check + 自動 rollback
- [x] AppDelegate 起動時 rollback check + 24h auto-update check
- [x] verify.sh で LAN 閉じ確認
- [x] Relay (nou.run) mesh 動作確認
- [x] Backups + patches + rollback script 保存
- [x] 全プロセス再起動、locally working, LAN blocked

### 推奨される次の対応

(1) 別セッションが復帰したら、`git diff` で私の変更を確認してもらう
(2) Auto-update UI (menubar badge / banner) は **まだ実装してない** —
    別セッションに `docs/NOU-auto-update-design.md` の A2 (menubar UI) を依頼
(3) `:4004` の Bearer token auth を追加すれば `NOU_BIND_ALL=1` で
    LAN 再開しても安全 (今は強制 localhost のみ)
(4) friend の iPhone が LAN で繋ぎに来る既存フローが動かなくなった場合は、
    iPhone 側も relay 経由に切り替え必要
