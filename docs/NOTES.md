We *cannot* support:
    - static executables cannot use this approach.
    - dynamic executables that use syscall instructions directly.

We *will* support:
    - loading via LD_PRELOAD, DT_NEEDED or /etc/ld.so.preload.
    - we will never break anything when loaded.
    - static executables (or dynamic binaries not using our shim) interacting with us.
        - this means non-cloexec file descriptors inherited or passed to children will be correct.

We *strive* to support:
    - exotic, multithreaded processes.
    - zero or low overhead for anything we can just pass to the kernel.
        - no promises if we need to polyfill something, slow is better than broken.
