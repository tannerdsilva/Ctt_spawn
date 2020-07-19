# Ctt_spawn
`Ctt_spawn` is a software product **for Linux** that re-imagines the traditional `posix_spawn` system call. `Ctt_spawn` was conceived to be the driving function of a soon-to-be-released Swift Library known as `SwiftSlash`.

`Ctt_spawn` is a Swift Package contining C code with one public function: `tt_spawn_core`. This is the function that will launch external processes, as `posix_spawn` would do.

Unlike `posix_spawn`, `tt_spawn_core` offers precise control over far more attributes of the spawning process when compared to `posix_spawn`. These attributes are as follows:

- The ability to bind STDIN, STDOUT, and STDERR data channels of the spawned process to a file handle in the current process. 
This is done by means of an intermediary process known as the `tt_spawn_container_proc` which facilitates many of the security and control features of `tt_spawn_core`.

- The ability to securely specify the working directory of the spawned process without the need to compromise the working directory of the current process.

- Detaches the spawned process from its parents file handles before beginning the primary workload, closing a potential security risk between the two proceses.

- Independent (per instance) control over the environment variables of the spawned process. If no environment variables are specified, `tt_spawn_core` will still clear the spawned process of all environment variables before execution. **Environment variables are assigned, never inherited, from the process that initiates the spawned workload.**

The fundamental element of `tt_spawn_core` that enables this functionality and control (without the need to compromise or modify the initiating process's environment) is the `tt_spawn_cotainer_proc`. This process is designed as an imbedded system (of sorts), and helps facilitate and manage the lifecycle of the spawned process.