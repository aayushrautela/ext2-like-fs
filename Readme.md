## Custom Filesystem in C

This project implements a simple, Unix-like filesystem in C. It provides a command-line interface to interact with a virtual disk file, allowing users to perform common filesystem operations like creating directories, copying files, managing links, and more.

---
## How to Build and Run

### Prerequisites

* A C compiler (like GCC)
* Standard Linux utilities (`bash`, `rm`, `stat`, etc.)

### Building

The filesystem code is contained in `myfs.c`. To compile it, use GCC:

```bash
gcc -Wall -o myfs myfs.c
```

This will create an executable file named `myfs`.

### Running

1.  **Virtual Disk:** The filesystem operates on a virtual disk file. If you don't have one (e.g., `disk.img`), the program will prompt you to create it when you run it for the first time.
    ```bash
    ./myfs disk.img
    ```
    If `disk.img` doesn't exist, it will ask:
    ```
    Virtual disk file 'disk.img' not found. Create it? (y/n):
    ```
    Enter `y` and then specify the desired size in bytes (e.g., `10485760` for 10MB).

2.  **Interactive Shell:** Once the virtual disk is ready, you'll be greeted with the `vfs>` prompt. You can type `help` to see a list of available commands.

3.  **Exiting:** To exit the filesystem shell, type `exit` or `quit`.

---
## Available Commands

The filesystem shell supports the following commands:

| Command          | Syntax                              | Description                                                                                             |
| :--------------- | :---------------------------------- | :------------------------------------------------------------------------------------------------------ |
| **`ls`** | `ls [path]`                         | Lists the contents of the specified directory. If no path is given, it lists the current directory.     |
| **`cd`** | `cd <path>`                         | Changes the current working directory to the specified path. `cd /` returns to the root.                |
| **`pwd`** | `pwd`                               | Prints the full path of the current working directory.                                                  |
| **`mkdir`** | `mkdir <path>`                      | Creates a new directory at the specified path.                                                          |
| **`rmdir`** | `rmdir <path>`                      | Removes an **empty** directory.                                                                         |
| **`cp-to`** | `cp-to <host_path> <vdisk_path>`    | Copies a file from your computer's filesystem (host) into the virtual disk.                             |
| **`cp-from`** | `cp-from <vdisk_path> <host_path>`  | Copies a file from the virtual disk back to your computer's filesystem.                                 |
| **`rm`** | `rm <path>`                         | Removes a file or a hard link.                                                                          |
| **`ln`** | `ln <target> <link_name>`           | Creates a hard link named `link_name` that points to the `target` file.                                 |
| **`append`** | `append <path> <bytes>`             | Appends a specified number of null bytes to the end of a file, increasing its size.                     |
| **`truncate`** | `truncate <path> <bytes>`           | Shortens a file by a specified number of bytes from the end. If bytes >= file size, truncates to 0.      |
| **`df`** | `df`                                | Displays disk usage information, including inode and data block usage.                                  |
| **`help`** | `help`                              | Shows a list of all available commands.                                                                 |
| **`exit`** | `exit` or `quit`                    | Exits the program.                                                                                      |

---
## Testing

The project includes a test script `test.sh` to provide a structured way to verify the filesystem's functionality.

### Running the Tests

To run the tests, simply execute the script from your terminal:

```bash
bash test.sh
```

### Test Script Overview

The `test.sh` script performs the following actions:

1.  **Compilation:** It compiles `myfs.c` into an executable named `myfs_test`.
2.  **Disk Creation:** It creates a fresh 10MB virtual disk image named `test_disk.img` for each test run.
3.  **Command Execution:** It runs a predefined sequence of filesystem commands against the virtual disk.
4.  **Human-Readable Logging:** All operations are logged to `test_run.log`. For each operation, the script logs the state of the relevant directory **before** and **after** the command, making it easy to see the effect of each step.
5.  **Status Checking:** The script checks the output of each command for keywords like "Error," "failed," or "cannot." It logs a `SUCCESS` or `FAILURE` status for each operation.
6.  **Final Summary:** At the end, it reports a summary of the test run to the console.

### Test Case Breakdown

The `test.sh` script methodically tests the filesystem's features in the following order:

| Test Case              | Description                                                                                              |
| :--------------------- | :------------------------------------------------------------------------------------------------------- |
| **Initial State** | Runs `df` and `ls /` to verify the initial state of a newly formatted disk.                              |
| **Directory Operations** | Tests `mkdir` by creating `/dir1` and a nested `/dir1/subdir`, verifying the directory structure with `ls` at each step. |
| **File Creation** | Tests `cp-to` by copying a host file into `/dir1/file1.txt` and confirms its existence.                  |
| **File Modification** | Tests `append` and `truncate` on `/dir1/file1.txt` to ensure the file size is updated correctly.       |
| **Linking** | Tests `ln` by creating a hard link (`/link1`) to a file and verifies it appears in the root directory's listing. |
| **Removal** | Tests `rm` on the hard link, then on the original file. Finally, it tests `rmdir` on the now-empty directories to ensure the cleanup is successful. |
