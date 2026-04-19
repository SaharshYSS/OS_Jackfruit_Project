# OS-Jackfruit — Mini Container Runtime

A lightweight container runtime built from scratch using core Operating Systems concepts. Inspired by how Docker works under the hood, this project demonstrates process isolation, resource control, kernel-level memory monitoring, and inter-process communication — all implemented in C on Linux.

---
## Authors
Yerramilli Siva Sai Saharsh PES2UG24CS618  
Yadunandana Reddy M PES2UG24CS605

---
## What This Project Does

OS-Jackfruit lets you spin up isolated containers where processes run in their own environment, with separate filesystems and controlled resource limits. A central supervisor manages all containers, handles their lifecycle, and collects logs — all through a simple CLI.

---

## Features

- Isolated containers using Linux namespaces and `chroot`
- Central supervisor process managing multiple containers concurrently
- CLI commands: `run`, `start`, `stop`, `ps`, `logs`
- Logging pipeline built on a producer–consumer model with a bounded buffer
- UNIX domain socket-based IPC between the CLI and supervisor
- Loadable kernel module (`monitor.ko`) that tracks per-container memory usage
- Soft limit → warning via `dmesg`; Hard limit → process termination
- Clean container lifecycle: created → running → exited

---

## Architecture

```
┌─────────────────────────────────────────────────┐
│                   CLI (engine)                  │
│   run | start | stop | ps | logs                │
└───────────────────┬─────────────────────────────┘
                    │  UNIX Domain Socket
                    ▼
┌─────────────────────────────────────────────────┐
│              Supervisor Process                 │
│  - Manages container records                    │
│  - Spawns isolated child processes              │
│  - Handles logging via bounded buffer           │
│  - Communicates with kernel module via ioctl    │
└───────────────────┬─────────────────────────────┘
                    │  ioctl
                    ▼
┌─────────────────────────────────────────────────┐
│           Kernel Module (monitor.ko)            │
│  - Registers containers with PID               │
│  - Tracks RSS memory usage                      │
│  - Enforces soft + hard memory limits           │
│  - Exposes /dev/container_monitor               │
└─────────────────────────────────────────────────┘
```

---

## File Structure

```
boilerplate/
├── engine.c              # User-space runtime: supervisor + CLI dispatcher
├── monitor.c             # Kernel module: memory tracking and enforcement
├── monitor_ioctl.h       # Shared ioctl interface (user ↔ kernel)
├── Makefile              # Build system
├── cpu_hog.c             # CPU-intensive workload
├── memory_hog.c          # Memory-intensive workload
├── io_pulse.c            # I/O workload
├── logs/                 # Container log output directory
├── rootfs-alpha/         # Rootfs for alpha containers
├── rootfs-beta/          # Rootfs for beta containers
└── rootfs-base/          # Base rootfs with busybox utilities
```

---

## Build Instructions

Install dependencies:
```bash
sudo apt update && sudo apt install -y gcc build-essential linux-headers-$(uname -r)
```

Build everything:
```bash
make
```

Load the kernel module:
```bash
sudo insmod monitor.ko
lsmod | grep monitor
ls -l /dev/container_monitor
```

---

## Running the System

### 1. Start the Supervisor
```bash
sudo ./engine supervisor ../rootfs-base
```

### 2. Start Containers (new terminal)
```bash
sudo ./engine run test ../rootfs-alpha /bin/sh
sudo ./engine start cpu ../rootfs-alpha /cpu_hog
sudo ./engine start io ../rootfs-beta /io_pulse
```

### 3. List All Containers
```bash
sudo ./engine ps
```

### 4. View Container Logs
```bash
sudo ./engine logs cpu
```

---

## Memory Limit Enforcement

```bash
sudo dmesg -C
sudo ./engine start mem ../rootfs-alpha /memory_hog
sleep 2
sudo dmesg | tail
```

Expected kernel output:
```
[container_monitor] Registering container=mem pid=XXXX soft=... hard=...
[container_monitor] SOFT LIMIT container=mem pid=XXXX rss=... limit=...
[container_monitor] HARD LIMIT container=mem pid=XXXX rss=... limit=...
[container_monitor] Unregister request container=mem pid=XXXX
```

- **Soft limit** — logs a warning to the kernel ring buffer
- **Hard limit** — sends `SIGKILL` to the container process

---

## Scheduling Demo

```bash
sudo ./engine run test ../rootfs-alpha /bin/ls
sudo ./engine start alpha ../rootfs-alpha /bin/ls
sudo ./engine start beta ../rootfs-beta /bin/ls
sudo ./engine ps
```

This demonstrates multiple containers running concurrently with different workload profiles, showing the scheduler handling CPU-bound and I/O-bound processes simultaneously.

---

## Clean Teardown

```bash
ps aux | grep engine
sudo pkill -9 engine
ps aux | grep engine
```

Unload the kernel module:
```bash
sudo rmmod monitor
```

---

## OS Concepts Demonstrated

| Concept | Where |
|---|---|
| Process isolation | `chroot` + Linux namespaces in `engine.c` |
| Inter-process communication | UNIX domain sockets (CLI ↔ supervisor) |
| Synchronization | Bounded buffer with mutex + condition variables |
| Producer–consumer | Log reader thread + logging thread |
| Kernel-user interaction | `ioctl` calls to `/dev/container_monitor` |
| Memory resource management | Soft/hard limits enforced by kernel module |
| Scheduling | Multiple concurrent containers with different workloads |
| Atomic operations | Temp-file + rename for log writes |

---

## Build/CI Check

```bash
make ci
```

---

## Notes

- Use **Ubuntu 22.04** for compatibility with the kernel module
- The `logs/` directory must exist before starting containers: `mkdir -p logs`
- Workload binaries (`/cpu_hog`, `/io_pulse`, `/memory_hog`) must be present inside the respective rootfs directory
- Always start the supervisor before running any container commands


