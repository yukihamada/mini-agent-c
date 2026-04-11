# NOU — Auto-update / Update Notification 設計書

作成日: 2026-04-11
対象: NOU v2.5.0 (macOS + iOS + Windows)
前提: `local-multi-agent/` は **別セッションが編集中 — 本ドキュメントはspecのみ、本体未編集**

## 現状監査 (実装済の部分)

`NOU.app-src/Sources/App/UpdateChecker.swift` を読み取り確認:

| 機能 | 実装状況 | 備考 |
|---|---|---|
| GitHub Releases API poll (`api.github.com/repos/yukihamada/NOU/releases/latest`) | ✅ あり | `checkForUpdates()` |
| セマンティックバージョン比較 | ✅ あり | `isNewer(a, b)` |
| asset download (streaming + progress) | ✅ あり | `downloadAndInstall()` |
| zip 展開 (`/usr/bin/unzip`) | ✅ あり | |
| バイナリ置換 + 再起動 (シェルスクリプト経由) | ✅ あり | `cp -f ... && open /Applications/NOU.app` |
| エラーハンドリング | ⚠️ print のみ | ユーザーに通知なし |
| **定期チェック (タイマー)** | ❌ なし | 起動時 or 手動のみ? 呼び出し元未確認 |
| **通知 UI (menubar badge / dialog)** | ❌ 不明 | SettingsHandler / AppDelegate 要確認 |
| **署名検証** | ❌ なし | ⚠️ **リスク**: GitHub release 差し替え攻撃で任意コード実行 |
| **"Skip this version"** | ❌ なし | |
| **Auto vs Notify-only 設定** | ❌ なし | |
| **Rollback on crash** | ❌ なし | 新 binary が crash loop すると復旧不能 |
| **Friend mesh 通知** | ❌ なし | relay 経由で peer に "新バージョンあるよ" broadcast |
| **.app bundle (vs binary) 置換** | ⚠️ 怪しい | `Bundle.main.executablePath` に `cp` → 単一バイナリ前提、`.app/Contents/MacOS/NOU` に cp しても Info.plist や Resources は更新されない |
| **Windows auto-update** | ❌ なし | `nou-windows/` 別経路必要 |
| **iOS auto-update** | ✅ (TestFlight) | App Store 経由なので自動 |

## 設計目標

1. **ユーザー (＝あなた＋現2名＋今後増えるfriends) が最新NOUを簡単に得られる**
2. **signed & verified** で差し替え攻撃を防ぐ
3. **不在中に壊れない** — rollback 保険
4. **mesh 連動** — 誰か (特にあなた) が新しい版リリースしたら friends が即気付く
5. **非 intrusive** — 「強制再起動」はしない、作業中は邪魔しない

## 提案アーキテクチャ

### フェーズ A: macOS NOU.app (優先)

#### A1. 定期チェック
```swift
// AppDelegate.swift applicationDidFinishLaunching に追加
// 起動時即チェック + 以降24h間隔
Task { @MainActor in
    await UpdateChecker.shared.checkForUpdates()
    if UpdateChecker.shared.isUpdateAvailable { showUpdateBanner() }
    Timer.scheduledTimer(withTimeInterval: 24 * 3600, repeats: true) { _ in
        Task { @MainActor in
            await UpdateChecker.shared.checkForUpdates()
            if UpdateChecker.shared.isUpdateAvailable { showUpdateBanner() }
        }
    }
}
```

#### A2. 通知 UI
3 段階 notification:

1. **Menubar badge** (passive)
   - NOU menu item に `•` 赤ドット or `↑` 矢印
   - 右クリックメニューに "Update to v2.6.0 →"

2. **Drop-down banner** (active, 邪魔しない)
   - menubar popover 開いたときの上部にバナー
   - "✨ v2.6.0 available [View] [Update now] [Later]"

3. **macOS notification** (escalation)
   - 3回banner無視されたら OS通知
   - `UNUserNotificationCenter` 経由
   - 24h 後に再度

#### A3. ユーザー設定
`SettingsHandler.swift` (既存) に追加:
```swift
// ~/.nou/config.toml または UserDefaults
"update": {
    "policy": "notify_only",     // "notify_only" | "auto_on_restart" | "manual"
    "channel": "stable",          // "stable" | "beta" | "nightly"
    "last_check": "2026-04-11T07:00:00Z",
    "skipped_versions": ["2.5.1"],
    "check_interval_hours": 24
}
```

UI: NOU settings pane に update tab 追加
- [x] Check for updates automatically
- Channel: [Stable ▾]
- [x] Notify me on menubar
- [ ] Auto-install on next restart
- [Check now] [View release notes]

#### A4. 署名検証 (⚠️ 必須 — 現状の穴)
ダウンロードした新 binary が自分の鍵で署名されてることを `codesign` で検証:

```swift
// download 完了後、置換 BEFORE
private func verifySignature(at path: String) -> Bool {
    let task = Process()
    task.executableURL = URL(fileURLWithPath: "/usr/bin/codesign")
    task.arguments = [
        "--verify",
        "--deep",
        "--strict",
        "--requirement", "anchor apple generic and certificate leaf[subject.CN] = \"Developer ID Application: Yuki Hamada (5BV85JW8US)\"",
        path
    ]
    let pipe = Pipe()
    task.standardError = pipe
    try? task.run()
    task.waitUntilExit()
    return task.terminationStatus == 0
}

// 呼び出し側:
guard verifySignature(at: binaryURL.path) else {
    print("[UpdateChecker] signature verification FAILED — refusing")
    NSAlert.showError("Update signature invalid")
    return
}
```

署名検証失敗時は install せず警告を出す。**これがないと GitHub account が pwn された瞬間ユーザー全員に malware pushできる。**

#### A5. Rollback 保険
```swift
// 置換 BEFORE: 現在の binary を backup
let backupDir = URL(fileURLWithPath: "/Applications/NOU.app/Contents/MacOS/.backup")
try? FileManager.default.createDirectory(at: backupDir, withIntermediateDirectories: true)
try? FileManager.default.copyItem(
    atPath: targetPath,
    toPath: backupDir.appendingPathComponent("NOU.v\(currentVersion)").path)

// 新版 install 後、次回起動時にヘルスチェック
// AppDelegate.applicationDidFinishLaunching:
if UserDefaults.standard.bool(forKey: "justUpdated") {
    UserDefaults.standard.set(false, forKey: "justUpdated")
    let launchTime = Date()
    // 30秒以内にクラッシュしたら rollback
    DispatchQueue.main.asyncAfter(deadline: .now() + 30) {
        UserDefaults.standard.set(true, forKey: "postUpdateHealthy")
    }
}
// 次々回起動時、前回 postUpdateHealthy==false なら rollback
```

#### A6. .app bundle 全体更新 (現状の罠の修正)
現在の UpdateChecker は `Bundle.main.executablePath` (Mach-O binary) に `cp` してるけど、Info.plist や Resources が更新されない → バージョン文字列と実装が不一致になる。

正しい流れ: **.app バンドル全体を置換**
```swift
// DMG/zip の中は "NOU.app" (bundle) として来る想定
// 1. /Applications/NOU.app を /Applications/NOU.app.old にrename
// 2. 新 NOU.app を /Applications/NOU.app に move
// 3. 再起動
// 4. 次回起動時 .old を削除
```

### フェーズ B: Friend mesh broadcast

あなたが新 release を push すると、relay (nou.run) 経由で peer に "新版あり" を伝える:

#### B1. Relay API 追加 (nou-relay 側)
```
POST /mesh/announce
  {"type": "version_update", "version": "2.6.0", "min_upgrade_from": "2.5.0"}
  → 全 connected peer に push
```

#### B2. Client 側 (NOU.app)
relay から `version_update` event を受信 → UpdateChecker を即発火

```swift
func onRelayEvent(_ ev: RelayEvent) {
    if ev.type == "version_update", let v = ev.version,
       UpdateChecker.shared.isNewer(v, than: UpdateChecker.shared.currentVersion) {
        Task { await UpdateChecker.shared.checkForUpdates() }
    }
}
```

効果: 普通は `UpdateChecker.shared` が 24h 間隔で polling するので最大24h遅延。relay push があれば **数秒** で全 friends に届く。

#### B3. 管理用コマンド
あなたが新版出したら手動 broadcast:
```bash
curl -X POST https://nou.run/mesh/announce \
  -H "authorization: Bearer $NOU_ADMIN_TOKEN" \
  -d '{"version":"2.6.0","notes":"bugfix release","severity":"recommended"}'
```

GitHub Actions で release published イベント → 自動実行:
```yaml
# .github/workflows/release.yml に追加
- name: Notify NOU mesh
  run: |
    curl -X POST https://nou.run/mesh/announce \
      -H "authorization: Bearer ${{ secrets.NOU_ADMIN_TOKEN }}" \
      -d "{\"version\":\"${{ github.event.release.tag_name }}\",\"notes\":\"${{ github.event.release.name }}\"}"
```

### フェーズ C: Windows / Linux

#### C1. nou-windows auto-update
現在: ユーザーが `NOU-Setup-Windows.exe` を再ダウンロードする想定
追加: nou-windows CLI (Rust) に同様の UpdateChecker を実装
- GitHub Releases poll
- squirrel.windows 形式の差分 patch (やれば)
- 簡易版: "new version! [link]" を stderr/GUI に表示

#### C2. Linux (`nou.link/install.sh` 経由)
既存 install script を再実行する案内のみ
`nou update` CLI コマンドで `curl -sSL nou.link/install.sh | bash` を再実行

## 実装優先順位

| # | 内容 | 所要 | 誰が |
|---|---|---|---|
| 🔴 1 | 署名検証追加 (A4) | 1h | 別セッション |
| 🔴 2 | .app bundle 全体置換 (A6) | 2h | 別セッション |
| 🟠 3 | 起動時 + 24h 定期チェック (A1) | 30min | 別セッション |
| 🟠 4 | menubar badge + banner UI (A2) | 2h | 別セッション |
| 🟠 5 | rollback 保険 (A5) | 1h | 別セッション |
| 🟡 6 | 設定 UI + ユーザー policy (A3) | 2h | 別セッション |
| 🟡 7 | relay broadcast API (B1-B3) | 4h | relay + NOU.app 両方 |
| 🟢 8 | Windows auto-update (C1) | 4h | nou-windows |

**1 と 2** が最優先 — 現状の実装はセキュリティホール + バージョン不整合リスクがある。

## mini-agent-c からの貢献

別セッションが忙しい場合、mini-agent-c で以下を手伝える:

1. **Release health monitoring** (既存 ExampleFly-deploy-verify と同じパターン)
   - GitHub release published 後、GitHub Actions から mini-agent-c 呼び出し
   - `curl nou.run/api/version_probe` で各 relay-connected peer の更新状況報告
   - 24h 後、更新してない peer に再通知

2. **Changelog auto-generation**
   - `git log v2.5.0..HEAD --pretty='%s'` → mini-agent-c に投げて日本語 release note 生成

3. **Friend discovery**
   - relay 側で "mesh に参加してる peer 一覧" を周期的に Claude に投げて、更新通知送るべき peer をリストアップ

これらは全部 mini-agent-c の dynamic tools (`.agent/tools/nou_*.sh`) で実装できて、本体 (local-multi-agent/) に触らない。

## リリース運用フロー (提案)

あなたが新版出す流れ:

```bash
# 1. tag + push (既存)
cd ~/workspace/local-multi-agent
# 作業して commit ...
git tag v2.6.0
git push origin v2.6.0

# 2. GitHub Actions が:
#    - build (macOS / Windows)
#    - codesign (Developer ID Application)
#    - notarize (Apple notary)
#    - create GitHub release + upload assets
#    - nou-relay に broadcast (POST /mesh/announce)

# 3. mini-agent-c で release verify (optional)
./mini-agent/examples/3-fly-deploy-verify/verify.sh nou-release \
    https://github.com/yukihamada/NOU/releases/latest \
    https://nou.run/health

# 4. 24h 後、未更新 peer 数 を mini-agent-c で report
./mini-agent/examples/1-openclaw-fleet/tools/telegram_report.sh \
    "$(curl -s https://nou.run/api/mesh/stale | jq '.count') peers still on old version"
```

## まとめ

**短期 (今日-今週)**:
- 別セッションに A4 (署名検証) と A6 (.app bundle 置換) を渡す
- 定期チェック (A1) と UI (A2) も投げる

**中期 (1-2週)**:
- relay broadcast 実装 (B1-B3)
- Windows update (C1)

**長期**:
- "Sparkle Swift" (純 Swift版 Sparkle) の検討
- または Cask 経由 (`brew upgrade nou`) を推奨フロー化

**mini-agent-c の役割**: NOU 本体には触らないで、release verify + peer notification の周辺自動化を担当。

---

## 付録: 今すぐ別セッションに渡す diff 提案

**diff 1: 定期チェック (AppDelegate.swift)**
```swift
// applicationDidFinishLaunching の中に追加
@MainActor
private func setupAutoUpdate() {
    Task {
        // 起動時チェック
        await UpdateChecker.shared.checkForUpdates()
        await MainActor.run { self.handleUpdateState() }
    }
    // 24h 間隔
    updateTimer = Timer.scheduledTimer(withTimeInterval: 86400, repeats: true) { _ in
        Task { @MainActor in
            await UpdateChecker.shared.checkForUpdates()
            self.handleUpdateState()
        }
    }
}

private func handleUpdateState() {
    guard UpdateChecker.shared.isUpdateAvailable else { return }
    // menubar に •
    statusItem?.button?.title = "NOU•"
    // banner は menu open 時に表示
}
```

**diff 2: 署名検証 (UpdateChecker.swift)**
(上のA4 参照、コピペで使える)

**diff 3: .app bundle 置換 (UpdateChecker.swift)**
```swift
// 現状の 128行-156行 の binary cp を以下に置換:
// Assumption: zip extract で "NOU.app" bundle が得られる
let extractedApp = extractDir.appendingPathComponent("NOU.app")
guard FileManager.default.fileExists(atPath: extractedApp.path) else {
    print("[UpdateChecker] extracted archive does not contain NOU.app")
    return
}

// Verify signature of extracted .app
guard verifySignature(at: extractedApp.path) else {
    print("[UpdateChecker] signature verification FAILED")
    return
}

let targetApp = URL(fileURLWithPath: "/Applications/NOU.app")
let oldApp = URL(fileURLWithPath: "/Applications/NOU.app.old")

// Atomic-ish replace
let script = """
#!/bin/bash
set -e
sleep 1.5
rm -rf "\(oldApp.path)"
mv "\(targetApp.path)" "\(oldApp.path)"
mv "\(extractedApp.path)" "\(targetApp.path)"
xattr -cr "\(targetApp.path)"
open "\(targetApp.path)"
"""
```

これを別セッションに投げれば作業分担できる。
