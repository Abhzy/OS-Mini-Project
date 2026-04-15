# Multi-Container Runtime (Jackfruit)

A lightweight Linux container runtime in C with a long-running supervisor and a kernel-space memory monitor.

## Team Information
- **Abhay Natesh Bharadwaj** (SRN: PES1UG24AM007)
- **Abhinav D** (SRN: PES1UG24AM011)

---

## Build and Run Instructions

### 1. Prerquisites
Ensure you are on an Ubuntu 22.04/24.04 VM with Secure Boot OFF.
Install dependencies:
```bash
sudo apt update
sudo apt install -y build-essential linux-headers-$(uname -r)
```

### 2. Setup Root Filesystem
Run the provided setup script to download and prepare the Alpine minirootfs:
```bash
chmod +x setup-rootfs.sh
./setup-rootfs.sh
```

### 3. Build the Project
```bash
make all
```

### 4. Load Kernel Module
```bash
sudo insmod monitor.ko
# Verify control device
ls -l /dev/container_monitor
```

### 5. Start Supervisor
```bash
# Start the supervisor in one terminal
sudo ./engine supervisor ./rootfs-base
```

### 6. Using the CLI (In another terminal)
```bash
# Create per-container writable rootfs copies
cp -a ./rootfs-base ./rootfs-alpha
cp -a ./rootfs-base ./rootfs-beta

# Start containers
sudo ./engine start alpha ./rootfs-alpha "echo hello from alpha && sleep 60"
sudo ./engine run beta ./rootfs-beta "ls /proc"

# Check status
sudo ./engine ps

# Check logs
sudo ./engine logs alpha

# Stop container
sudo ./engine stop alpha
```

### 7. Cleanup
```bash
sudo rmmod monitor
make clean
```

---

## Engineering Analysis

### 1. Isolation Mechanisms
This runtime achieves isolation using Linux **Namespaces**:
- **PID Namespace**: Isolates process IDs so the container sees itself as PID 1.
- **UTS Namespace**: Isolates hostname.
- **Mount Namespace**: Provides a private mount view, allowing us to change the root filesystem.
The project uses `chroot` and `mount("proc", ...)` to ensure the container sees only its assigned filesystem and its own processes. The host kernel is still shared, meaning all containers share the same kernel instance and resources not explicitly isolated.

### 2. Supervisor and Process Lifecycle
A long-running supervisor is essential for managing the lifecycle of multiple containers concurrently. It handles:
- **Process Creation**: Using `clone()` with namespace flags.
- **Parent-Child Relationships**: The supervisor remains the parent, allowing it to reap exited containers (`SIGCHLD`) and avoid zombie processes.
- **Metadata Tracking**: Maintains a list of active containers, their PIDs, and states.

### 3. IPC, Threads, and Synchronization
- **Control Plane**: Uses a **Unix Domain Socket** (`/tmp/mini_runtime.sock`) for reliable communication between the CLI client and the supervisor.
- **Logging Plane**: Uses **Pipes** and a **Bounded Buffer**.
- **Synchronization**:
    - **Mutexes**: Protect the shared container metadata and the bounded buffer head/tail.
    - **Condition Variables**: Used in the bounded buffer to wake up the logger thread (consumer) when data is available, or the producer when space is available.
- **Race Avoidance**: Locking ensures that multiple containers logging simultaneously do not corrupt the shared buffer, and that metadata updates (like `ps` or `stop`) are atomic.

### 4. Memory Management and Enforcement
- **RSS (Resident Set Size)**: Measures the portion of memory occupied by a process that is held in RAM. It does not measure swapped-out memory or shared libraries (completely).
- **Soft vs Hard Limits**:
    - **Soft Limit**: A "warning" threshold. When exceeded, the kernel monitor logs an event but allows the process to continue.
    - **Hard Limit**: A "fatal" threshold. When exceeded, the kernel monitor forcefully terminates the process (`SIGKILL`).
- **Kernel Enforcement**: Moving enforcement to the kernel ensures that even if the user-space supervisor is busy or compromised, the resource limits are strictly and accurately enforced at the scheduling level.

### 5. Scheduling Behavior
The runtime allows setting `nice` values via the `--nice` flag. 
- Higher nice values (lower priority) result in less CPU time when competing with lower nice value processes.
- **CPU-bound** workloads (like `cpu_hog`) will be significantly affected by priority differences, whereas **I/O-bound** workloads (like `io_pulse`) might be less affected due to frequent voluntary yielding of the CPU.

---

## Design Decisions and Tradeoffs

1. **Unix Domain Sockets for Control**: Chosen for its simplicity and reliability over FIFOs or shared memory. *Tradeoff*: Requires a filesystem path and management of the socket file.
2. **Spinlocks in Kernel Monitor**: Used because the monitoring happens in a timer callback (atomic context) where sleeping is not allowed. *Tradeoff*: High CPU usage if the lock is held for too long (minimized by short critical sections).
3. **Pipes for Logging**: Each container's stdout/stderr is redirected to a pipe. *Tradeoff*: Buffer limits in pipes can cause blocking if the supervisor doesn't read fast enough (mitigated by the multi-threaded logger).
4. **Alpine Linux Rootfs**: Used as a base template due to its extremely small size (~5MB). *Tradeoff*: Limited tools available by default compared to Ubuntu, but sufficient for OS experiments.
