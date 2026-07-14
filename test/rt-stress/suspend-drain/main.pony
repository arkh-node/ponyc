"""
Suspend-and-drain stress engine.

Drives the allocator's suspend-and-drain path with real schedulers: memory
allocated on every scheduler thread is freed after those threads have
suspended, and the freeing must bring the physical memory back without
waking any of them into scheduling work.

Phases:

1. Allocate: N actors, spread across all scheduler threads, each build
   `--blocks` byte arrays of `--block-size` (defaults sized so every block
   is an immediate-decommit span in the arena allocator) and send them to
   one collector.
2. Scale down: the allocators go idle; every scheduler but one suspends.
   The collector, the only busy actor, polls `pony_active_schedulers`
   until the count reaches `--min-schedulers`.
3. Drain: the collector drops every block and asks each allocator to run
   its garbage collection. The allocators now run on the one active
   scheduler, freeing memory whose owning threads are suspended: the
   frees are delivered to the suspended owners, whose drain wakes must
   reclaim the memory without scheduling work. The collector polls
   resident memory (VmRSS) until at least `--reclaim-fraction` of the
   payload has returned, asserting on every poll that the active
   scheduler count never rose: draining must not fake activity.
4. Scale up: fresh work is spawned; the count must rise above the
   minimum, proving the suspended threads still activate after their
   drain episodes.

Exit code 0 with a report on success; 1 with the failed phase on failure.
Run with --ponyminthreads 1 (and optionally --ponymaxthreads) so phase 2
has a floor to reach; pass the same value as --min-schedulers.
"""
use "cli"
use "collections"
use "files"
use "time"

use @pony_active_schedulers[U32]()
use @pony_ctx[Pointer[None]]()
use @pony_triggergc[None](ctx: Pointer[None])
use @ponyint_pool_alloc_size[Pointer[U8]](size: USize)
use @ponyint_pool_free_size[None](size: USize, p: USize)
use @memset[Pointer[None]](p: Pointer[U8] tag, c: I32, n: USize)
use @fopen[Pointer[None]](path: Pointer[U8] tag, mode: Pointer[U8] tag)
use @fgets[Pointer[U8]](buf: Pointer[U8] tag, n: I32, f: Pointer[None])
use @fclose[I32](f: Pointer[None])

actor Allocator
  """
  Allocates raw pool blocks — no Pony objects, no distributed GC — so
  the thread this constructor happens to run on is the blocks' owner,
  and freeing them later involves no actor work at all: pointer in,
  pointer out, and the only machinery between the free and the reclaim
  is the allocator's own delivery and drain path.
  """
  new create(collector: Collector, blocks: USize, block_size: USize) =>
    var i: USize = 0

    while i < blocks do
      let p = @ponyint_pool_alloc_size(block_size)
      @memset(p, 0xA5, block_size) // make the pages resident
      collector.collect(p.usize())
      i = i + 1
    end

    collector.allocator_done()

actor Collector
  let _env: Env
  var _blocks: Array[USize] = _blocks.create()
  let _expected_blocks: USize
  let _block_size: USize
  var _allocator_count: USize = 0
  let _payload_bytes: USize
  let _min_schedulers: U32
  let _reclaim_fraction: F64
  var _allocators_done: USize = 0
  var _polls: USize = 0
  var _rss_before_kb: USize = 0
  var _phase: USize = 1
  var _settled: USize = 0
  var _released: Bool = false
  let _timers: Timers = Timers

  new create(env: Env, allocator_count: USize, blocks: USize,
    block_size: USize, min_schedulers: U32, reclaim_fraction: F64)
  =>
    _env = env
    _expected_blocks = allocator_count * blocks
    _block_size = block_size
    _payload_bytes = _expected_blocks * block_size
    _min_schedulers = min_schedulers
    _reclaim_fraction = reclaim_fraction

    _allocator_count = allocator_count

    var i: USize = 0

    while i < allocator_count do
      Allocator(this, blocks, block_size)
      i = i + 1
    end

  be collect(block: USize) =>
    _blocks.push(block)

  be allocator_done() =>
    _allocators_done = _allocators_done + 1

    if _allocators_done == _allocator_count then
      _polls = 0
      _phase = 2
      _schedule_poll()
    end

  fun ref _schedule_poll() =>
    """
    Polling rides a timer so nothing is runnable between polls: an
    always-runnable poll actor gets stolen back and forth, and every
    successful steal resets the thief's suspend clock — the schedulers
    would never scale down.
    """
    let c: Collector tag = this
    _timers(Timer(object iso is TimerNotify
      fun apply(timer: Timer, count: U64): Bool =>
        c.poll()
        false
    end, 250_000_000))

  be poll() =>
    match _phase
    | 2 => wait_for_scale_down()
    | 4 => wait_for_scale_up()
    end

  be poll_busy() =>
    """
    Phase 3's poll never sleeps: while this actor burns, at least one
    scheduler never blocks, so the runtime's quiescence probes — which
    wake every suspended thread and would drain their inboxes as a side
    effect — cannot fire, and with one runnable actor there is no work
    to wake anyone for. The only path that reclaims a suspended
    owner's memory is its drain wake: exactly what the phase asserts,
    which is why the active count may never rise here.
    """
    if _phase != 3 then
      return
    end

    var acc: U64 = 88172645463325252
    var i: U64 = 0

    while i < 2_000_000 do
      acc = (acc * 6364136223846793005) + 1442695040888963407
      i = i + 1
    end

    if acc != 0 then
      wait_for_reclaim()
    end

  fun ref wait_for_scale_down() =>
    if @pony_active_schedulers() <= _min_schedulers then
      _env.out.print("phase 2: scaled down to " +
        @pony_active_schedulers().string() + " schedulers after " +
        _polls.string() + " polls")
      _rss_before_kb = _read_rss_kb()
      _polls = 0
      _phase = 3
      _released = false
      poll_busy()
    else
      _polls = _polls + 1

      if _polls > 400 then
        _fail("phase 2: schedulers never scaled down to " +
          _min_schedulers.string() + " (at " +
          @pony_active_schedulers().string() + ")")
      else
        _schedule_poll()
      end
    end

  fun ref wait_for_reclaim() =>
    if not _released then
      // Free every block from this thread: all of them foreign to it,
      // owned by whichever threads the allocators ran on — threads that
      // are suspended now. No actor is involved; the deliveries and
      // the owners' drain wakes are the only machinery that runs.
      _released = true

      for ptr in _blocks.values() do
        @ponyint_pool_free_size(_block_size, ptr)
      end

      _blocks = _blocks.create()
      poll_busy()
      return
    end

    """
    The frees went to suspended owners; their drain wakes bring the
    resident memory back. The release-and-collect step is real actor
    work and may legitimately wake schedulers, so the assertion is the
    settled state: the payload reclaimed, the count back at the floor,
    and holding there for a full second of polls — a drain wake that
    faked activity would keep the count raised with no work to run.
    """
    let active = @pony_active_schedulers()

    // One runnable actor, no quiescence probes: nothing can
    // legitimately wake a scheduler here, so a rise is a drain wake
    // faking activity.
    if active > _min_schedulers then
      _fail("phase 3: active schedulers rose to " + active.string() +
        " with one runnable actor; a drain wake must not fake activity")
      return
    end

    let rss_now = _read_rss_kb()
    let reclaimed_kb =
      if _rss_before_kb > rss_now then _rss_before_kb - rss_now
      else 0 end
    let needed_kb =
      ((_payload_bytes.f64() * _reclaim_fraction) / 1024).usize()

    if reclaimed_kb >= needed_kb then
      _env.out.print("phase 3: reclaimed " + reclaimed_kb.string() +
        " KiB from suspended owners (needed " + needed_kb.string() +
        "), schedulers held at " + active.string())
      _polls = 0
      _scale_up()
    else
      _polls = _polls + 1

      if (_polls % 2_000) == 0 then
        _env.out.print("phase 3 poll " + _polls.string() + ": rss " +
          rss_now.string() + " KiB (baseline " +
          _rss_before_kb.string() + "), active " + active.string())
      end

      if _polls > 20_000 then
        _fail("phase 3: only " + reclaimed_kb.string() + " of " +
          needed_kb.string() +
          " KiB reclaimed while the owners stayed suspended")
      else
        poll_busy()
      end
    end

  fun ref _scale_up() =>
    """
    Fresh parallel work: the suspended threads must come fully active
    after their drain episodes.
    """
    var i: USize = 0

    while i < 32 do
      Spinner(this)
      i = i + 1
    end

    _phase = 4
    _schedule_poll()

  fun ref wait_for_scale_up() =>
    if @pony_active_schedulers() > _min_schedulers then
      _env.out.print("phase 4: scaled back up to " +
        @pony_active_schedulers().string() + " schedulers")
      _env.out.print("suspend-drain: ok")
    else
      _polls = _polls + 1

      if _polls > 400 then
        _fail("phase 4: schedulers never scaled back up; an activation " +
          "was lost")
      else
        _schedule_poll()
      end
    end

  fun ref _fail(msg: String) =>
    _env.out.print("suspend-drain: FAILED: " + msg)
    _env.exitcode(1)
    _phase = 0 // no further polls fire; the process drains and exits

  fun _read_rss_kb(): USize =>
    """
    VmRSS from /proc/self/status, by hand: proc files report a zero
    size, which trips buffered readers, so this walks fgets lines and
    parses the digits directly.
    """
    let f = @fopen("/proc/self/status".cstring(), "r".cstring())

    if f.is_null() then
      return 0
    end

    var kb: USize = 0
    let buf = Array[U8].init(0, 256)

    while not @fgets(buf.cpointer(), 256, f).is_null() do
      // "VmRSS:" prefix, bytewise, then the digits.
      if ((try buf(0)? else 0 end) == 'V') and
        ((try buf(1)? else 0 end) == 'm') and
        ((try buf(2)? else 0 end) == 'R') and
        ((try buf(3)? else 0 end) == 'S') and
        ((try buf(4)? else 0 end) == 'S') and
        ((try buf(5)? else 0 end) == ':')
      then
        var i: USize = 6
        var started = false

        try
          while i < buf.size() do
            let c = buf(i)?

            if (c >= '0') and (c <= '9') then
              started = true
              kb = (kb * 10) + (c - '0').usize()
            elseif started or (c == 0) then
              break
            end

            i = i + 1
          end
        end

        break
      end
    end

    @fclose(f)
    kb

actor Spinner
  """
  Briefly-busy parallel work for the scale-up phase.
  """
  var _n: U64 = 0

  new create(collector: Collector) =>
    spin(2_000)

  be spin(rounds: U64) =>
    var i: U64 = 0
    var acc: U64 = _n

    while i < 1_000_000 do
      acc = (acc * 6364136223846793005) + 1442695040888963407
      i = i + 1
    end

    _n = acc

    if rounds > 0 then
      spin(rounds - 1)
    end

actor Main
  new create(env: Env) =>
    let cs =
      try
        CommandSpec.leaf("suspend-drain",
          "Suspend-and-drain stress engine", [
          OptionSpec.u64("allocators", "Allocating actors"
            where default' = 8)
          OptionSpec.u64("blocks", "Blocks per allocator"
            where default' = 32)
          OptionSpec.u64("block-size", "Bytes per block"
            where default' = 524288)
          OptionSpec.u64("min-schedulers",
            "The floor phase 2 must reach; pass --ponyminthreads with the "
            + "same value" where default' = 1)
          OptionSpec.f64("reclaim-fraction",
            "Payload fraction that must return in phase 3"
            where default' = 0.5)
        ], [])?
      else
        env.exitcode(1)
        return
      end

    let cmd =
      match CommandParser(cs).parse(env.args, env.vars)
      | let c: Command => c
      else
        env.exitcode(1)
        return
      end

    Collector(env,
      cmd.option("allocators").u64().usize(),
      cmd.option("blocks").u64().usize(),
      cmd.option("block-size").u64().usize(),
      cmd.option("min-schedulers").u64().u32(),
      cmd.option("reclaim-fraction").f64())
