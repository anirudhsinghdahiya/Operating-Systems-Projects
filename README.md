# XV6 Operating System Projects

## Overview
A series of projects extending and modifying the XV6 educational operating system, progressing from basic system calls to advanced concepts like memory management and filesystem implementation. These projects were completed as part of UW-Madison's Operating Systems course (CS537).

## Project Progression

### 1. Basic System Programming (Project 1)
- Letter Boxed puzzle solver demonstrating C systems programming
- Focus on file I/O and string manipulation
- Error handling and memory management in C

### 2. System Call Implementation (Project 2)
- Added new system call `getparentname` to XV6
- Modified kernel to handle parent-child process relationships
- Implemented system call argument passing and validation

### 3. Shell Implementation (Project 3)
Developed a UNIX-like shell (wsh - Wisconsin Shell) featuring:
- Process creation and management using fork/exec
- I/O redirection (>, <, >>)
- Environment variables and shell variables
- Built-in commands (cd, exit, history)
- Command history with configurable size

### 4. Process Scheduler (Project 4)
Enhanced XV6 scheduler with:
- Dynamic stride scheduling algorithm
- Ticket-based priority system
- Process runtime tracking
- Performance monitoring system calls

### 5. Memory Management (Project 5)
Advanced memory subsystem improvements:
- Memory mapping (wmap/wunmap) implementation
- Copy-on-write fork optimization
- Page fault handler
- Memory protection based on ELF segments

### 6. File System Implementation (Project 6)
FUSE-based filesystem with:
- RAID 0/1 support
- Basic file operations
- Directory management
- Block-based storage system

## Repository Structure
Each project maintains its original implementation and testing structure from the course:
```bash
├── Project-1/  # Letter Boxed
├── Project-2/  # System Calls
├── Project-3/  # Shell
├── Project-4/  # Scheduler
├── Project-5/  # Memory Management
└── Project-6/  # File System
```
## Key Learning Outcomes
- Operating System Internals
- System Programming in C
- Process & Memory Management
- File Systems & Storage
- Concurrent Programming
- System Call Implementation

## Running the Projects
Each project directory contains the original source code and can be built following standard XV6 build procedures. Specific instructions for building and testing are preserved in each project's directory.

## Skills Demonstrated
- Low-level C programming and system implementation
- Operating system kernel modification
- Memory and process management
- File system design and implementation  
- System call design and implementation
- Concurrent programming and synchronization
- Performance optimization and monitoring
- Testing and debugging system software

## Note
These projects are academic implementations meant to demonstrate understanding of operating system concepts. They were completed as part of CS537 at UW-Madison.

## License
MIT License - See LICENSE file for details
