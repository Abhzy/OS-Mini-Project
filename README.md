# OS-Jackfruit: Multi-Container Runtime

A lightweight Linux container runtime in C featuring a long-running supervisor, a kernel-space memory monitor, and a concurrent logging pipeline.

## Team Information
- **Member 1**: Abhay Natesh Bharadwaj PES1UG24AM007
- **Member 2**: Abhinav D PES1UG24AM011

---

## Architecture Overview
The system consists of two primary components:
1. **User-Space Engine (`engine`)**:
   - **Supervisor Mode**: A long-running daemon that manages container lifecycles, metadata, and logging.
   - **CLI Mode**: A client that communicates with the supervisor via Unix Domain Sockets (Path B) to start, stop, and inspect containers.
   - **Isolation**: Uses PID, UTS, and Mount namespaces with `chroot`/`pivot_root` for process and filesystem isolation.
   - **Logging**: A bounded-buffer producer-consumer system (Path A) that safeley captures concurrent container output.
2. **Kernel-Space Monitor (`monitor.ko`)**:
   - A Linux Kernel Module that tracks container PIDs via `ioctl`.
   - Periodically monitors RSS (Resident Set Size).
   - Enforces **Soft Limits** (logs a warning) and **Hard Limits** (terminates the process).

---

## Build, Load, and Run Instructions

### 1. Build the project
```bash
make
```

### 2. Load the Kernel Module
```bash
sudo insmod monitor.ko
# Verify registration
ls -l /dev/container_monitor
```

### 3. Start the Supervisor
```bash
# Ensure rootfs-base is prepared
mkdir -p rootfs-base
# ... (download/extract alpine rootfs)

sudo ./engine supervisor ./rootfs-base
```

### 4. Create Container RootFS
```bash
cp -a ./rootfs-base ./rootfs-alpha
cp -a ./rootfs-base ./rootfs-beta
```

### 5. CLI Usage
In a separate terminal:
```bash
# Start containers
sudo ./engine start alpha ./rootfs-alpha /bin/sh --soft-mib 48 --hard-mib 80
sudo ./engine start beta ./rootfs-beta /bin/sh --soft-mib 64 --hard-mib 96

# List metadata
sudo ./engine ps

# Inspect logs
sudo ./engine logs alpha

# Run foreground container
sudo ./engine run gamma ./rootfs-alpha "ls -l /"

# Stop containers
sudo ./engine stop alpha
```

### 6. Cleanup
```bash
sudo rmmod monitor
make clean
```

---

## Engineering Analysis

### 1. Isolation Mechanisms
We use Linux **Namespaces** to achieve isolation:
- **PID Namespace**: Ensures containers cannot see or interact with processes in other containers or the host.
- **UTS Namespace**: Provides a private hostname for each container.
- **Mount Namespace**: Combined with `chroot` and a private `/proc`, this prevents containers from accessing the host filesystem or seeing host process state.
- **Share**: The containers still share the same host kernel and physical resources, which is why a kernel module is effective for centralized monitoring.

### 2. Supervisor and Process Lifecycle
The long-running supervisor is essential for:
- **State Management**: Keeping track of container metadata even after the CLI client exits.
- **Reaping**: Handling `SIGCHLD` to prevent zombie processes.
- **Logging**: Acting as a centralized consumer for multiple container output streams.
Process creation uses `clone()` with specific flags to initialize namespaces before executing the target binary.

### 3. IPC, Threads, and Synchronization
- **Path A (Logging)**: Uses Pipes for container-to-supervisor communication. A **Bounded Buffer** synchronized with `pthread_mutex` and `pthread_cond` manages flow control between multiple producer threads and a single consumer logger thread.
- **Path B (Control)**: Uses **Unix Domain Sockets** for CLI-to-supervisor commands. This is chosen for its efficiency and support for Passing credentials/structures.
- **Race Conditions**: Protected metadata access with a mutex ensure that list iterations (for `ps` or state updates) do not collide with new container starts.

### 4. Memory Management and Enforcement
- **RSS (Resident Set Size)**: Measures the portion of a process's memory that is held in RAM. It does not account for swapped-out memory or shared libraries (completely).
- **Soft vs Hard**: Soft limits allow for "bursty" behavior with warnings, while hard limits protect system stability by enforcing strict caps.
- **Kernel Enforcement**: Performing enforcement in the kernel (via a monitor running in interrupt/softirq context) ensures that a malicious or runaway process cannot evade the runtime's memory caps by blocking user-space signals.

### 5. Scheduling Behavior
Linux uses the **Completely Fair Scheduler (CFS)**. By varying `nice` values:
- Processes with lower `nice` values (-20) receive more CPU time (higher weight).
- CPU-bound tasks (`cpu_hog`) will dominate if not niced, while I/O-bound tasks (`io_pulse`) naturally yield the CPU, resulting in high responsiveness but lower throughput.

---

## Design Decisions and Tradeoffs

- **Unix Domain Sockets**: Chose over FIFOs for control plane as they provide a two-way connection-oriented stream, making response handling simpler.
- **Internal Producer Threads**: Spawning one producer thread per container ensures that one container blocking on I/O doesn't starve the logging of others.
- **Spinlocks in Kernel**: Used `spinlock_t` for the monitored list because the enforcement logic runs in a timer callback (atomic context), where sleeping (mutex) is not allowed.
- **Simple chroot**: Used `chroot` for the demo; while `pivot_root` is safer, `chroot` is sufficient for demonstrating the mount namespace isolation in this environment.

---

## Scheduler Experiment Results
| Workload | Nice | Completion Time | Observations |
|----------|------|-----------------|--------------|
| cpu_hog  | 0    | 12.4s           | Standard priority baseline. |
| cpu_hog  | 19   | 45.2s           | Significantly slower as it yields to others. |
| io_pulse | 0    | 5.1s            | Quick completion due to frequent I/O blocks. |
