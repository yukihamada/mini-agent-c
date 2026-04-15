#!/bin/bash
#@name: m5_exec
#@description: Execute a command on the M5 Mac (192.168.0.47) which has MLX LLM inference server on port 5001
#@arg: command:Shell command to run on M5 Mac (quoted string)
set -e

CMD="$*"
if [[ -z "$CMD" ]]; then
    echo "Usage: m5_exec.sh <command>"
    echo ""
    echo "The M5 Mac at 192.168.0.47 runs MLX inference (OpenAI-compatible API on port 5001)"
    echo "Available models: Qwen3.5-122B-A10B-4bit, 35B-A3B-4bit, 27B-4bit, 9B-4bit, gemma-4-31b-it-4bit"
    echo ""
    echo "Examples:"
    echo "  m5_exec.sh 'ps aux | grep mlx'"
    echo "  m5_exec.sh 'curl -s http://localhost:5001/v1/models | python3 -m json.tool'"
    echo "  m5_exec.sh 'df -h /'"
    exit 1
fi

# Try SSH (LAN only)
ssh -o ConnectTimeout=5 -o StrictHostKeyChecking=no yukihamada@192.168.0.47 "$CMD"
