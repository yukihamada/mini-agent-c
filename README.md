# mini-agent-c

**A minimal autonomous AI agent in pure C, powered by Claude via the Anthropic API.**

mini-agent-c is a zero-dependency (except libcurl + cJSON), lightweight implementation of an agentic AI system with tool-use, prompt caching, persistent memory, dynamic tool loading, and advanced safety features. Written in ~1500 lines of C, it demonstrates that powerful AI agents can be built with minimal abstraction.

---

## Features by Version

### v1 (Initial Release)
- Basic tool-use loop with Anthropic Messages API
- Core tools: `read_file`, `write_file`, `bash`, `list_dir`
- Path confinement (CWD-only, no `..` or `~`)
- Dangerous command denylist for bash execution
- API key redaction in all output

### v2
- **edit_file tool**: Precise in-place string replacement
- Optional `replace_all` parameter for multi-occurrence edits
- Improved error messages for ambiguous edits

### v3
- **Prompt caching**: System prompt + tools cached via `cache_control` (Anthropic)
- **Persistent memory**: `save_memory` / `recall_memory` in `~/.mini-agent/memory.md`
- **Dynamic tools**: Load shell scripts from `.agent/tools/*.sh`
- **Subagent spawning**: `spawn_agent(task)` for recursive task delegation (depth-limited)
- **Context compaction**: Auto-summarize old messages via Claude Haiku to stay under token limits
- Exponential backoff + retry on 529/500-class API errors

### v4
- **Plan mode** (`--plan`): Dry-run all tool calls without side effects
- **Sandbox mode** (`--sandbox`): Execute bash commands under macOS `sandbox-exec`
- **Cost tracking**: Per-session cost estimate based on token usage
- **SSE streaming** (`--stream`): Real-time output streaming (Anthropic backend only)
- `.agent/STOP` kill switch: Creates a stop file to halt agent on next turn
- `.agent/audit.log`: JSONL log of every tool call for auditing

### v5
- **OpenAI-compatible backend** (`--backend openai`): Bridge to run on OpenAI-style APIs
- **Custom API base** (`--api-base`): Use local or alternative LLM endpoints
- Automatic sandbox enforcement for remote backends (unless `--allow-remote-backend`)
- Improved token budget enforcement

### v6
- **Max turns cap** (`--max-turns`): Prevent runaway loops
- **Token budget cap** (`--budget`): Hard limit on total tokens consumed
- Enhanced error handling and retry logic
- Configurable model selection (`--model`)

### v7
- **Improved caching**: Tool array gets cache_control on last tool
- **Spawn depth tracking**: `MINI_AGENT_DEPTH` env var prevents deep recursion
- Better streaming text output with newline handling
- Memory file size limits (16KB memory, 32KB tool output)

### v8 (Current)
- **grep_files tool**: Fast regex search across files (extended regex, respects `.git`/`node_modules`)
- **glob_files tool**: Find files by glob pattern (e.g. `*.c`, `test_*.py`)
- **http_get tool**: Fetch URLs (HTTP/HTTPS) with `--allow-http` flag (max 32KB response)
- **todo tool**: Task list management in `.agent/todo.md` (add, list, done, clear)
- **SIGINT handler**: Graceful Ctrl-C exit with cost summary
- **Interactive REPL mode** (`--interactive` / `-i`): Multi-task conversation sessions
- **Cost tracking**: Detailed breakdown of input/output/cache tokens + $ estimate
- Improved shell quoting for dynamic tools
- Better grep/find patterns: prefer built-in tools over bash for safety

---

## Tools Reference

### File Operations

| Tool | Description | Parameters |
|------|-------------|------------|
| `read_file` | Read file contents | `path` (relative) |
| `write_file` | Write content to file (overwrites) | `path`, `content` |
| `edit_file` | Precise in-place string replacement | `path`, `old_string`, `new_string`, `replace_all` (optional) |

### System Operations

| Tool | Description | Parameters |
|------|-------------|------------|
| `bash` | Execute bash command (60s CPU limit, 100MB output cap) | `command` |
| `list_dir` | List non-hidden directory entries | `path` |
| `grep_files` | **[v8]** Regex search across files (extended regex) | `pattern`, `path` (default: `.`), `include_glob` (optional) |
| `glob_files` | **[v8]** Find files by glob pattern (skips `.git`, `node_modules`) | `pattern`, `path` (default: `.`) |

### Network

| Tool | Description | Parameters |
|------|-------------|------------|
| `http_get` | **[v8]** Fetch URL (HTTP/HTTPS only, requires `--allow-http`) | `url` |

### Task Management

| Tool | Description | Parameters |
|------|-------------|------------|
| `todo` | **[v8]** Manage task list in `.agent/todo.md` | `op` (add\|list\|done\|clear), `item` (for add/done) |

### Memory & Delegation

| Tool | Description | Parameters |
|------|-------------|------------|
| `save_memory` | Persist fact to `~/.mini-agent/memory.md` | `key`, `value` |
| `recall_memory` | Retrieve persistent memory | (none) |
| `spawn_agent` | Spawn sub-agent for isolated subtask (depth ≤ 2) | `task` |

### Dynamic Tools

Place shell scripts in `.agent/tools/` with:
```bash
#@name: tool_name
#@description: What this tool does
#@arg: param1:Description of param1
#@arg: param2:Description of param2

# ... your shell code using $1, $2, etc.
```

Example: `.agent/tools/word_count.sh`
```bash
#!/bin/bash
#@name: word_count
#@description: Count lines, words, chars in a file
#@arg: path:File path to analyze

wc "$1"
```

---

## CLI Options

```
Usage: agent.v8 [options] "<task>"
       agent.v8 --interactive [options]

Options:
  --model MODEL          claude-sonnet-4-5 (default), claude-haiku-4-5, claude-opus-4-5
  --max-turns N          Turn cap (default: 30)
  --budget N             Token budget cap (default: 300000)
  --plan                 Plan mode: no side effects, only simulate tool calls
  --sandbox              Run bash commands under sandbox-exec (macOS)
  --stream               SSE streaming (Anthropic backend only)
  --quiet                Suppress logs, output final text only (for subagents)
  --no-memory            Don't load persistent memory
  --allow-http           Enable http_get tool (disabled by default)
  --interactive / -i     Interactive REPL mode (multi-task conversation)
  --backend B            anthropic (default) | openai
  --api-base URL         Override API base URL (localhost allowed by default)
  --allow-remote-backend Allow non-localhost --api-base (auto-enables sandbox)
  --version              Print version
  --help                 Show help
```

### Interactive Mode Commands
- `exit` or `quit`: Exit REPL
- `/cost`: Show cost breakdown
- `/reset`: Reset token counters

---

## Usage Examples

### Basic Task Execution
```bash
export ANTHROPIC_API_KEY=sk-ant-...
./agent.v8 "Create a README.md for this project explaining what it does"
```

### Interactive Mode
```bash
./agent.v8 --interactive
> Analyze all .c files and report code metrics
> Create unit tests for agent.v8.c
> /cost
> exit
```

### Codebase Exploration
```bash
./agent.v8 "Find all TODO comments in .c files using grep_files"
./agent.v8 "List all test files using glob_files with pattern 'test_*.sh'"
```

### Plan Mode (Dry-Run)
```bash
./agent.v8 --plan "Refactor agent.c to split into modules"
# Shows what tools would be called without executing them
```

### Sandbox Mode
```bash
./agent.v8 --sandbox "Download and analyze https://example.com/data.csv"
# Bash commands run in restricted sandbox-exec environment
```

### With OpenAI-Compatible Backend
```bash
export OPENAI_API_KEY=sk-...
./agent.v8 --backend openai --api-base http://localhost:1234 \
  "Explain how the tool loop works"
```

### HTTP Fetching
```bash
./agent.v8 --allow-http "Fetch https://api.github.com/repos/torvalds/linux and summarize"
```

### Task List Management
```bash
./agent.v8 "Add a todo: implement unit tests. Then list all todos"
./agent.v8 "Mark 'implement unit tests' as done"
```

### Subagent Delegation
```bash
./agent.v8 "Use spawn_agent to research C memory management, then write a guide"
# Spawns isolated subagent with own context
```

### Dynamic Tools
```bash
mkdir -p .agent/tools
cat > .agent/tools/git_status.sh <<'EOF'
#!/bin/bash
#@name: git_status
#@description: Get git repository status
git status --short
EOF
chmod +x .agent/tools/git_status.sh

./agent.v8 "Check git status and commit any changes"
```

---

## Safety Features

### Path Confinement
- All file operations restricted to current working directory
- No `..` (parent directory) or `~` (home directory) traversal
- Absolute paths must be within CWD

### Command Blocking
Dangerous bash patterns are blocked:
- `rm -rf /`, `rm -rf /*`, `rm -rf ~`
- `sudo`, `doas` (privilege escalation)
- `dd if=`, `mkfs`, `fdisk` (disk operations)
- `/etc/shadow`, `/etc/sudoers` (system file tampering)
- `shutdown`, `halt`, `reboot`
- `git push --force origin main` (destructive VCS)
- And more...

### Resource Limits
- **Token budget**: Hard cap on total tokens (default: 300k)
- **Turn limit**: Max turns per task (default: 30)
- **CPU time**: bash executions limited to 60s via `ulimit -t`
- **Output size**: Tool output truncated at 128KB
- **File size**: Memory capped at 16KB, tool output at 32KB
- **HTTP response**: Limited to 32KB

### Audit & Control
- **`.agent/audit.log`**: JSONL log of every tool call (timestamps, PIDs, depth, I/O)
- **`.agent/STOP`**: Emergency kill switch (checked every turn)
- **API key redaction**: All secrets scrubbed from logs/output
- **Spawn depth limit**: Max 2 levels of subagent recursion

### Sandbox Mode
- macOS: Uses `sandbox-exec` with profile restricting filesystem/network access
- Auto-enabled for non-localhost API backends
- Custom sandbox profiles in `.agent/sandbox.sb`

---

## Architecture Notes

### Tool-Use Loop
1. Send user message + system prompt + tools to Claude API
2. Claude responds with text and/or tool_use blocks
3. Execute each tool, collect results
4. Send tool_result blocks back to Claude
5. Repeat until Claude stops calling tools (max 30 turns)

### Prompt Caching
- System prompt: Last block gets `cache_control: ephemeral`
- Tools array: Last tool gets `cache_control: ephemeral`
- Reduces cost by ~90% for tool definitions + system context on cache hits

### Context Compaction
- After 24 messages, summarize oldest N-6 messages via Claude Haiku
- Keeps conversation under token limits for long tasks
- Preserves: key facts, decisions, file changes, pending work, errors

### Streaming (Anthropic Only)
- `--stream` enables SSE (Server-Sent Events) for real-time output
- Parses event stream to reconstruct final JSON response
- Prints assistant text as it arrives

### Cost Tracking
Based on Sonnet 4.5 pricing (per 1M tokens):
- Input: $3.00
- Output: $15.00
- Cache write: $3.75
- Cache read: $0.30

Formula:
```
cost = (in_tokens / 1e6 × $3.00)
     + (out_tokens / 1e6 × $15.00)
     + (cache_write / 1e6 × $3.75)
     + (cache_read / 1e6 × $0.30)
```

---

## Building

### Requirements
- C compiler (gcc, clang)
- libcurl (for HTTP)
- POSIX system (macOS, Linux, BSD)

### Compile
```bash
make agent.v8
```

Or manually:
```bash
cc -O2 -Wall -o agent.v8 agent.v8.c cJSON.c -lcurl -lm
```

### Test
```bash
# Run included test suite (requires agent.v3+)
./eval.sh ./agent.v8
```

---

## Environment Variables

| Variable | Description | Default |
|----------|-------------|---------|
| `ANTHROPIC_API_KEY` | Anthropic API key (required for default backend) | (none) |
| `OPENAI_API_KEY` | OpenAI API key (for `--backend openai`) | (none) |
| `MINI_AGENT_DEPTH` | Current spawn depth (auto-set by spawn_agent) | 0 |
| `MINI_AGENT_ALLOW_REMOTE` | Allow remote --api-base without explicit flag | (none) |

---

## File Structure

```
.
├── agent.v8.c          # Main source (v8)
├── agent.v7.c          # Previous version
├── agent.v3.c          # v3 with caching/memory
├── cJSON.c / cJSON.h   # JSON parser
├── Makefile            # Build system
├── eval.sh             # Test suite
├── README.md           # This file
└── .agent/             # Runtime directory (auto-created)
    ├── tools/          # Dynamic tool scripts (*.sh)
    ├── audit.log       # JSONL tool call log
    ├── todo.md         # Task list (for todo tool)
    ├── sandbox.sb      # macOS sandbox profile (optional)
    └── STOP            # Emergency stop flag
```

---

## Advanced Usage

### Custom Models
```bash
./agent.v8 --model claude-opus-4-5 "Complex reasoning task requiring Opus"
./agent.v8 --model claude-haiku-4-5 "Simple task requiring speed"
```

### Persistent Memory
```bash
# First run
./agent.v8 "My favorite color is blue"
# (agent calls save_memory automatically)

# Later run
./agent.v8 "What's my favorite color?"
# (agent recalls from ~/.mini-agent/memory.md)
```

### Multi-Step Workflows
```bash
./agent.v8 "Create a plan with todo, then execute each step:
1. Analyze codebase with grep_files
2. Generate test file
3. Run tests
Mark each done as you complete it"
```

### Combining Tools
```bash
./agent.v8 --allow-http "
1. Use http_get to fetch https://raw.githubusercontent.com/torvalds/linux/master/MAINTAINERS
2. Use grep_files to find all MAINTAINERS in local repo
3. Compare and report differences
"
```

---

## Troubleshooting

### "ANTHROPIC_API_KEY not set"
```bash
export ANTHROPIC_API_KEY=sk-ant-api03-...
```

### "refused: --api-base is not localhost"
For non-localhost backends:
```bash
./agent.v8 --allow-remote-backend --api-base https://api.example.com ...
```

### "token cap exceeded"
Increase budget:
```bash
./agent.v8 --budget 500000 "large task"
```

### "spawn depth limit reached"
Subagents can only spawn 2 levels deep. Refactor task to avoid deep recursion.

### "http_get is disabled"
Enable with:
```bash
./agent.v8 --allow-http "fetch https://example.com"
```

### Streaming not working
Streaming only works with Anthropic backend:
```bash
./agent.v8 --stream "task"  # ✓ works
./agent.v8 --backend openai --stream "task"  # ✗ ignored
```

---

## Performance Tips

1. **Use grep_files/glob_files** instead of `bash grep` or `bash find` for codebase exploration (faster, safer)
2. **Enable caching** by using same system prompt + tools across runs (automatic in v3+)
3. **Use --quiet** for subagents to reduce noise and speed up spawn_agent
4. **Limit --budget** for cost-sensitive tasks
5. **Use Haiku** (`--model claude-haiku-4-5`) for simple tasks to reduce cost by ~10x

---

## Security Considerations

⚠️ **Warning**: mini-agent-c executes arbitrary commands via the `bash` tool. Always:

- Review tasks before running
- Use `--plan` mode to preview actions
- Never run untrusted agent prompts
- Keep API keys secret (auto-redacted in logs)
- Use `--sandbox` for untrusted code execution
- Disable `--allow-http` unless needed (prevents data exfiltration)
- Review `.agent/audit.log` after sensitive operations
- Set conservative `--budget` and `--max-turns` limits

**Sandbox mode** is recommended for production use:
```bash
./agent.v8 --sandbox --budget 100000 --max-turns 20 "task"
```

---

## Contributing

mini-agent-c is a research project demonstrating minimal agentic AI in C. Contributions welcome:

- **Bug reports**: Open an issue with steps to reproduce
- **Feature requests**: Describe use case and proposed API
- **Pull requests**: Keep changes minimal, maintain C89/C99 compatibility, add tests

---

## License

MIT License - see LICENSE file for details.

---

## Version History

- **v8.0** (2024-04): grep_files, glob_files, http_get, todo, REPL mode, SIGINT handler
- **v7.0** (2024-04): Improved caching, spawn depth tracking
- **v6.0** (2024-04): Max turns, token budget, enhanced error handling
- **v5.0** (2024-04): OpenAI backend, custom API base
- **v4.0** (2024-03): Plan mode, sandbox, cost tracking, streaming, audit log
- **v3.0** (2024-03): Prompt caching, memory, dynamic tools, spawn_agent, compaction
- **v2.0** (2024-03): edit_file tool
- **v1.0** (2024-03): Initial release with basic tool loop

---

## Acknowledgments

- **Anthropic** for the Claude API and prompt caching feature
- **cJSON** library by Dave Gamble
- Inspired by projects like Aider, GPT-Engineer, and Claude Code
