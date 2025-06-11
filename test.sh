#!/bin/bash

# Exit immediately if a command exits with a non-zero status.
set -e
TEST_FAILED=0

# Define variables
EXECUTABLE="./myfs_test"
DISK_IMAGE="test_disk.img"
DISK_SIZE_BYTES="10485760" # 10MB
TEST_COMMANDS_FILE="test_commands.txt"
OUTPUT_FILE="output.txt"
EXPECTED_OUTPUT_FILE="expected_output.txt" # Will be created later
HOST_TEST_FILE="host_file.txt"
HOST_TEST_FILE_BACK="host_file_back.txt"

# Cleanup function
cleanup() {
    echo "Cleaning up..."
    rm -f "$EXECUTABLE" "$DISK_IMAGE" "$TEST_COMMANDS_FILE" "$OUTPUT_FILE" "$EXPECTED_OUTPUT_FILE" "$HOST_TEST_FILE" "$HOST_TEST_FILE_BACK"
    echo "Cleanup complete."
}

# Trap EXIT signal to ensure cleanup runs
trap cleanup EXIT

# Compile the C program
echo "Compiling myfs.c..."
if gcc -Wall -Werror -o "$EXECUTABLE" myfs.c; then
    echo "Compilation successful."
else
    echo "Compilation failed."
    exit 1
fi

# Create a dummy disk image if it doesn't exist, or if mkfs is part of the commands
# For now, we'll always remove and recreate for a clean state.
echo "Removing old disk image if it exists..."
rm -f "$DISK_IMAGE"

echo "Creating host test file..."
echo "Hello from host_file.txt" > "$HOST_TEST_FILE"

echo "Preparing test commands..."
cat > "$TEST_COMMANDS_FILE" << EOF
# Initial setup: create disk if needed
y
$DISK_SIZE_BYTES

# === Initialization & Basic Structure ===
df # Check initial disk usage

# === Directory Operations ===
mkdir /dir1
ls / # Should show dir1
cd /dir1
pwd # Should be /dir1
mkdir /dir1/subdir1
ls # Should show subdir1 (in /dir1)
ls /dir1 # Should show subdir1
cd /dir1/subdir1
pwd # Should be /dir1/subdir1
cd /
pwd # Should be /

# === File Operations (Part 1 - Create, Write, Read) ===
cp-to $HOST_TEST_FILE /dir1/file1.txt
ls /dir1 # Should show file1.txt with its size

# === File Operations (Part 2 - Append, Truncate) ===
append /dir1/file1.txt 10
ls /dir1 # Check new size
truncate /dir1/file1.txt 5
ls /dir1 # Check new size
truncate /dir1/file1.txt 10000 # Truncate to 0
ls /dir1 # Check size is 0

# === Link Operations ===
ln /dir1/file1.txt /link1 # Link to the (now empty) file1.txt
ls / # Should show link1
ls /dir1 # Should show file1.txt

# === Removal Operations (leading up to full cleanup) ===
rm /link1
ls / # Verify link1 is gone
# Attempt to copy back the file - it should be empty now
# We'll add the actual file content check in the assertion step
cp-from /dir1/file1.txt $HOST_TEST_FILE_BACK


# === Error Handling and Edge Cases (Partial - more can be added in assertion step) ===
# Recreate dir1 and file1.txt for error tests
mkdir /dir2 # Using dir2 to avoid conflict if dir1 cleanup failed for some reason
cp-to $HOST_TEST_FILE /dir2/error_test_file.txt
rmdir /dir2 # Attempt to remove non-empty directory (should show error)
cd /dir2/error_test_file.txt # Attempt to cd into a file (should show error)
rm /non_existent_file # Attempt to remove non-existent file (should show error)
rmdir /non_existent_dir # Attempt to remove non-existent dir (should show error)

# === Final Cleanup within VFS before exit ===
# This part is crucial to test rm and rmdir properly
rm /dir2/error_test_file.txt
rmdir /dir2

# Try to remove the first set of dirs/files if they weren't fully cleaned by earlier tests
# For file1.txt, it should be empty due to truncate.
rm /dir1/file1.txt
rmdir /dir1/subdir1 # This rmdir should work
rmdir /dir1 # This rmdir should work now

df # Check disk usage at the end

exit
EOF

echo "Running $EXECUTABLE with $DISK_IMAGE..."
# The program expects 'y' then size if disk not found, then commands.
# Stderr is redirected to stdout to capture all output in one file.
if "$EXECUTABLE" "$DISK_IMAGE" < "$TEST_COMMANDS_FILE" > "$OUTPUT_FILE" 2>&1; then
    echo "Execution finished. Output captured in $OUTPUT_FILE"
else
    echo "Execution of $EXECUTABLE returned a non-zero exit code or failed to run. Output captured in $OUTPUT_FILE"
    cat "$OUTPUT_FILE" # Show output if the program crashed.
    TEST_FAILED=1 # Mark as failure if the program itself fails.
fi

# --- Assertion Logic ---
echo "Comparing output with expected output..."
if diff -q "$EXPECTED_OUTPUT_FILE" "$OUTPUT_FILE"; then
    echo "SUCCESS: Main output matches expected output."
else
    echo "FAILURE: Main output does not match expected output."
    echo "Showing diff:"
    diff -u "$EXPECTED_OUTPUT_FILE" "$OUTPUT_FILE" || true # Show diff
    TEST_FAILED=1
fi

# Check content of the file copied back from vdisk AFTER truncate operations
echo "Verifying content of $HOST_TEST_FILE_BACK (expected to be empty)..."
if [ -f "$HOST_TEST_FILE_BACK" ] && [ ! -s "$HOST_TEST_FILE_BACK" ]; then
    echo "SUCCESS: $HOST_TEST_FILE_BACK is empty as expected."
else
    echo "FAILURE: $HOST_TEST_FILE_BACK is not empty or does not exist."
    if [ -f "$HOST_TEST_FILE_BACK" ]; then
        echo "Content of $HOST_TEST_FILE_BACK:"
        cat "$HOST_TEST_FILE_BACK"
    else
        echo "$HOST_TEST_FILE_BACK not found."
    fi
    TEST_FAILED=1
fi

echo "Test script finished." # Replaces any old placeholder message

# Final status check
if [ "$TEST_FAILED" -eq 1 ]; then
    echo ""
    echo "!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!"
    echo "!!! ONE OR MORE TESTS FAILED !!!"
    echo "!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!"
    exit 1
else
    echo ""
    echo "******************************"
    echo "*** ALL TESTS PASSED! YAY! ***"
    echo "******************************"
    exit 0
fi
