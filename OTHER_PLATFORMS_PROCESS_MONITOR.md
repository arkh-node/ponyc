# Testing the ProcessMonitor redesign on other platforms

This is a temporary handoff document for verifying and finishing the
`ProcessMonitor` exit-detection redesign on macOS, Windows, and the BSDs. It is
**not meant to ship** — delete it before the PR merges.

- Branch: `redesign-process-monitor-exit-detection`
- PR: #5770 (draft)
- Design: discussion #5769
- Closes: #5764, #5748, #5765, #5766

Linux is done and verified (26 process tests pass, counterfactual-checked, and
the full stdlib suite is green). Nothing on Linux needs re-testing. What is not
verified: macOS, the BSDs, and Windows. This document is how to close that gap.

## What the change does (read this first)

`ProcessMonitor` used to decide the child had exited when its stdout and stderr
both reached end-of-file. That is wrong: a pipe reaches end-of-file only when
every process holding its write end has closed it, and a grandchild that
inherited the pipes (or the parent still holding stdin) keeps them open after
the child is gone. So the exit was reported late or never (#5764, #5748).

Now the child's exit is delivered as a native OS event, and that event is the
only signal that the child exited. Pipe end-of-file just means "stop reading
this pipe." The per-platform exit mechanism:

- **Linux**: a `pidfd` (from `pidfd_open`) that goes readable when the child
  exits, registered with epoll as a normal read event. No runtime C change.
- **macOS / BSD**: kqueue `EVFILT_PROC` + `NOTE_EXIT`, keyed on the pid. Needs
  a runtime C change (already written, see below).
- **Windows**: no native event yet. An interim timer poll of `wait()`
  (GetExitCodeProcess) stands in. See the Windows section.

Construction is now a `StartProcess` factory returning `(ProcessMonitor |
ProcessError)` — all fallible setup happens before the actor exists.

## Files changed

Pony (`packages/process/`):
- `process_monitor.pony` — the `StartProcess` factory, the three-state actor
  (`_Running`/`_Disposing`/`_Reaped`), the reap edge and drain, the exit-event
  routing, and the Windows interim timer.
- `_process.pony` — the `_Process` interface (`kill`/`wait`/`arm_exit_event`/
  `close_exit_source`), `_ProcessPosix` (pidfd on Linux, pid for kqueue),
  `_ProcessWindows`, `_WaitPidStatus`.
- `_pipe.pony` — `begin()`/`dispose()` guarded for placeholder pipes.
- `process_error.pony` — new `ExecutableNotFound`, `UnsupportedKernel`;
  docstrings on all variants.
- `process_notify.pony`, `process.pony` — docstrings + example for the factory.
- `_test.pony` — 26 tests.

Runtime (`src/libponyrt/`):
- `asio/asio.h` — `ASIO_PROC = 1 << 5`.
- `asio/kqueue.c` — `EVFILT_PROC` handling in subscribe/dispatch/unsubscribe.
- `lang/process.c` + `.h` — `ponyint_pidfd_open` (Linux only).
- `packages/builtin/asio_event.pony` — `AsioEvent.proc()` mirrors `ASIO_PROC`.

Other:
- `test/full-program-runner/_tester.pony` — updated to the `StartProcess` API.

## How exit detection is wired (so you can navigate)

1. `StartProcess` (in `process_monitor.pony`) validates, creates the four
   pipes, forks/creates the child, and builds the actor via the package-private
   `ProcessMonitor._create`.
2. In `_create`, the actor calls `_child.arm_exit_event(this)` to register the
   native exit event and stores the returned `AsioEventID` in `_exit_event`.
   It then fires `notifier.created` and does a synchronous `_reap_if_exited()`
   probe (covers a child that exited before the event was armed — asio
   subscription is synchronous, so the probe closes the race on every platform).
3. When the exit event fires, `_event_notify` matches `event is _exit_event`
   and calls `_reap_if_exited()`, which calls `_child.wait()` (non-blocking
   reap) and, on a status, runs the reap edge `_do_reap`: drain the pipes,
   close everything (`_close_all` → `_close_exit_source`), `notifier.dispose`.
4. The reap is gated on `_state is _Reaped`, so it runs at most once. That gate
   is load-bearing — removing it double-reaps and aborts the runtime.

The `_Process` interface is the seam. Each platform implements `arm_exit_event`
(register the native event, return its id), `close_exit_source` (unsubscribe +
release), `wait` (non-blocking reap), and `kill`.

## Building and running the process tests

Follow BUILD.md for the full build. Fast iteration on just the process package:

```
# Linux/macOS/BSD, after a normal `cmake --build --preset debug`:
./build/debug/ponyc packages/process --output /tmp --bin-name process_test
/tmp/process_test        # runs all process tests; whole suite is a few seconds
```

```
# Windows (from a dev shell), preset windows-x86-64-debug:
.\build\debug\ponyc.exe packages\process --output . --bin-name process_test
.\process_test.exe
```

Or run the whole stdlib suite (which includes the process package):

```
cmake --build --preset debug
ctest --preset debug -R stdlib-debug --output-on-failure
```

The runtime C changes (`asio.h`, `kqueue.c`, `lang/process.c`) require
rebuilding libponyrt — a normal `cmake --build --preset <preset>` does that.

## The tests, and the testing discipline

The suite is in `packages/process/_test.pony`. Two kinds:

- **Real-process tests** spawn actual programs. Several are posix-only (they
  use `/bin/sh`, `/bin/sleep`, `/dev/zero`) and short-circuit to
  `h.complete(true)` under `ifdef windows`. The cross-platform regression
  tests (STDIN-STDOUT, STDERR, Expect, WritevOrdering, PrintvOrdering, Chdir,
  BadChdir, BadExec, STDIN-WriteBuf, long/kill-running-child) run everywhere
  with per-platform commands (cmd/find/ping on Windows).
- **Seam tests** (`exit-reports-exactly-once`, `dispose-then-exit`,
  `double-dispose`, `no-kill-after-reap`, `write-after-exit-is-dropped`) use an
  injectable spy `_Process` and placeholder pipes — no real process — to pin
  the state-machine invariants. These run on all platforms.

Critical discipline (from the design's Testing section): the #5764/#5748 tests
must separate correct from broken by **time**, not by status. A status-only
assertion (`dispose(Exited(7))` fired) passes even on the buggy code, just
late. So `grandchild-does-not-block-exit` uses `sh -c "sleep 30 & exit 7"` with
a 10s timeout: a pass proves exit was detected before the grandchild died, i.e.
without waiting on the pipes. Keep that discipline for any platform analog.

To confirm a test discriminates, break the assertion (or the code path it
covers) and confirm it fails. Examples already validated on Linux: removing the
`_Reaped` reap gate double-reaps and aborts; a kill-after-reap fails
`no-kill-after-reap`; suppressing the exec/chdir relay fails BadExec/BadChdir.

## macOS and the BSDs (kqueue)

macOS and the BSDs run the **same** `EVFILT_PROC` C. Verifying it on one gives
strong confidence in the others.

The kqueue change (`src/libponyrt/asio/kqueue.c`): an event created with the
`ASIO_PROC` flag (fd = the pid) registers `EVFILT_PROC | NOTE_EXIT` as
`EV_ADD | EV_RECEIPT | EV_ONESHOT`; on `EVFILT_PROC` the dispatch loop delivers
`ASIO_READ` (so the owner reaps, matching the Linux pidfd shape); unsubscribe
does `EV_DELETE` (ENOENT after a fired one-shot is ignored via EV_RECEIPT). The
Pony side is `_ProcessPosix.arm_exit_event`'s `bsd or osx` branch, which creates
the event with `AsioEvent.proc()` and fd = `pid.u32()`.

What to verify:
- The build compiles the kqueue path.
- The whole process suite passes, especially the posix-only tests
  (`grandchild-does-not-block-exit`, `stdin-open-child-exits`,
  `grandchild-delivers-buffered-output`, `child-closes-output-keeps-running`,
  `flooding-grandchild-does-not-stall-teardown`, `many-starts-do-not-leak-fds`)
  — these are the ones that exercise real kqueue exit detection.
- No hang. If the fast-exit case hangs, suspect the probe or the
  EVFILT_PROC-on-a-zombie registration (the probe should cover it since it
  reaps synchronously).

The kqueue `EPIPE`-on-macOS handling already exists in `kevent_receipt_has_error`
— unrelated to this change, but be aware of it.

FreeBSD is being verified locally with a QEMU VM. macOS needs a real machine or
CI (`pr.yml` runs macOS jobs, but only once the PR is marked ready — draft PRs
skip it).

## Windows

Windows is the least-finished platform and needs the most judgment.

### Current state: interim timer poll

`_ProcessWindows.arm_exit_event` returns `AsioEvent.none()` and
`close_exit_source` is a no-op. Instead, `ProcessMonitor._create` starts a
10ms repeating timer (`_start_windows_poll` / `_windows_poll` in
`process_monitor.pony`) that, while running, does the pipe reads
(`_pending_writes`, `_read_pipe` on stdout/stderr/err) and calls
`_reap_if_exited()`. `wait()` on Windows is `GetExitCodeProcess` via
`ponyint_win_process_wait`. Because exit is now read from `wait()` and not from
the pipes, this interim fixes the Windows pipe-close bugs too — it just doesn't
eliminate the poll the way the design intends.

The timer is disposed on every terminal path (`_close_all` →
`_dispose_windows_poll`) and is not started if the construction probe already
reaped the child.

### The design's goal (section D): a native exit event

Move Windows exit detection onto a `RegisterWaitForSingleObject` bridge in the
`sock_notify` IOCP backend (`src/libponyrt/asio/sock_notify.c`), and move the
pipe reads onto asio too, so the poll timer goes away entirely.

The pattern to follow already exists for console stdin: `arm_stdin` (line
~162) calls `RegisterWaitForSingleObject(..., stdin_notify_cb, ...)`;
`stdin_notify_cb` posts a `KEY_STDIN` completion packet; the dispatch loop
turns that into an `ASIO_READ`. An exit event would `RegisterWaitForSingleObject`
on the process **handle** (the comment at sock_notify.c ~148-149 confirms
"processes" are valid wait objects), post a new `KEY_PROC` packet, and deliver
an exit event to the monitor.

**The obstacle** (why this wasn't written blind): the process handle is 64-bit,
but `asio_event_t.fd` is `int` (`src/libponyrt/asio/event.h:20`), so the handle
can't ride in `ev->fd`. It needs a Windows-only path — likely a Windows-only
`HANDLE` field on `asio_event_t` (precedent: the existing Windows-only
`HANDLE timer` field there) plus a dedicated create/subscribe path. This
touches the shared IOCP backend that all Windows networking depends on, so it
needs real Windows build+test, not a blind write.

Decision to make (parked for Sean): keep the interim poll for now, or implement
the native event in this PR. The interim works and fixes the bugs; the native
event is the design's end state.

### Suspected pre-existing bug to check

`ponyint_win_process_wait` (`src/libponyrt/lang/process.c` ~132-160) returns
`GetLastError()` on `WAIT_FAILED`. If that error is `1` (ERROR_INVALID_FUNCTION),
`_ProcessWindows.wait` reads `1` — which is also its "still running" sentinel —
and the interim timer would then re-poll a closed handle forever. This is
pre-existing, but the interim timer drives that function repeatedly, so it may
now be reachable. Worth confirming under Windows and fixing with a distinct
still-running sentinel if it bites.

### What to verify on Windows

- The build compiles (`cmake --preset windows-x86-64-debug` then build).
- The cross-platform process tests pass: STDIN-STDOUT, STDERR, Expect,
  WritevOrdering, PrintvOrdering, Chdir, BadChdir, BadExec, STDIN-WriteBuf,
  long-running-child, kill-long-running-child, wait-on-closed-process-twice,
  and the seam tests.
- No hang, no spin. Watch CPU: the interim poll is a 10ms timer, so idle CPU
  should be near zero once the child exits and the monitor is reaped.
- The posix-only tests short-circuit to pass on Windows; that is expected. If
  you want real Windows coverage of the grandchild/stdin-open cases, write
  Windows analogs (cmd-based) that keep the same time-discrimination
  discipline — e.g. a child that backgrounds a long-lived process inheriting
  its stdout and exits, with a timeout well under the background process's life.

### Windows test-command reference (already in `_test.pony`)

- `cat` → `find.exe /v ""`; `sleep N` → `ping.exe 127.0.0.1 -n N`;
  `echo` / `pwd` / stderr → `cmd.exe /c ...`. See `_CatCommand`, `_SleepPath`,
  `_EchoPath`, `_PwdPath`/`_PwdArgs`, and the `ifdef windows` arms in each test.

## Reporting back

For each platform: whether it builds, which tests pass/fail (with output),
whether anything hangs or spins, and — for Windows — the decision on interim
vs. native and whether the `ponyint_win_process_wait` sentinel bug is real.
Keep the time-discrimination discipline for any new exit-detection test.
