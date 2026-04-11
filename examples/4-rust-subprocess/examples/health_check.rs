//! Health-check sub-worker from a main Rust service.
//! Real-world pattern: a long-running Rust server delegates a periodic
//! "is everything OK?" check to an isolated agent so it never blocks the main loop.
//!
//! Run: `cargo run --example health_check`

use mini_agent_wrapper::Agent;
use std::time::Duration;

fn main() -> anyhow::Result<()> {
    let here = std::env::current_dir()?;
    let repo_root = here.ancestors()
        .find(|p| p.join("agent.v4").exists())
        .ok_or_else(|| anyhow::anyhow!("agent.v4 not found"))?
        .to_path_buf();
    let binary = repo_root.join("agent.v4");

    let agent = Agent::new(&binary)
        .budget(20_000)
        .max_turns(8)
        .sandbox(true)
        .cwd(&repo_root)    // sandbox.sb lives here
        .timeout(Duration::from_secs(120))
        .no_memory(true);

    let task = "\
        Check the following: \
        1) is this machine's current CPU load below 8.0? Use bash uptime. \
        2) Can you reach https://api.anthropic.com (use bash curl -sSo /dev/null -w '%{http_code}')? \
        Respond with exactly one line: OK <load> <status>, or FAIL <reason>.";

    println!("[service] spawning isolated health check...");
    match agent.run(task) {
        Ok(out) => {
            println!("[service] result: {}", out);
            if out.starts_with("OK") {
                std::process::exit(0);
            } else {
                eprintln!("[service] UNHEALTHY");
                std::process::exit(1);
            }
        }
        Err(e) => {
            eprintln!("[service] agent failed: {}", e);
            std::process::exit(2);
        }
    }
}
