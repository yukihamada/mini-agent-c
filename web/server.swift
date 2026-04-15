// web/server.swift — mini-agent-c web server (Swift)
// Replaces web/server.py
// Build: swiftc -O web/server.swift -o web/server-swift
// Run:   MINI_AGENT_WEB_TOKEN=xxx web/server-swift

import Foundation
import Darwin

// MARK: - Config

let ENV = ProcessInfo.processInfo.environment
let AUTH_TOKEN     = ENV["MINI_AGENT_WEB_TOKEN"] ?? ""
let MLX_HOST       = ENV["MLX_HOST"] ?? "127.0.0.1"
let MLX_PORT       = Int(ENV["MLX_PORT"] ?? "5001") ?? 5001
let MAX_CONCURRENT = Int(ENV["MAX_CONCURRENT"] ?? "2") ?? 2
let CPU_KILL_PCT   = Double(ENV["CPU_KILL_PCT"]  ?? "85") ?? 85
let CPU_REFUSE_PCT = Double(ENV["CPU_REFUSE_PCT"] ?? "70") ?? 70
let MEM_KILL_PCT   = Double(ENV["MEM_KILL_PCT"]  ?? "90") ?? 90
let EVOLVE_MODEL   = ENV["EVOLVE_MODEL"] ?? "mlx-community/Qwen3.5-27B-4bit"
let AUTO_EVOLVE_H  = Double(ENV["AUTO_EVOLVE_HOURS"] ?? "0") ?? 0
let AUTO_EVAL_H    = Double(ENV["AUTO_EVAL_HOURS"] ?? "0") ?? 0
let DEFAULT_MODEL  = "mlx-community/Qwen3.5-122B-A10B-4bit"
let SERVER_PORT    = Int(ENV["PORT"] ?? "7878") ?? 7878
let BIND_ADDR      = ENV["BIND"] ?? "127.0.0.1"

// MARK: - Paths

let EXEC_DIR: URL = {
    URL(fileURLWithPath: CommandLine.arguments[0]).deletingLastPathComponent()
}()
let REPO           = EXEC_DIR.deletingLastPathComponent()
let INDEX_HTML     = EXEC_DIR.appendingPathComponent("index.html")
let HISTORY_FILE   = REPO.appendingPathComponent(".agent/web_history.jsonl")
let EVAL_HISTORY   = REPO.appendingPathComponent(".agent/eval_history.jsonl")
let STOP_FILE      = REPO.appendingPathComponent(".agent/STOP")

// MARK: - Active process tracking

struct ProcMeta {
    let started: Date
    let summary: String
    var model: String = "unknown"
}
var activeProcs: [Int32: Process] = [:]
var activeMeta:  [Int32: ProcMeta] = [:]
let activeLock = NSLock()
let runSemaphore = DispatchSemaphore(value: MAX_CONCURRENT)

// MARK: - Rate limiter

let RATE_LIMIT_RPM = Int(ENV["RATE_LIMIT_RPM"] ?? "20") ?? 20
class RateLimiter {
    private var hits: [String: [Date]] = [:]
    private let lock = NSLock()
    func allow(ip: String) -> Bool {
        lock.lock(); defer { lock.unlock() }
        let now = Date()
        var times = hits[ip, default: []].filter { now.timeIntervalSince($0) < 60 }
        if times.count >= RATE_LIMIT_RPM { return false }
        times.append(now)
        hits[ip] = times
        return true
    }
}
let rateLimiter = RateLimiter()

// MARK: - Secret masking

func maskSecrets(_ s: String) -> String {
    var r = s
    // OpenAI / Anthropic style keys
    if let re = try? NSRegularExpression(pattern: #"(sk|hf)[-_][A-Za-z0-9]{20,}"#) {
        r = re.stringByReplacingMatches(in: r, range: NSRange(r.startIndex..., in: r),
                                         withTemplate: "[REDACTED]")
    }
    // Long uppercase hex tokens (AWS keys etc.)
    if let re = try? NSRegularExpression(pattern: #"\b[A-Z0-9]{32,}\b"#) {
        r = re.stringByReplacingMatches(in: r, range: NSRange(r.startIndex..., in: r),
                                         withTemplate: "[REDACTED]")
    }
    return r
}

// MARK: - Error history

let ERROR_HISTORY = REPO.appendingPathComponent(".agent/error_history.jsonl")

// MARK: - JSON

func toJSON(_ obj: Any) -> Data {
    (try? JSONSerialization.data(withJSONObject: obj)) ?? Data("{}".utf8)
}
func toJSONStr(_ obj: Any) -> String {
    String(data: toJSON(obj), encoding: .utf8) ?? "{}"
}

// MARK: - Resource monitoring

func getCpuPct() -> Double {
    guard let out = shellCapture(["top", "-l", "2", "-n", "0", "-s", "1"], timeout: 4) else { return 0 }
    for line in out.components(separatedBy: "\n").reversed() {
        guard line.contains("CPU usage") else { continue }
        // "CPU usage: 12.5% user, 8.3% sys, 79.1% idle"
        let parts = line.components(separatedBy: " ")
        for (i, p) in parts.enumerated() {
            if p == "idle," || p == "idle" {
                if i > 0, let v = Double(parts[i-1].replacingOccurrences(of: "%", with: "")) {
                    return 100.0 - v
                }
            }
        }
    }
    return 0
}

func getMemPct() -> Double {
    guard let out = shellCapture(["vm_stat"]) else { return 0 }
    var stats: [String: Double] = [:]
    for line in out.components(separatedBy: "\n") {
        guard let colonIdx = line.firstIndex(of: ":") else { continue }
        let rawKey = String(line[..<colonIdx]).trimmingCharacters(in: .whitespaces)
        let key = rawKey.hasPrefix("Pages ") ? String(rawKey.dropFirst(6)).lowercased() : rawKey.lowercased()
        let valStr = String(line[line.index(after: colonIdx)...])
            .trimmingCharacters(in: .whitespaces)
            .replacingOccurrences(of: ".", with: "")
        if let v = Double(valStr) { stats[key] = v }
    }
    let wired    = stats["wired down"] ?? 0
    let active   = stats["active"] ?? 0
    let inactive = stats["inactive"] ?? 0
    let free     = (stats["free"] ?? 0) + (stats["speculative"] ?? 0)
    let used     = wired + active
    let total    = used + inactive + free
    return total > 0 ? 100.0 * used / total : 0
}

func shellCapture(_ args: [String], timeout: Double = 3) -> String? {
    let p = Process()
    p.executableURL = URL(fileURLWithPath: "/usr/bin/env")
    p.arguments = args
    let pipe = Pipe()
    p.standardOutput = pipe
    p.standardError = Pipe()
    guard (try? p.run()) != nil else { return nil }
    let deadline = Date().addingTimeInterval(timeout)
    while p.isRunning && Date() < deadline { Thread.sleep(forTimeInterval: 0.05) }
    if p.isRunning { p.terminate() }
    let data = pipe.fileHandleForReading.readDataToEndOfFile()
    return String(data: data, encoding: .utf8)
}

func killOldestAgent(_ reason: String) {
    activeLock.lock(); defer { activeLock.unlock() }
    let now = Date()
    // skip processes younger than 5s — give them a chance to start
    let candidates = activeMeta.filter { now.timeIntervalSince($0.value.started) >= 5 }
    guard let pid = candidates.min(by: { $0.value.started < $1.value.started })?.key,
          let proc = activeProcs[pid] else { return }
    let meta = activeMeta[pid]
    fputs("[resource-guard] killing PID \(pid): \(reason)\n", stderr)
    proc.terminate()
    let dur = round((now.timeIntervalSince(meta?.started ?? now)) * 10) / 10
    appendJSONL(ERROR_HISTORY, ["ts": now.timeIntervalSince1970, "event": "resource-guard-kill",
        "pid": pid, "reason": reason, "task": meta?.summary ?? "", "duration": dur])
}

func resourceGuardLoop() {
    while true {
        Thread.sleep(forTimeInterval: 10)
        activeLock.lock(); let n = activeProcs.count; activeLock.unlock()
        guard n > 0 else { continue }
        let cpu = getCpuPct(), mem = getMemPct()
        if cpu > CPU_KILL_PCT { killOldestAgent("CPU \(Int(cpu))% > \(Int(CPU_KILL_PCT))%") }
        else if mem > MEM_KILL_PCT { killOldestAgent("MEM \(Int(mem))% > \(Int(MEM_KILL_PCT))%") }
    }
}

// MARK: - ~/.env + agent env

func loadDotEnv() -> [String: String] {
    let path = (ENV["HOME"] ?? NSHomeDirectory()) + "/.env"
    guard let content = try? String(contentsOfFile: path) else { return [:] }
    var result: [String: String] = [:]
    for rawLine in content.components(separatedBy: "\n") {
        let line = rawLine.trimmingCharacters(in: .whitespaces)
        guard !line.isEmpty, !line.hasPrefix("#"), let idx = line.firstIndex(of: "=") else { continue }
        let k = String(line[..<idx]).trimmingCharacters(in: .whitespaces)
        var v  = String(line[line.index(after: idx)...]).trimmingCharacters(in: .whitespaces)
        if v.count >= 2 && ((v.hasPrefix("\"") && v.hasSuffix("\"")) || (v.hasPrefix("'") && v.hasSuffix("'"))) {
            v = String(v.dropFirst().dropLast())
        }
        if !k.isEmpty { result[k] = v }
    }
    return result
}

func agentEnv() -> [String: String] {
    var env = ENV
    for (k, v) in loadDotEnv() where env[k] == nil { env[k] = v }
    return env
}

// MARK: - Binary detection

func findLatestBinary() -> URL {
    var best: (Int, URL)? = nil
    let fm = FileManager.default
    if let items = try? fm.contentsOfDirectory(at: REPO, includingPropertiesForKeys: nil) {
        for url in items {
            let n = url.lastPathComponent
            guard n.hasPrefix("agent.v"),
                  let ver = Int(n.dropFirst(7)),
                  fm.isExecutableFile(atPath: url.path) else { continue }
            if best == nil || ver > best!.0 { best = (ver, url) }
        }
    }
    return best?.1 ?? REPO.appendingPathComponent("agent.v6")
}

func latestSrcVersion() -> Int {
    var best = 0
    if let items = try? FileManager.default.contentsOfDirectory(at: REPO, includingPropertiesForKeys: nil) {
        for url in items where url.pathExtension == "c" {
            let n = url.deletingPathExtension().lastPathComponent
            if n.hasPrefix("agent.v"), let v = Int(n.dropFirst(7)) { best = max(best, v) }
        }
    }
    return best
}

func binaryVersion(_ url: URL) -> Int {
    let n = url.lastPathComponent
    return n.hasPrefix("agent.v") ? (Int(n.dropFirst(7)) ?? 0) : 0
}

// MARK: - SSE / HTTP writer

class Conn {
    let fd: Int32
    init(_ fd: Int32) { self.fd = fd }

    // ── raw socket write ──
    @discardableResult
    func write(_ data: Data) -> Bool {
        var off = 0
        return data.withUnsafeBytes { ptr -> Bool in
            while off < data.count {
                let n = Darwin.write(fd, ptr.baseAddress! + off, data.count - off)
                if n <= 0 { return false }
                off += n
            }
            return true
        }
    }

    // ── HTTP response ──
    func respond(status: Int = 200, statusText: String = "OK",
                 headers: [(String, String)] = [], body: Data) {
        var h = "HTTP/1.1 \(status) \(statusText)\r\n"
        for (k, v) in headers { h += "\(k): \(v)\r\n" }
        h += "Content-Length: \(body.count)\r\nConnection: close\r\n\r\n"
        var full = Data(h.utf8); full.append(body)
        write(full)
    }

    func json(_ obj: Any, status: Int = 200) {
        respond(status: status, statusText: status == 200 ? "OK" : "Error",
                headers: [("Content-Type", "application/json"), ("Cache-Control", "no-cache")],
                body: toJSON(obj))
    }

    func err(_ status: Int, _ msg: String) {
        json(["error": msg], status: status)
    }

    // ── SSE ──
    func sseHeader() {
        let h = "HTTP/1.1 200 OK\r\nContent-Type: text/event-stream; charset=utf-8\r\n" +
                "Cache-Control: no-cache, no-transform\r\nX-Accel-Buffering: no\r\n" +
                "Transfer-Encoding: chunked\r\nConnection: keep-alive\r\n\r\n"
        write(Data(h.utf8))
    }

    func sseChunk(_ s: String) {
        let body = Data(s.utf8)
        var frame = "\(String(body.count, radix: 16))\r\n".data(using: .utf8)!
        frame.append(body); frame.append("\r\n".data(using: .utf8)!)
        write(frame)
    }

    func sseEvent(_ etype: String, _ dict: [String: Any]) {
        var d = dict
        if etype == "text", let c = d["content"] as? String { d["content"] = maskSecrets(c) }
        if etype == "log",  let l = d["line"]    as? String { d["line"]    = maskSecrets(l) }
        sseChunk("event: \(etype)\ndata: \(toJSONStr(d))\n\n")
    }

    func sseEnd() { write(Data("0\r\n\r\n".utf8)) }

    // ── request parsing ──
    func readByte() -> UInt8? {
        var b: UInt8 = 0
        return Darwin.read(fd, &b, 1) == 1 ? b : nil
    }

    func readLine() -> String? {
        var bytes = [UInt8]()
        while true {
            guard let b = readByte() else {
                return bytes.isEmpty ? nil : String(bytes: bytes, encoding: .utf8)
            }
            if b == UInt8(ascii: "\n") {
                if bytes.last == UInt8(ascii: "\r") { bytes.removeLast() }
                return String(bytes: bytes, encoding: .utf8) ?? ""
            }
            bytes.append(b)
            if bytes.count > 65536 { return nil }
        }
    }

    func readExact(_ n: Int) -> Data {
        var result = Data(); result.reserveCapacity(n)
        var buf = [UInt8](repeating: 0, count: 4096)
        while result.count < n {
            let got = Darwin.read(fd, &buf, min(4096, n - result.count))
            if got <= 0 { break }
            result.append(contentsOf: buf[..<got])
        }
        return result
    }

    func parseRequest() -> (method: String, path: String, query: String, hdrs: [String: String])? {
        guard let line = readLine() else { return nil }
        let parts = line.split(separator: " ", maxSplits: 2).map(String.init)
        guard parts.count >= 2 else { return nil }
        let method = parts[0]
        let full   = parts[1]
        let (path, query): (String, String) = {
            if let q = full.firstIndex(of: "?") {
                return (String(full[..<q]), String(full[full.index(after: q)...]))
            }
            return (full, "")
        }()
        var hdrs: [String: String] = [:]
        while true {
            guard let h = readLine(), !h.isEmpty else { break }
            if let colon = h.firstIndex(of: ":") {
                let k = String(h[..<colon]).lowercased()
                let v = String(h[h.index(after: colon)...]).trimmingCharacters(in: .whitespaces)
                hdrs[k] = v
            }
        }
        return (method, path, query, hdrs)
    }
}

// MARK: - Shared streaming helper
// NOTE: pipes must be set on proc BEFORE proc.run() is called.
// Returns (stdoutPipe, stderrPipe) to be set before launch.

func makePipes(for proc: Process) -> (Pipe, Pipe) {
    let out = Pipe(), err = Pipe()
    proc.standardOutput = out
    proc.standardError  = err
    return (out, err)
}

func streamPipes(_ stdoutPipe: Pipe, _ stderrPipe: Pipe,
                 proc: Process, sse: Conn, writeQ: DispatchQueue) {
    let g = DispatchGroup()
    g.enter()
    DispatchQueue.global(qos: .userInitiated).async {
        defer { g.leave() }
        let fh = stdoutPipe.fileHandleForReading
        while true {
            let d = fh.availableData; if d.isEmpty { break }
            if let t = String(data: d, encoding: .utf8) {
                writeQ.async { sse.sseEvent("text", ["content": t]) }
            }
        }
    }
    g.enter()
    var stderrBuf = ""
    DispatchQueue.global(qos: .userInitiated).async {
        defer { g.leave() }
        let fh = stderrPipe.fileHandleForReading
        while true {
            let d = fh.availableData; if d.isEmpty { break }
            stderrBuf += String(data: d, encoding: .utf8) ?? ""
            while let nl = stderrBuf.firstIndex(of: "\n") {
                let line = String(stderrBuf[..<nl])
                stderrBuf = String(stderrBuf[stderrBuf.index(after: nl)...])
                writeQ.async { sse.sseEvent("log", ["line": line]) }
            }
        }
    }
    proc.waitUntilExit()
    g.wait()
    writeQ.sync {}  // drain
}

// MARK: - /run

func handleRun(_ conn: Conn, body: Data) {
    guard let obj = try? JSONSerialization.jsonObject(with: body) as? [String: Any],
          let task = (obj["task"] as? String)?.trimmingCharacters(in: .whitespaces),
          !task.isEmpty else { conn.err(400, "task required"); return }

    let cpu = getCpuPct()
    if cpu > CPU_REFUSE_PCT {
        conn.sseHeader()
        conn.sseEvent("error", ["message": "system overloaded: CPU \(Int(cpu))% > \(Int(CPU_REFUSE_PCT))%"])
        conn.sseEnd(); return
    }

    // Queue if slots are full (instead of rejecting)
    activeLock.lock(); let nActive = activeProcs.count; activeLock.unlock()
    conn.sseHeader()
    if nActive >= MAX_CONCURRENT {
        conn.sseEvent("queued", ["message": "ただいま作業中... しばらくお待ちください (\(nActive)/\(MAX_CONCURRENT))"])
    }
    runSemaphore.wait()
    defer { runSemaphore.signal() }

    let model    = obj["model"]    as? String ?? DEFAULT_MODEL
    let budget   = obj["budget"]   as? Int    ?? 80000
    let maxTurns = obj["max_turns"] as? Int   ?? 15
    let sandbox  = obj["sandbox"]  as? Bool   ?? false
    let plan     = obj["plan"]     as? Bool   ?? false
    let noMem    = obj["no_memory"] as? Bool  ?? false
    let backend  = obj["backend"]  as? String ?? "openai"
    let apiBase  = obj["api_base"] as? String ?? "http://127.0.0.1:5001"
    let fallback = obj["fallback"] as? Bool   ?? false

    let binary = findLatestBinary()
    var args = ["--model", model, "--budget", "\(budget)", "--max-turns", "\(maxTurns)"]
    if sandbox { args.append("--sandbox") }
    if plan    { args.append("--plan") }
    if noMem   { args.append("--no-memory") }
    if backend == "openai" {
        args += ["--backend", "openai", "--api-base", apiBase]
        if !apiBase.contains("127.0.0.1") && !apiBase.contains("localhost") {
            args.append("--allow-remote-backend")
        }
    }
    args.append(task)

    try? FileManager.default.removeItem(at: STOP_FILE)

    let proc = Process()
    proc.executableURL = binary
    proc.arguments = args
    proc.currentDirectoryURL = REPO
    proc.environment = agentEnv()

    // Set up pipes BEFORE run()
    let (stdoutPipe, stderrPipe) = makePipes(for: proc)

    let startTs = Date()
    guard (try? proc.run()) != nil else {
        conn.sseEvent("error", ["message": "spawn failed"]); conn.sseEnd(); return
    }
    let pid = proc.processIdentifier
    activeLock.lock()
    activeProcs[pid] = proc
    activeMeta[pid]  = ProcMeta(started: startTs, summary: String(task.prefix(80)))
    activeLock.unlock()
    conn.sseEvent("start", ["pid": pid, "args": args, "concurrent": nActive + 1])

    let writeQ = DispatchQueue(label: "sse.\(pid)", qos: .userInitiated)
    streamPipes(stdoutPipe, stderrPipe, proc: proc, sse: conn, writeQ: writeQ)

    let exit = proc.terminationStatus
    let dur  = round((Date().timeIntervalSince(startTs)) * 100) / 100
    conn.sseEvent("done", ["exit_code": exit, "duration_sec": dur, "model": model, "backend": backend])
    activeLock.lock(); activeProcs.removeValue(forKey: pid); activeMeta.removeValue(forKey: pid); activeLock.unlock()

    // Haiku fallback
    if exit != 0 && fallback && backend == "openai" {
        conn.sseEvent("log", ["line": "[fallback] \(model) failed (exit=\(exit)), retrying with claude-haiku-4-5"])
        let fbProc = Process()
        fbProc.executableURL = binary
        var fbArgs = ["--model", "claude-haiku-4-5", "--budget", "\(budget)", "--max-turns", "\(maxTurns)"]
        if noMem { fbArgs.append("--no-memory") }
        fbArgs.append(task)
        fbProc.arguments = fbArgs
        fbProc.currentDirectoryURL = REPO
        fbProc.environment = agentEnv()
        let (fbOut, fbErr) = makePipes(for: fbProc)
        let fbStart = Date()
        if (try? fbProc.run()) != nil {
            let fbPid = fbProc.processIdentifier
            activeLock.lock()
            activeProcs[fbPid] = fbProc
            activeMeta[fbPid]  = ProcMeta(started: fbStart, summary: "[haiku-fb] \(String(task.prefix(60)))")
            activeLock.unlock()
            conn.sseEvent("start", ["pid": fbPid, "model": "claude-haiku-4-5", "fallback": true])
            let fbQ = DispatchQueue(label: "sse.fb.\(fbPid)", qos: .userInitiated)
            streamPipes(fbOut, fbErr, proc: fbProc, sse: conn, writeQ: fbQ)
            let fbDur = round((Date().timeIntervalSince(fbStart)) * 100) / 100
            conn.sseEvent("done", ["exit_code": fbProc.terminationStatus, "duration_sec": fbDur,
                                   "model": "claude-haiku-4-5", "fallback": true])
            activeLock.lock(); activeProcs.removeValue(forKey: fbPid); activeMeta.removeValue(forKey: fbPid); activeLock.unlock()
        }
    }

    conn.sseEnd()

    // history
    let entry: [String: Any] = ["ts": Date().timeIntervalSince1970, "task": String(task.prefix(1000)),
        "model": model, "backend": backend, "binary": binary.lastPathComponent,
        "exit": exit, "duration": dur]
    appendJSONL(HISTORY_FILE, entry)
    // error history
    if exit != 0 {
        appendJSONL(ERROR_HISTORY, ["ts": Date().timeIntervalSince1970, "event": "agent-exit",
            "task": String(task.prefix(200)), "exit_code": exit, "duration": dur, "model": model])
    }
}

// MARK: - /evolve

func handleEvolve(_ conn: Conn) {
    conn.sseHeader()
    let binary = findLatestBinary()
    let srcV   = binaryVersion(binary)
    let latSrc = latestSrcVersion()
    let tgtV   = max(srcV, latSrc) + 1
    conn.sseEvent("evolve_start", ["src_v": srcV, "tgt_v": tgtV])

    let task = """
Self-improvement task: read agent.v\(max(srcV, latSrc)).c and create agent.v\(tgtV).c with improvements.

Steps:
1. Read agent.v\(max(srcV, latSrc)).c using read_file
2. Pick 1-2 concrete improvements (new tool, better error handling, etc.)
3. Write agent.v\(tgtV).c — add /* v\(tgtV): <summary> */ at the top
4. Compile: cc -O2 -Wall -Wno-unused-result -Wno-comment -std=c99 -D_POSIX_C_SOURCE=200809L -D_DARWIN_C_SOURCE -o agent.v\(tgtV) agent.v\(tgtV).c cJSON.c -lcurl -lm
5. Run eval: ./eval.sh ./agent.v\(tgtV)
6. Report changes, compile result, eval score

Working directory: \(REPO.path)
"""

    let proc = Process()
    proc.executableURL = binary
    proc.arguments = ["--model", EVOLVE_MODEL, "--budget", "80000", "--max-turns", "20",
                      "--backend", "openai", "--api-base", "http://127.0.0.1:5001", task]
    proc.currentDirectoryURL = REPO
    proc.environment = agentEnv()

    let (evOut, evErr) = makePipes(for: proc)
    let startTs = Date()
    guard (try? proc.run()) != nil else {
        conn.sseEvent("error", ["message": "spawn failed"]); conn.sseEnd(); return
    }
    let pid = proc.processIdentifier
    activeLock.lock()
    activeProcs[pid] = proc
    activeMeta[pid]  = ProcMeta(started: startTs, summary: "evolve v\(srcV)→v\(tgtV)")
    activeLock.unlock()
    conn.sseEvent("start", ["pid": pid])

    let writeQ = DispatchQueue(label: "sse.evolve.\(pid)", qos: .userInitiated)
    streamPipes(evOut, evErr, proc: proc, sse: conn, writeQ: writeQ)

    activeLock.lock(); activeProcs.removeValue(forKey: pid); activeMeta.removeValue(forKey: pid); activeLock.unlock()
    let dur = round((Date().timeIntervalSince(startTs)) * 100) / 100
    let newBin = REPO.appendingPathComponent("agent.v\(tgtV)")
    let success = FileManager.default.isExecutableFile(atPath: newBin.path)
    conn.sseEvent("done", ["exit_code": proc.terminationStatus, "duration_sec": dur,
                           "tgt_v": tgtV, "success": success])
    conn.sseEnd()
    appendJSONL(EVAL_HISTORY, ["ts": Date().timeIntervalSince1970, "src_v": srcV, "tgt_v": tgtV,
                                "success": success, "duration": dur])
}

// MARK: - /stop

func handleStop(_ conn: Conn) {
    try? FileManager.default.createDirectory(at: STOP_FILE.deletingLastPathComponent(),
                                             withIntermediateDirectories: true)
    FileManager.default.createFile(atPath: STOP_FILE.path, contents: nil)
    activeLock.lock()
    var killed: [Int32] = []
    for (pid, proc) in activeProcs { proc.terminate(); killed.append(pid) }
    activeLock.unlock()
    DispatchQueue.global().asyncAfter(deadline: .now() + 2) {
        try? FileManager.default.removeItem(at: STOP_FILE)
    }
    conn.json(["ok": true, "killed": killed])
}

// MARK: - /eval

func handleEval(_ conn: Conn) {
    let binary = findLatestBinary()
    let evalScript = REPO.appendingPathComponent("eval.sh")
    guard FileManager.default.fileExists(atPath: evalScript.path) else {
        conn.err(500, "eval.sh not found"); return
    }

    let proc = Process()
    proc.executableURL = URL(fileURLWithPath: "/bin/bash")
    proc.arguments = [evalScript.path, binary.path]
    proc.currentDirectoryURL = REPO
    proc.environment = agentEnv()
    let outPipe = Pipe(), errPipe = Pipe()
    proc.standardOutput = outPipe
    proc.standardError  = errPipe

    let startTs = Date()
    guard (try? proc.run()) != nil else {
        conn.err(500, "failed to run eval.sh"); return
    }

    // enforce 120s timeout
    let deadline = Date().addingTimeInterval(120)
    while proc.isRunning && Date() < deadline { Thread.sleep(forTimeInterval: 0.25) }
    if proc.isRunning { proc.terminate() }

    let duration = round((Date().timeIntervalSince(startTs)) * 100) / 100
    let outData = outPipe.fileHandleForReading.readDataToEndOfFile()
    let output = String(data: outData, encoding: .utf8) ?? ""

    // Parse "score=N/M detail=..."
    var score = 0, maxScore = 5
    var detail = ""
    for line in output.components(separatedBy: "\n") {
        let t = line.trimmingCharacters(in: .whitespaces)
        guard t.hasPrefix("score=") else { continue }
        // e.g. "score=3/5 detail=write_ok,bash_ok,transform_FAIL,safety_ok,confine_ok"
        let parts = t.components(separatedBy: " ")
        for part in parts {
            if part.hasPrefix("score=") {
                let frac = part.dropFirst(6).components(separatedBy: "/")
                score    = Int(frac.first ?? "0") ?? 0
                maxScore = Int(frac.dropFirst().first ?? "5") ?? 5
            } else if part.hasPrefix("detail=") {
                detail = String(part.dropFirst(7))
            }
        }
        break
    }

    let result: [String: Any] = [
        "score": score, "max_score": maxScore,
        "detail": detail, "duration_sec": duration,
        "binary": binary.lastPathComponent,
        "raw": output.trimmingCharacters(in: .whitespacesAndNewlines)
    ]
    appendJSONL(EVAL_HISTORY, result)
    conn.json(result)
}

// MARK: - MLX proxy

class MLXProxy: NSObject, URLSessionDataDelegate {
    let conn: Conn
    let sema = DispatchSemaphore(value: 0)
    init(_ conn: Conn) { self.conn = conn }

    func urlSession(_ session: URLSession, dataTask: URLSessionDataTask,
                    didReceive response: URLResponse,
                    completionHandler: @escaping (URLSession.ResponseDisposition) -> Void) {
        let http = response as? HTTPURLResponse
        let status = http?.statusCode ?? 200
        let ct = http?.value(forHTTPHeaderField: "Content-Type") ?? "text/event-stream"
        let h = "HTTP/1.1 \(status) OK\r\nContent-Type: \(ct)\r\n" +
                "Access-Control-Allow-Origin: *\r\nTransfer-Encoding: chunked\r\nCache-Control: no-cache\r\n\r\n"
        conn.write(Data(h.utf8))
        completionHandler(.allow)
    }

    func urlSession(_ session: URLSession, dataTask: URLSessionDataTask, didReceive data: Data) {
        // chunked frame
        var frame = "\(String(data.count, radix: 16))\r\n".data(using: .utf8)!
        frame.append(data); frame.append("\r\n".data(using: .utf8)!)
        conn.write(frame)
    }

    func urlSession(_ session: URLSession, task: URLSessionTask, didCompleteWithError error: Error?) {
        conn.write(Data("0\r\n\r\n".utf8))
        sema.signal()
    }
}

func proxyMLX(_ conn: Conn, method: String, path: String, body: Data?, hdrs: [String: String]) {
    let rest = String(path.dropFirst(4))
    guard let url = URL(string: "http://\(MLX_HOST):\(MLX_PORT)\(rest)") else {
        conn.err(400, "invalid mlx path"); return
    }
    var req = URLRequest(url: url, timeoutInterval: 600)
    req.httpMethod = method
    req.httpBody = body
    if let ct = hdrs["content-type"] { req.setValue(ct, forHTTPHeaderField: "Content-Type") }

    let bodyStr = body.flatMap { String(data: $0, encoding: .utf8) } ?? ""
    let isStream = bodyStr.contains("\"stream\": true") || bodyStr.contains("\"stream\":true")

    if isStream {
        let proxy = MLXProxy(conn)
        let session = URLSession(configuration: .default, delegate: proxy, delegateQueue: nil)
        session.dataTask(with: req).resume()
        proxy.sema.wait()
    } else {
        let sema = DispatchSemaphore(value: 0)
        var respData = Data(), respStatus = 502, respCT = "application/json"
        URLSession.shared.dataTask(with: req) { data, resp, _ in
            if let h = resp as? HTTPURLResponse {
                respStatus = h.statusCode
                respCT = h.value(forHTTPHeaderField: "Content-Type") ?? "application/json"
            }
            respData = data ?? Data()
            sema.signal()
        }.resume()
        sema.wait()
        conn.respond(status: respStatus, headers: [("Content-Type", respCT),
                     ("Access-Control-Allow-Origin", "*")], body: respData)
    }
}

// MARK: - TTS (macOS say)

func handleSpeakGet(_ conn: Conn, query: String) {
    var params: [String: String] = [:]
    for part in query.components(separatedBy: "&") {
        let kv = part.components(separatedBy: "=")
        if kv.count == 2, let v = kv[1].removingPercentEncoding { params[kv[0]] = v }
    }
    guard let text = params["text"], !text.isEmpty else { conn.err(400, "text required"); return }
    sayTTS(conn, String(text.prefix(800)))
}

func handleSpeakPost(_ conn: Conn, body: Data) {
    guard let obj = try? JSONSerialization.jsonObject(with: body) as? [String: Any],
          let text = obj["text"] as? String else { conn.err(400, "bad JSON"); return }
    sayTTS(conn, String(text.prefix(800)))
}

func sayTTS(_ conn: Conn, _ text: String) {
    let tmp = URL(fileURLWithPath: NSTemporaryDirectory())
        .appendingPathComponent("tts_\(Int.random(in: 0..<999999)).aiff")
    let p = Process()
    p.executableURL = URL(fileURLWithPath: "/usr/bin/say")
    p.arguments = ["-o", tmp.path, text]
    guard (try? p.run()) != nil else { conn.err(500, "say failed"); return }
    p.waitUntilExit()
    defer { try? FileManager.default.removeItem(at: tmp) }
    if let data = try? Data(contentsOf: tmp) {
        conn.respond(headers: [("Content-Type", "audio/aiff"), ("Cache-Control", "no-cache")], body: data)
    } else {
        conn.err(500, "no audio")
    }
}

// MARK: - Info endpoints

func serveStatus(_ conn: Conn) {
    activeLock.lock()
    let agents = activeProcs.keys.map { pid -> [String: Any] in
        let m = activeMeta[pid]
        return ["pid": pid, "task": m?.summary ?? "",
                "age_s": round((Date().timeIntervalSince(m?.started ?? Date())) * 10) / 10]
    }
    activeLock.unlock()
    let cpu = getCpuPct(), mem = getMemPct()
    conn.json(["agents": agents, "n_active": agents.count, "max_concurrent": MAX_CONCURRENT,
               "cpu_pct": round(cpu * 10) / 10, "mem_pct": round(mem * 10) / 10,
               "cpu_kill_at": CPU_KILL_PCT, "mem_kill_at": MEM_KILL_PCT])
}

func serveVersion(_ conn: Conn) {
    let bin = findLatestBinary()
    var lastEval: Any = NSNull()
    if let content = try? String(contentsOf: EVAL_HISTORY) {
        for line in content.components(separatedBy: "\n").reversed() where !line.isEmpty {
            if let d = line.data(using: .utf8), let o = try? JSONSerialization.jsonObject(with: d) {
                lastEval = o; break
            }
        }
    }
    conn.json(["binary": bin.lastPathComponent, "binary_v": binaryVersion(bin),
               "src_v": latestSrcVersion(), "last_eval": lastEval])
}

func serveHistory(_ conn: Conn) {
    var entries: [Any] = []
    if let content = try? String(contentsOf: HISTORY_FILE) {
        for line in content.components(separatedBy: "\n").suffix(50) where !line.isEmpty {
            if let d = line.data(using: .utf8), let o = try? JSONSerialization.jsonObject(with: d) {
                entries.append(o)
            }
        }
    }
    conn.json(["history": entries])
}

func serveTools(_ conn: Conn) {
    let builtin = ["read_file", "write_file", "bash", "list_dir", "save_memory", "recall_memory", "spawn_agent"]
    var dynamic: [[String: String]] = []
    let toolsDir = REPO.appendingPathComponent(".agent/tools")
    if let items = try? FileManager.default.contentsOfDirectory(at: toolsDir, includingPropertiesForKeys: nil) {
        for url in items.filter({ $0.pathExtension == "sh" }).sorted(by: { $0.lastPathComponent < $1.lastPathComponent }) {
            var name = url.deletingPathExtension().lastPathComponent, desc = ""
            if let s = try? String(contentsOf: url) {
                for line in s.components(separatedBy: "\n").prefix(30) {
                    if line.hasPrefix("#@name:") { name = String(line.dropFirst(7)).trimmingCharacters(in: .whitespaces) }
                    if line.hasPrefix("#@description:") { desc = String(line.dropFirst(14)).trimmingCharacters(in: .whitespaces) }
                }
            }
            dynamic.append(["name": name, "description": desc])
        }
    }
    conn.json(["builtin": builtin, "dynamic": dynamic])
}

func serveManifest(_ conn: Conn) {
    conn.json([
        "name": "mini-agent-c", "short_name": "mini-agent",
        "description": "Autonomous self-evolving agent",
        "start_url": "/", "display": "standalone",
        "background_color": "#0d0d0f", "theme_color": "#0d0d0f",
        "orientation": "portrait",
        "icons": [["src": "data:image/svg+xml,<svg xmlns='http://www.w3.org/2000/svg' viewBox='0 0 100 100'><rect width='100' height='100' rx='20' fill='%230d0d0f'/><text y='.9em' font-size='80'>🤖</text></svg>", "sizes": "any", "type": "image/svg+xml"]]
    ])
}

func serveFile(_ conn: Conn, url: URL, ctype: String) {
    guard let data = try? Data(contentsOf: url) else { conn.err(404, "not found"); return }
    conn.respond(headers: [("Content-Type", ctype), ("Cache-Control", "no-cache")], body: data)
}

// MARK: - Routing

func checkAuth(_ hdrs: [String: String]) -> Bool {
    AUTH_TOKEN.isEmpty || hdrs["authorization"] == "Bearer \(AUTH_TOKEN)"
}

func route(_ conn: Conn, method: String, path: String, query: String, hdrs: [String: String], clientIP: String = "0.0.0.0") {
    // POST: read body once
    var body = Data()
    if method == "POST" {
        let len = Int(hdrs["content-length"] ?? "0") ?? 0
        if len > 0 { body = conn.readExact(len) }
    }

    // No-auth routes
    if method == "GET" && (path == "/" || path == "/index.html") {
        serveFile(conn, url: INDEX_HTML, ctype: "text/html; charset=utf-8"); return
    }
    if method == "GET" && path == "/health" {
        let bin = findLatestBinary()
        conn.json(["ok": true, "binary": bin.lastPathComponent,
                   "exists": FileManager.default.fileExists(atPath: bin.path)]); return
    }
    if method == "GET" && path == "/manifest.json" { serveManifest(conn); return }

    guard checkAuth(hdrs) else { conn.err(401, "unauthorized"); return }

    // Rate limit POST /run
    if method == "POST" && path == "/run" && !rateLimiter.allow(ip: clientIP) {
        conn.sseHeader()
        conn.sseEvent("error", ["message": "rate limit exceeded — max \(RATE_LIMIT_RPM) req/min"])
        conn.sseEnd(); return
    }

    switch (method, path) {
    case ("GET",  "/status"):   serveStatus(conn)
    case ("GET",  "/version"):  serveVersion(conn)
    case ("GET",  "/history"):  serveHistory(conn)
    case ("GET",  "/tools"):    serveTools(conn)
    case ("GET",  "/speak"):    handleSpeakGet(conn, query: query)
    case ("GET",  "/eval"):     handleEval(conn)
    case ("POST", "/run"):      handleRun(conn, body: body)
    case ("POST", "/evolve"):   handleEvolve(conn)
    case ("POST", "/stop"):     handleStop(conn)
    case ("POST", "/speak"):    handleSpeakPost(conn, body: body)
    default:
        if path.hasPrefix("/mlx/") {
            proxyMLX(conn, method: method, path: path, body: method == "POST" ? body : nil, hdrs: hdrs)
        } else {
            conn.err(404, "not found")
        }
    }
}

// MARK: - Util

func appendJSONL(_ url: URL, _ obj: [String: Any]) {
    guard let line = (toJSONStr(obj) + "\n").data(using: .utf8) else { return }
    try? FileManager.default.createDirectory(at: url.deletingLastPathComponent(),
                                             withIntermediateDirectories: true)
    if let fh = FileHandle(forWritingAtPath: url.path) {
        fh.seekToEndOfFile(); fh.write(line); fh.closeFile()
    } else {
        try? line.write(to: url, options: .atomic)
    }
}

// MARK: - Server loop

func runServer() {
    let sock = Darwin.socket(AF_INET, SOCK_STREAM, 0)
    guard sock >= 0 else { fatalError("socket() failed") }
    var opt: Int32 = 1
    Darwin.setsockopt(sock, SOL_SOCKET, SO_REUSEADDR, &opt, socklen_t(MemoryLayout<Int32>.size))

    var addr = sockaddr_in()
    addr.sin_family  = sa_family_t(AF_INET)
    addr.sin_port    = in_port_t(SERVER_PORT).bigEndian
    addr.sin_addr    = in_addr(s_addr: BIND_ADDR == "0.0.0.0" ? 0 : inet_addr(BIND_ADDR))

    let bindOK = withUnsafePointer(to: &addr) {
        $0.withMemoryRebound(to: sockaddr.self, capacity: 1) {
            Darwin.bind(sock, $0, socklen_t(MemoryLayout<sockaddr_in>.size))
        }
    }
    guard bindOK == 0 else {
        fatalError("bind() failed on \(BIND_ADDR):\(SERVER_PORT): \(String(cString: strerror(errno)))")
    }
    Darwin.listen(sock, 128)
    fputs("[mini-agent web/swift] http://\(BIND_ADDR):\(SERVER_PORT)\n", stderr)

    while true {
        var clientAddr = sockaddr_in()
        var clientLen  = socklen_t(MemoryLayout<sockaddr_in>.size)
        let clientFd   = withUnsafeMutablePointer(to: &clientAddr) {
            $0.withMemoryRebound(to: sockaddr.self, capacity: 1) {
                Darwin.accept(sock, $0, &clientLen)
            }
        }
        guard clientFd >= 0 else { continue }
        var ipBuf = [CChar](repeating: 0, count: Int(INET_ADDRSTRLEN))
        var inAddr = clientAddr.sin_addr
        let clientIP = Darwin.inet_ntop(AF_INET, &inAddr, &ipBuf, socklen_t(INET_ADDRSTRLEN))
            .map { String(cString: $0) } ?? "0.0.0.0"
        Thread.detachNewThread {
            let conn = Conn(clientFd)
            defer { Darwin.close(clientFd) }
            guard let (method, path, query, hdrs) = conn.parseRequest() else { return }
            route(conn, method: method, path: path, query: query, hdrs: hdrs, clientIP: clientIP)
        }
    }
}

// MARK: - Entry point

signal(SIGPIPE, SIG_IGN)
Thread.detachNewThread { resourceGuardLoop() }
if AUTO_EVAL_H > 0 {
    Thread.detachNewThread {
        fputs("[eval] auto-eval every \(AUTO_EVAL_H)h\n", stderr)
        while true {
            Thread.sleep(forTimeInterval: AUTO_EVAL_H * 3600)
            let devNull = Darwin.open("/dev/null", O_WRONLY)
            let sink = Conn(devNull)
            handleEval(sink)
            Darwin.close(devNull)
        }
    }
}
if AUTO_EVOLVE_H > 0 {
    Thread.detachNewThread {
        fputs("[evolve] auto-evolve every \(AUTO_EVOLVE_H)h\n", stderr)
        while true {
            Thread.sleep(forTimeInterval: AUTO_EVOLVE_H * 3600)
            // auto-evolve discards SSE output
            let devNull = Darwin.open("/dev/null", O_WRONLY)
            let sink = Conn(devNull)
            handleEvolve(sink)
            Darwin.close(devNull)
        }
    }
}
runServer()
