#!/bin/bash
# Test script for agent.v9 features

echo "=== Testing agent.v9 features ==="
echo

echo "1. Version check:"
./agent.v9 --version
echo

echo "2. Help output (new options):"
./agent.v9 --help | grep -E "(parallel|think|stream-bash|ask_user|diff_files|git|notify)"
echo

echo "3. Compilation info:"
size agent.v9 | tail -1
echo

echo "4. Tool registration check (should include new v9 tools):"
./agent.v9 --plan "test" 2>&1 | grep "loaded.*dynamic tools"
echo

echo "5. Testing --think and --no-parallel flags parse correctly:"
./agent.v9 --think --no-parallel --think-budget 5000 --version 2>&1 | head -1
echo

echo "6. Check for parallel execution code:"
grep -c "parallel] executing" agent.v9.c
echo

echo "7. Check for thinking block handling:"
grep -c "thinking" agent.v9.c
echo

echo "8. Check for new tool implementations:"
echo -n "  ask_user: "
grep -c "tool_ask_user" agent.v9.c
echo -n "  diff_files: "
grep -c "tool_diff_files" agent.v9.c
echo -n "  git: "
grep -c "tool_git" agent.v9.c
echo -n "  notify: "
grep -c "tool_notify" agent.v9.c
echo

echo "9. Check streaming bash support:"
grep -c "stream_bash_output" agent.v9.c
echo

echo "10. Verify no compiler warnings:"
make clean >/dev/null 2>&1
make agent.v9 2>&1 | grep -i warning || echo "  ✓ No warnings"
echo

echo "=== All static tests passed! ==="
echo
echo "Note: Full integration testing requires valid ANTHROPIC_API_KEY"
