# mini-agent-wrapper (Rust crate)

Call `mini-agent-c` from your Rust app as an isolated subprocess.

## Why

- **Budget isolation**: main Rust app doesn't burn tokens if the agent loops
- **No SDK bloat**: main Rust app doesn't depend on Anthropic SDK
- **Sandboxed**: every call runs under path confinement + denylist + optional seatbelt
- **Auditable**: every tool call logged to `.agent/audit.log` in the agent's CWD
- **Crash isolation**: agent crash can't take down the parent

## Quickstart

```rust
use mini_agent_wrapper::Agent;

let agent = Agent::new("./agent.v4")
    .budget(20_000)
    .max_turns(8)
    .sandbox(true);
let answer = agent.run("compute 17*23 with bash and return the number only")?;
// answer == "391"
```

## Examples

```bash
# From this directory (examples/4-rust-subprocess/)
export ANTHROPIC_API_KEY=sk-ant-...

cargo run --example nl_task          # delegate NL task
cargo run --example health_check     # sandboxed health probe
```

Both examples auto-locate `agent.v4` by walking up from CWD.

## Real-world fit

Inside **claudeterm / miseban-ai / chatweb.ai** (Rust axum servers), you can:

```rust
#[axum::debug_handler]
async fn handle_nl_query(Json(req): Json<Query>) -> impl IntoResponse {
    let agent = Agent::new("/opt/bin/agent.v4")
        .budget(30_000).max_turns(10).sandbox(true).no_memory(true);
    // Run in blocking pool so the async runtime isn't blocked
    let result = tokio::task::spawn_blocking(move || agent.run(&req.prompt))
        .await.unwrap()?;
    Json(ApiResponse { answer: result })
}
```

The main server stays lean; every user query is a fresh, budget-capped agent process.
