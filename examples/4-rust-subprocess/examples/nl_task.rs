//! Natural-language task delegation from Rust.
//! Run: `cargo run --example nl_task`
//!
//! Demonstrates: the wrapper lib spawning an isolated agent subprocess,
//! receiving the final answer as a String.

use mini_agent_wrapper::Agent;

fn main() -> anyhow::Result<()> {
    // Assume binary is two dirs up (examples/4-rust-subprocess/examples → ../..)
    let here = std::env::current_dir()?;
    let binary = here.ancestors()
        .find(|p| p.join("agent.v4").exists())
        .ok_or_else(|| anyhow::anyhow!("agent.v4 not found in any parent dir"))?
        .join("agent.v4");

    let agent = Agent::new(&binary)
        .budget(15_000)
        .max_turns(6)
        .no_memory(true);

    let task = "\
        Using bash, compute the sum of the first 100 natural numbers. \
        Respond with ONLY the number, nothing else.";

    println!("[main] delegating task to isolated agent at {:?}", binary);
    let result = agent.run(task)?;
    println!("[main] agent returned: {:?}", result);

    // Verify
    if result.trim() == "5050" {
        println!("✅ correct");
        Ok(())
    } else {
        anyhow::bail!("wrong answer: {:?}", result);
    }
}
