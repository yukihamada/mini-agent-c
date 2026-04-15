#!/bin/bash
#@name: github_ops
#@description: GitHub operations via gh CLI: list/create PRs, issues, check CI status, view repos
#@arg: command:pr-list|pr-create|pr-view|issue-list|issue-create|ci-status|repo-list
#@arg: args:Command-specific arguments
set -e

CMD="$1"
shift

# Load GitHub token if not set
if [[ -z "$GITHUB_TOKEN" ]]; then
    export GITHUB_TOKEN=$(grep "^GITHUB_TOKEN" ~/.env 2>/dev/null | cut -d= -f2- | tr -d '"')
fi

GH=/opt/homebrew/bin/gh

case "$CMD" in
    pr-list)
        REPO="${1:-}"
        if [[ -n "$REPO" ]]; then
            $GH pr list --repo "yukihamada/$REPO"
        else
            $GH pr list
        fi
        ;;
    pr-create)
        # args: --title "..." --body "..." [--repo name]
        $GH pr create "$@"
        ;;
    pr-view)
        $GH pr view "$@"
        ;;
    issue-list)
        REPO="${1:-}"
        if [[ -n "$REPO" ]]; then
            $GH issue list --repo "yukihamada/$REPO"
        else
            $GH issue list
        fi
        ;;
    issue-create)
        $GH issue create "$@"
        ;;
    ci-status)
        REPO="${1:-}"
        if [[ -n "$REPO" ]]; then
            $GH run list --repo "yukihamada/$REPO" --limit 5
        else
            $GH run list --limit 5
        fi
        ;;
    repo-list)
        $GH repo list yukihamada --limit 20
        ;;
    *)
        echo "Usage: github_ops.sh <command> [args...]"
        echo "Commands: pr-list, pr-create, pr-view, issue-list, issue-create, ci-status, repo-list"
        exit 1
        ;;
esac
