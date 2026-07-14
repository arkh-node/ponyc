# Suspend-and-drain stress engine

Drives the arena allocator's suspend-and-drain path with real
schedulers. Memory is allocated on every scheduler thread, the threads
scale down and suspend, and the memory is then freed by actors running
on the one remaining active scheduler — foreign frees whose owners are
asleep. The suspended owners must take the memory back through their
drain wakes without any of them coming active for work.

## Phases and what each asserts

1. **Allocate** — `--allocators` actors spread across all schedulers,
   each building `--blocks` arrays of `--block-size` bytes (the default
   512 KiB makes every block an immediate-decommit span, so its return
   is deterministic) and sending them to one collector.
2. **Scale down** — the allocators go idle; the collector polls
   `pony_active_schedulers` until it reaches `--min-schedulers`.
3. **Drain** — the collector drops every block and each allocator runs
   its garbage collection from the active scheduler. The collector
   polls `VmRSS` until `--reclaim-fraction` of the payload has
   returned, asserting on every poll that the active count never rose:
   a drain wake must not fake activity.
4. **Scale up** — fresh parallel work is spawned; the count must rise,
   proving suspended threads still activate after drain episodes. This
   asserts activation-after-draining; the precise scale-up-lands-mid-
   drain interleaving cannot be timed from Pony code and is covered by
   the runtime's unit tests and the sanitizer runs instead.

Exit 0 with a phase-by-phase report, or 1 naming the failed phase.

## Running

Build (debug keeps the runtime's assertions on):

    ponyc --debug test/rt-stress/suspend-drain -o /tmp

Run with a scheduler floor for phase 2 to reach, and Linux for the
VmRSS read:

    /tmp/suspend-drain --ponyminthreads 1 --min-schedulers 1

Any scheduler count works; more schedulers means more suspended owners.
The polls are bounded, so a hang in the runtime shows up as a phase
failure, not a stuck process.
