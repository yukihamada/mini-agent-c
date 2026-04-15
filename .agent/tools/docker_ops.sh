#!/bin/bash
#@name: docker_ops
#@description: Docker container management — ps, logs, start, stop, exec, images
# Usage: docker_ops <action> [args...]
# Actions:
#   ps                       — list running containers
#   logs <name>              — tail last 100 lines of container logs
#   start <name>             — start a stopped container
#   stop <name>              — stop a running container
#   exec <name> <cmd...>     — run a command inside a container
#   images                   — list local Docker images

set -euo pipefail

ACTION="${1:-}"

# Check docker is installed and daemon is running
if ! command -v docker &>/dev/null; then
    echo "ERROR: docker is not installed." >&2
    exit 1
fi

if ! docker info &>/dev/null 2>&1; then
    echo "ERROR: Docker daemon is not running. Start Docker and retry." >&2
    exit 1
fi

case "$ACTION" in
    ps)
        docker ps --format "table {{.Names}}\t{{.Image}}\t{{.Status}}\t{{.Ports}}"
        ;;
    logs)
        NAME="${2:-}"
        if [ -z "$NAME" ]; then
            echo "ERROR: container name required. Usage: docker_ops logs <name>" >&2
            exit 1
        fi
        docker logs --tail 100 "$NAME"
        ;;
    start)
        NAME="${2:-}"
        if [ -z "$NAME" ]; then
            echo "ERROR: container name required. Usage: docker_ops start <name>" >&2
            exit 1
        fi
        docker start "$NAME"
        echo "Started container: $NAME"
        ;;
    stop)
        NAME="${2:-}"
        if [ -z "$NAME" ]; then
            echo "ERROR: container name required. Usage: docker_ops stop <name>" >&2
            exit 1
        fi
        docker stop "$NAME"
        echo "Stopped container: $NAME"
        ;;
    exec)
        NAME="${2:-}"
        if [ -z "$NAME" ]; then
            echo "ERROR: container name required. Usage: docker_ops exec <name> <cmd...>" >&2
            exit 1
        fi
        shift 2
        if [ $# -eq 0 ]; then
            echo "ERROR: command required. Usage: docker_ops exec <name> <cmd...>" >&2
            exit 1
        fi
        docker exec "$NAME" "$@"
        ;;
    images)
        docker images --format "table {{.Repository}}\t{{.Tag}}\t{{.Size}}\t{{.CreatedSince}}"
        ;;
    "")
        echo "ERROR: action required." >&2
        echo "Usage: docker_ops <action> [args...]" >&2
        echo "Actions: ps, logs <name>, start <name>, stop <name>, exec <name> <cmd...>, images" >&2
        exit 1
        ;;
    *)
        echo "ERROR: unknown action '$ACTION'." >&2
        echo "Valid actions: ps, logs, start, stop, exec, images" >&2
        exit 1
        ;;
esac
