# Agent Evolution Summary - v2

## Successfully Implemented Improvements

### 1. New `list_dir` Tool
- **Function**: `tool_list_dir(const char *path)` 
- **Purpose**: Lists all files and directories in a given directory path
- **Implementation**: Uses POSIX `opendir()` and `readdir()` APIs
- **Integration**: Added to tool schema and execute_tool dispatcher
- **Updated system prompt** to mention the new tool

### 2. Token Usage Tracking
- **Added variables**: `total_input_tokens` and `total_output_tokens`
- **Extraction**: Parse `usage` object from API response to get per-turn token counts
- **Logging**: Enhanced stderr logging with detailed token information:
  - Per-turn tokens (input and output)
  - Cumulative totals across all turns
  - Format: `[tokens] turn=X input=Y output=Z | total_input=A total_output=B`
- **Final summary**: Enhanced done/halt messages to include total token usage

### 3. API Retry Logic for Overloaded Errors
- **Constants**: `MAX_RETRIES=3`, `RETRY_SLEEP_SECONDS=2`
- **Detection**: Parse API response to check for `"type": "overloaded_error"`
- **Behavior**: 
  - Retry up to 3 times on overloaded errors
  - Sleep 2 seconds between retries
  - Log retry attempts to stderr
  - Don't retry on other error types or JSON parse failures
- **Smart retry**: Only retries on `overloaded_error`, not on other API errors

## Test Results

✅ Compilation: Success (no warnings)
✅ list_dir tool: Successfully listed current directory files
✅ write_file tool: Created evolved.txt with "v2 works"
✅ Token tracking: Visible in stderr output (turn 0: 828 in, 130 out; turn 1: 1076 in, 103 out)
✅ Total tokens: 2137 tokens (1904 input, 233 output)

## Backward Compatibility

All original functionality preserved:
- read_file, write_file, bash tools unchanged
- Original API call flow maintained
- Existing command-line interface unchanged
- All original features work as before

## Files Created

- agent.v2.c - Evolved source code
- agent.v2 - Compiled binary
- evolved.txt - Test output file
- EVOLUTION_SUMMARY.md - This summary
