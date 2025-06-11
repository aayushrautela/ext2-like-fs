#!/bin/bash
set -e

# --- Configuration ---
EXECUTABLE="./myfs_test"
DISK_IMAGE="test_disk.img"
DISK_SIZE_BYTES="10485760"
C_SOURCE_FILE="myfs.c"
LOG_FILE="test_run.log"
HOST_TEST_FILE="host_file.txt"
TEST_FAILED=0

# --- Helper Function ---
run_and_log() {
    local description="$1"
    local command_to_run="$2"
    local dir_to_show="${3:-/}"

    # Log the state before running the command
    echo "State Before: ls $dir_to_show" >> "$LOG_FILE"
    printf "ls %s\nexit\n" "$dir_to_show" | "$EXECUTABLE" "$DISK_IMAGE" | sed 's/^/    /' >> "$LOG_FILE"
    echo "" >> "$LOG_FILE"

    # Log the command being run
    echo "Test Description: $description" >> "$LOG_FILE"
    echo "Command: $command_to_run" >> "$LOG_FILE"
    
    # Run the command and capture its output
    output=$(printf "%s\nexit\n" "$command_to_run" | "$EXECUTABLE" "$DISK_IMAGE" 2>&1)
    
    echo "Command Output:" >> "$LOG_FILE"
    echo "$output" | sed 's/^/    /' >> "$LOG_FILE"

    # Check for errors and log the status
    if echo "$output" | grep -q -E "Error:|failed|cannot|not found"; then
        echo "Status: FAILURE" >> "$LOG_FILE"
        TEST_FAILED=1
    else
        echo "Status: SUCCESS" >> "$LOG_FILE"
    fi

    echo "--------------------------------------------------" >> "$LOG_FILE"
    echo "" >> "$LOG_FILE"
}

# --- Cleanup Function ---
cleanup() {
    echo "Cleaning up generated files..."
    # FIXED: Do not delete the log file, so the user can inspect it.
    rm -f "$EXECUTABLE" "$DISK_IMAGE" "$HOST_TEST_FILE"
}
trap cleanup EXIT

# --- Main Script ---

# 1. Compilation
echo "Compiling..."
gcc -Wall -Werror -o "$EXECUTABLE" "$C_SOURCE_FILE"

# 2. Disk Creation
echo "Creating disk..."
# Delete old log and disk to ensure a clean run
rm -f "$DISK_IMAGE" "$LOG_FILE"
printf "y\n%s\n" "$DISK_SIZE_BYTES" | "$EXECUTABLE" "$DISK_IMAGE" > /dev/null

# 3. Host File Creation
echo "Hello from the host file!" > "$HOST_TEST_FILE"

# 4. Run Command Sequence and Log Everything
echo "Running tests and generating human-readable log..."

run_and_log "Initial df" "df"
run_and_log "mkdir /dir1" "mkdir /dir1" "/"
run_and_log "mkdir /dir1/subdir" "mkdir /dir1/subdir" "/dir1"
run_and_log "cp-to /dir1/file1.txt" "cp-to $HOST_TEST_FILE /dir1/file1.txt" "/dir1"
run_and_log "append to /dir1/file1.txt" "append /dir1/file1.txt 10" "/dir1"
run_and_log "truncate /dir1/file1.txt" "truncate /dir1/file1.txt 5" "/dir1"
run_and_log "ln /dir1/file1.txt /link1" "ln /dir1/file1.txt /link1" "/"
run_and_log "rm /link1" "rm /link1" "/"
run_and_log "rm /dir1/file1.txt" "rm /dir1/file1.txt" "/dir1"
run_and_log "rmdir /dir1/subdir" "rmdir /dir1/subdir" "/dir1"
run_and_log "rmdir /dir1" "rmdir /dir1" "/"


# --- Final Output ---
echo ""
if [ "$TEST_FAILED" -eq 1 ]; then
    echo "!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!"
    echo "!!! ONE OR MORE TESTS FAILED !!!"
    echo "!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!"
    echo "Check 'test_run.log' for details."
    exit 1
else
    echo "******************************"
    echo "*** ALL TESTS PASSED! YAY! ***"
    echo "******************************"
    echo "Log created at: test_run.log"
fi
