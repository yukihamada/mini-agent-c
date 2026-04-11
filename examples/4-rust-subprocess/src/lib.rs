//! mini-agent-wrapper: safely delegate a task to mini-agent-c from Rust.
//!
//! Usage:
//! ```no_run
//! use mini_agent_wrapper::{Agent, AgentError};
//!
//! let agent = Agent::new("./agent.v4")
//!     .budget(20_000)
//!     .max_turns(10)
//!     .sandbox(true);
//! let answer = agent.run("数値 123 と 456 を足して結果だけ返せ")?;
//! println!("{}", answer);
//! # Ok::<(), AgentError>(())
//! ```

use std::path::{Path, PathBuf};
use std::process::{Command, Stdio};
use std::time::Duration;
use thiserror::Error;

#[derive(Debug, Error)]
pub enum AgentError {
    #[error("spawn failed: {0}")]
    Spawn(#[from] std::io::Error),
    #[error("agent exited with code {code}: {stderr}")]
    NonZero { code: i32, stderr: String },
    #[error("agent killed by signal")]
    Killed,
    #[error("missing ANTHROPIC_API_KEY")]
    NoApiKey,
}

/// A sandboxed wrapper around the `mini-agent-c` binary.
///
/// Each call is an isolated process; token budget is enforced by the agent,
/// path confinement and dangerous-command denylist are always on.
pub struct Agent {
    binary: PathBuf,
    model: Option<String>,
    budget: u32,
    max_turns: u32,
    sandbox: bool,
    plan_mode: bool,
    no_memory: bool,
    cwd: Option<PathBuf>,
    timeout: Option<Duration>,
}

impl Agent {
    pub fn new(binary: impl AsRef<Path>) -> Self {
        Self {
            binary: binary.as_ref().to_path_buf(),
            model: None,
            budget: 30_000,
            max_turns: 15,
            sandbox: false,
            plan_mode: false,
            no_memory: false,
            cwd: None,
            timeout: Some(Duration::from_secs(300)),
        }
    }

    pub fn model(mut self, m: impl Into<String>) -> Self { self.model = Some(m.into()); self }
    pub fn budget(mut self, n: u32) -> Self { self.budget = n; self }
    pub fn max_turns(mut self, n: u32) -> Self { self.max_turns = n; self }
    pub fn sandbox(mut self, on: bool) -> Self { self.sandbox = on; self }
    pub fn plan_mode(mut self, on: bool) -> Self { self.plan_mode = on; self }
    pub fn no_memory(mut self, on: bool) -> Self { self.no_memory = on; self }
    pub fn cwd(mut self, p: impl AsRef<Path>) -> Self { self.cwd = Some(p.as_ref().to_path_buf()); self }
    pub fn timeout(mut self, d: Duration) -> Self { self.timeout = Some(d); self }

    /// Run the agent on a task. Returns the final assistant text (the `--quiet` output).
    pub fn run(&self, task: &str) -> Result<String, AgentError> {
        if std::env::var("ANTHROPIC_API_KEY").is_err() {
            return Err(AgentError::NoApiKey);
        }
        let mut cmd = Command::new(&self.binary);
        cmd.arg("--quiet")
            .arg("--max-turns").arg(self.max_turns.to_string())
            .arg("--budget").arg(self.budget.to_string());
        if let Some(m) = &self.model {
            cmd.arg("--model").arg(m);
        }
        if self.sandbox   { cmd.arg("--sandbox"); }
        if self.plan_mode { cmd.arg("--plan"); }
        if self.no_memory { cmd.arg("--no-memory"); }
        if let Some(d) = &self.cwd { cmd.current_dir(d); }
        cmd.arg(task);
        cmd.stdin(Stdio::null())
           .stdout(Stdio::piped())
           .stderr(Stdio::piped());

        let mut child = cmd.spawn()?;
        // Simple timeout: poll wait. For production use a proper timeout crate.
        let start = std::time::Instant::now();
        let output;
        loop {
            match child.try_wait()? {
                Some(_) => {
                    output = child.wait_with_output()?;
                    break;
                }
                None => {
                    if let Some(timeout) = self.timeout {
                        if start.elapsed() > timeout {
                            let _ = child.kill();
                            return Err(AgentError::Killed);
                        }
                    }
                    std::thread::sleep(Duration::from_millis(200));
                }
            }
        }
        if !output.status.success() {
            return Err(AgentError::NonZero {
                code: output.status.code().unwrap_or(-1),
                stderr: String::from_utf8_lossy(&output.stderr).to_string(),
            });
        }
        Ok(String::from_utf8_lossy(&output.stdout).trim().to_string())
    }
}
