## Fix Windows programs never finishing when stdin has ended

On Windows, a program that subscribed to stdin after stdin had ended could hang forever at 0% CPU. The new subscription never received a read, so the end of the input never reached the program, and the program never exited. A program run with its input redirected from `NUL` had a matching problem: the end of its input never arrived either, and it spun at 100% CPU instead of exiting.

A Windows program reading redirected stdin now reaches the end of its input the way it does on Linux and macOS.

## Fix Windows programs stalling while reading stdin from a pipe

On Windows, a read of stdin from a pipe did not return until the program on the other end of the pipe wrote a byte or closed the pipe. With one scheduler thread, nothing else in the program ran in the meantime: no timer fired and no other actor ran. With the default number of scheduler threads, the rest of the program kept running, but `env.input.dispose()` had no effect until the other end wrote a byte or closed the pipe, so the program kept reading stdin and could not exit.

The rest of a Windows program now runs while it waits for input on stdin, and `env.input.dispose()` takes effect when you call it, as it already did on Linux and macOS. Console input, stdin redirected from a file, and stdin from NUL were never affected.

## Report a process's exit even when its pipes stay open

`ProcessMonitor` reported a child's exit status only once the child's stdout and stderr had both reached end-of-file. A pipe reaches end-of-file only when every process holding its write end has closed it, so a child that left a grandchild holding its stdout or stderr open, or that exited while you still held its stdin open, was reported late or never, and the program could hang. The exit is now detected directly, from the operating system, and reported promptly regardless of what still holds the pipes.

## ProcessMonitor.dispose no longer risks signaling an unrelated process

Disposing a `ProcessMonitor` after its child had already exited could send a signal to whatever process the operating system had since assigned the child's old process id. It no longer signals a child that has been reaped.

## ProcessMonitor no longer leaks file descriptors when a process fails to start

A process that failed to start left the pipes that had been opened for it open. They are now closed.

## Starting a process returns a result

Starting a process now returns either a `ProcessMonitor` or a `ProcessError`, instead of always giving you a monitor. Replace the constructor with `StartProcess`:

Before:

```pony
let pm = ProcessMonitor(sp_auth, bp_auth, consume notifier, path, args, vars)
```

After:

```pony
match StartProcess(sp_auth, bp_auth, consume notifier, path, args, vars)
| let pm: ProcessMonitor => // a live child is running
| let err: ProcessError  => // never started; err says why
end
```

Failures that used to arrive through `ProcessNotify.failed` — no execute permission, a missing executable, and, on Linux, a kernel older than 5.3 — are now returned by `StartProcess` instead. The `ExecveError` that meant both "the file is missing" and "execve failed in the child" is split; the missing-file case is now `ExecutableNotFound`.

## ProcessMonitor requires Linux 5.3 or newer

On Linux, detecting a child's exit now uses `pidfd_open`, which requires kernel 5.3 or newer. On an older kernel, `StartProcess` returns an `UnsupportedKernel` error rather than starting a process it cannot monitor.
