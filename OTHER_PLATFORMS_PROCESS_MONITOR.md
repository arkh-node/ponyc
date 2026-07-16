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
- **Windows**: the child's process handle registered with the `sock_notify`
  IOCP backend via `RegisterWaitForSingleObject`; the exit arrives as an asio
  event. The child's pipe reads move onto the same backend. See the Windows
  section. (An earlier interim timer poll was replaced by this native event.)

Construction is now a `StartProcess` factory returning `(ProcessMonitor |
ProcessError)` — all fallible setup happens before the actor exists.

## Files changed

Pony (`packages/process/`):
- `process_monitor.pony` — the `StartProcess` factory, the three-state actor
  (`_Running`/`_Disposing`/`_Reaped`), the reap edge and drain, the exit-event
  routing, and the exit-signal reap retry (`_reap_on_exit_signal`/`_reap_again`)
  that covers `waitpid` trailing the OS exit notification.
- `_process.pony` — the `_Process` interface (`kill`/`wait`/`arm_exit_event`/
  `close_exit_source`), `_ProcessPosix` (pidfd on Linux, pid for kqueue),
  `_ProcessWindows`, `_WaitPidStatus`.
- `_pipe.pony` — `begin()`/`dispose()` guarded for placeholder pipes.
- `process_error.pony` — new `ExecutableNotFound`, `UnsupportedKernel`;
  docstrings on all variants.
- `process_notify.pony`, `process.pony` — docstrings + example for the factory.
- `_test.pony` — 31 tests.

Runtime (`src/libponyrt/`):
- `asio/asio.h` — `ASIO_PROC = 1 << 5`.
- `asio/kqueue.c` — `EVFILT_PROC` handling in subscribe/dispatch/unsubscribe,
  and the ESRCH-on-subscribe → `ASIO_READ` mapping for an already-exited child.
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
   and calls `_reap_on_exit_signal(0)`, which calls `_child.wait()` (non-blocking
   reap) and, on a status, runs the reap edge `_do_reap`: drain the pipes, close
   everything (`_close_all` → `_close_exit_source`), `notifier.dispose`. If
   `wait()` still reports the child running (the OS exit signal can lead
   `waitpid`), it retries via the `_reap_again` self-message, bounded by
   `_reap_retry_cap`. The start-up probe in step 2 uses `_reap_if_exited`, which
   does not retry — there the child may genuinely still be running.
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

There is a second kqueue path, added after the macOS run below found the race it
handles: if the child has already exited by the time the asio thread registers
the filter, `EV_ADD EVFILT_PROC` fails with ESRCH — the kernel has no process to
watch. That is the exit, not a failure to watch for it, so the subscribe maps an
ESRCH receipt on an `ASIO_PROC` event to `ASIO_READ` (the same signal
`NOTE_EXIT` delivers) instead of `ASIO_ERROR`. The monitor then retries its
non-blocking reap (`_reap_on_exit_signal` → `_reap_again`), because `waitpid`
can briefly still report the exiting child as running after the kernel has
signalled the exit.

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

**macOS is verified, after a fix.** The assumption that macOS would pass because
it runs the same `EVFILT_PROC` code as FreeBSD was wrong. macOS returns ESRCH
from `EV_ADD EVFILT_PROC` for a child that exits the instant it is forked (the
`BadExec`/`BadChdir` tests), which the old code turned into `ASIO_ERROR` and a
non-draining error path that lost the child's exec/chdir error byte and reported
`WaitpidError` — an intermittent failure that surfaced within a few full-suite
runs. The ESRCH→`ASIO_READ` mapping plus the reap retry (above) fix it. With the
fix, 31 process tests pass, stable across 20 full-suite runs, and the whole
stdlib suite is green. FreeBSD did **not** show this race in the pre-fix run,
which is why it was missed.

**FreeBSD needs a re-run against the fix, and one specific probe.** The earlier
FreeBSD result — all 26 process tests pass on FreeBSD 15.1 (clang 19.1.7),
stable across repeated runs — was against the code *before* this fix. Re-run the
suite against the fixed code. The fix maps only ESRCH, because that is the errno
macOS returns; a BSD kernel could return a different errno for the same
instant-fork race, in which case it would fall through to the old non-retrying
error path and the bug would remain there. So, on each BSD, probe explicitly:
does `EV_ADD EVFILT_PROC` for a child that exits the instant it forks return an
error, and if so what errno? If it is ESRCH, the fix already covers it. If it is
another errno, extend the ESRCH branch in `kqueue.c` with that verified value —
do not guess. If the race does not occur there at all (registration wins the
race), nothing more is needed.

One portability fix came out of the FreeBSD run: the fd-leak test used
`/bin/true`, which is Linux-only; it now uses `/usr/bin/true`, which exists on
Linux, the BSDs, and macOS. If you add tests, prefer `/usr/bin/true` and check
any hardcoded `/bin/...` path exists on your platform.

## Windows

The native exit event is implemented; it needs a real Windows build+test run.

### Current state: native exit event

`_ProcessWindows.arm_exit_event` creates an asio event carrying the child's
process handle (in the event's `nsec` slot, since the handle is 64-bit and the
event's `fd` is `int`). On the asio thread the `sock_notify` IOCP backend
registers a wait on that handle with `RegisterWaitForSingleObject`; when the
child exits, a thread-pool callback posts a `KEY_PROC` completion packet and the
dispatch loop delivers an `ASIO_READ` to the monitor, the same shape as the
Linux pidfd. The child's pipe reads and stdin write-retries moved onto the same
backend (Windows pipes carry no readiness signal, so the backend peeks them each
wait cycle), so the per-child poll timer is gone. `wait()` is
`GetExitCodeProcess` via `ponyint_win_process_wait`, which no longer closes the
handle — the backend closes it once, after it unregisters the wait
(`close_exit_source` → the backend's `ASIO_PROC` unsubscribe).

New runtime C: `src/libponyrt/asio/sock_notify.c` (the `ASIO_PROC`/`ASIO_PIPE`
handling, the owned-event list, the `RegisterWaitForSingleObject` wait, the
`KEY_PROC` dispatch), `asio.h` (`ASIO_PIPE = 1 << 6`), `event.h`/`event.c` (the
Windows-only `proc_wait` HANDLE and `next` link fields), and `lang/process.c`
(`ponyint_win_process_wait` returns a fixed `-1` error sentinel and no longer
closes the handle).

This code was reviewed for handle/wait/event lifecycle and for the thread-pool-
callback-vs-unsubscribe race; both came back clean (no leak, double-free,
use-after-free, dropped or doubled exit). The exit is always re-confirmed
against the OS on the actor side (`_child.wait()` behind the `_Reaped` gate), so
the C-side packet matching only needs to be memory-safe, which it is. It has not
been run on Windows — that is what remains.

### The earlier sentinel bug (fixed)

`ponyint_win_process_wait` used to return `GetLastError()` on a wait failure,
which could be `1` — the same value as its "still running" result — and stall
the reap. It now returns a fixed `-1` sentinel that the two cases can never
share.

### Two review notes (Low, non-blocking)

- The `sock_notify.c` comments that oversold the IOCP-FIFO ordering argument
  were softened to lean on the actual backstop (the membership invariant for
  memory safety, the idempotent `wait()`-gated reap for harmlessness). Comment
  only; done.
- If `pony_asio_event_create` ever returned NULL in `arm_exit_event`, the
  process handle would leak (the Linux path closes its fd unconditionally; the
  Windows path delegates the close to the backend). Left as-is on purpose: it is
  effectively unreachable (`pony_asio_event_create` returns NULL only for bad
  flags / a missing msg_id, and the monitor always has one), and closing the
  handle here would need a new `CloseHandle` FFI and a Windows-only branch that
  can't be tested from a non-Windows machine — untested code guarding an
  impossible case. If a real Windows run ever shows it reachable, add the
  defensive close then.

### What to verify on Windows

- The build compiles (`cmake --preset windows-x86-64-debug` then build).
- The cross-platform process tests pass: STDIN-STDOUT, STDERR, Expect,
  WritevOrdering, PrintvOrdering, Chdir, BadChdir, BadExec, STDIN-WriteBuf,
  long-running-child, kill-long-running-child, wait-on-closed-process-twice,
  and the seam tests. Two Windows-specific tests were added
  (`windows-empty-environment`, `windows-grandchild-does-not-block-exit`).
- No hang, no spin. There is no per-child poll timer anymore, so idle CPU
  should be near zero.
- The posix-only tests short-circuit to pass on Windows; that is expected.
  `windows-grandchild-does-not-block-exit` is the Windows analog of the #5764
  grandchild case; keep the time-discrimination discipline (timeout well under
  the background process's life) for any further exit-detection tests.

### Windows test-command reference (already in `_test.pony`)

- `cat` → `find.exe /v ""`; `sleep N` → `ping.exe 127.0.0.1 -n N`;
  `echo` / `pwd` / stderr → `cmd.exe /c ...`. See `_CatCommand`, `_SleepPath`,
  `_EchoPath`, `_PwdPath`/`_PwdArgs`, and the `ifdef windows` arms in each test.

## Reporting back

For each platform: whether it builds, which tests pass/fail (with output), and
whether anything hangs or spins. Keep the time-discrimination discipline for any
new exit-detection test.

Status so far: macOS is verified after the ESRCH fix — 31 process tests pass,
stable across 20 full-suite runs, full stdlib green. Linux passes all process
tests at the current tip. FreeBSD (15.1) passed the *pre-fix* code (26 tests
then); it needs a re-run against the fix, with the explicit ESRCH-errno probe
described in the macOS/BSD section — the instant-fork race may or may not occur
there, and if it does with a non-ESRCH errno the fix must be extended. Windows
needs its first real build+test run.
