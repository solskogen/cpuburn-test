# Running and measuring

## Measure per-test emulation capacity

On Linux, with Python 3.10+, installed ROMs and an `AUTOMATION=ON` 86Box
build. 86Box opens natively in the current session (Wayland if `WAYLAND_DISPLAY`
is set, otherwise X with `DISPLAY`); without either, it runs with Qt's
offscreen platform, with no window:

```sh
make BENCHMARKS=1
python3 tools/run_86box.py --vm "/path/to/My VM"
```

The runner starts your VM **in place**; stop it first and do not start another
instance or serial controller during the run. `--vm` accepts any VM directory
containing `86box.cfg`, or a configuration file such as
`--vm "/another/location/custom.cfg"`. There is no required VM storage location.
CPU family, clock, multiplier, RAM, FPU and the primary video card come from
that configuration. Omit `--dynarec` to preserve its engine selection; use
`--dynarec on|off` to override it for this run only.
The runner uses `86Box` from `PATH` by default; `--emulator /path/to/86Box`
selects another binary.

To compare execution engines, run the same VM twice:

```sh
python3 tools/run_86box.py --vm "/path/to/My VM" --dynarec on
python3 tools/run_86box.py --vm "/path/to/My VM" --dynarec off \
  --output-dir "/path/to/interpreter-results"
```

Other optional switches:

| Option | When to use it |
|---|---|
| `--bios-f1` | Automatically press F1 when a recognized text-mode BIOS prompt asks to resume/continue, such as after a CMOS configuration change. It does not configure the BIOS and cannot detect graphical prompts. |
| `--label interpreter` | Add an arbitrary descriptive label to `result.json` for identifying comparisons. It does **not** select the interpreter; only `--dynarec off` does that. Omit it unless you want a label. |
| `--key 20000:0x3c` | Press F2 at 20,000 emulated milliseconds, only for firmware that explicitly offers F2 to load defaults and continue. General syntax is `EMULATED_MS:SET1_SCANCODE`; this is not automatic BIOS configuration. |
| `--pin 13` | Pin 86Box (all threads) to one logical CPU (Linux). That core's governor, boost state and frequency are recorded with the result. |
| `--repeat 5` | Boot and run the suite 5 times into `repeat-01/` … `repeat-05/`; `summary.json` pools them. |
| `--aa` | Run the same binary at least twice and report an A/A noise floor (see [Results](results.md#capacity-comparisons-and-summaryjson)). |
| `--headless` | Qt offscreen, null OpenAL/SDL audio and `[General] sound_muted = 1`, restored afterwards. |
| `--only a,b` / `--skip c` / `--resume-from d` | Run part of the suite without rebuilding the image (needs a guest that offers runtime selection; see below). Repeatable; comma-separated record names. |
| `--transcript off\|errors\|compact\|full` | Detail of `automation.jsonl.gz` (default `compact`). |
| `--perf`, `--perf-stat` | Host profiling, see [Sustained host profiling](#sustained-host-profiling). |

Nothing is copied: the VM uses its own NVRAM, disks, devices and relative paths.
The runner temporarily inserts the test image in drive A: (write-protected,
with `fdd_01_turbo = 1` so it loads without real floppy timing),
enables COM1 capture and applies any explicit CPU/RAM/FPU overrides. After
86Box exits, the original configuration is restored byte-for-byte, including
the previous floppy and COM1 settings, also on ordinary errors, Ctrl+C,
SIGTERM or SIGHUP (closed terminal or dropped SSH session). The runner asks
86Box to quit and waits for it, so firmware can save NVRAM, before it stops
the process. 86Box runs in its own session and is stopped by PID on every
exit path, so a second Ctrl+C cannot leave it running; a signal that arrives
during cleanup waits until 86Box is stopped and the configuration restored.
NVRAM and disks remain live and can be updated by firmware or the
guest; they are **not** rolled back. Before changing anything the runner saves
the original as `86box.cfg.cpuburn-backup` next to it and deletes that file
once the restore is written. If SIGKILL or a power failure prevents the
restore, the backup remains and the next run refuses to start until you move
it back over the configuration or delete it.
Ensure the BIOS boots drive A:, not your installed OS. Inspect `screen.log`
or `screen.png` if boot stalls.
Use `--rompath /path/to/roms` for firmware outside standard locations.
Missing configuration or required CPU/RAM settings produce an error;
firmware/boot problems remain explicit errors or incomplete runs, not passes.
No firmware is downloaded.

By default, `serial.log`, `result.json` and the other capture diagnostics go
directly into the supplied VM directory (beside the configuration file).
Use `--output-dir /path/to/results` to save them elsewhere. Existing capture
files are not overwritten; choose another output directory for another run.
In `result.json`, **each sampled test** has an entry under
`workload_capture.workloads`, identified by `name` and `id`:

- `cpu_full_speed_capacity_percent`: guest time advanced divided by the host
  CPU time of 86Box's emulation thread over the same steps, multiplied by
  100 (speed % per host CPU second). This is the figure `summary.json` and
  `perf_report.py --compare-capacity` use; `workload_capture.cpu_scope` says
  how the CPU time was measured (see
  [Sustained host profiling](#sustained-host-profiling)).
- `full_speed_capacity_percent`: guest time advanced divided by unthrottled
  host step (wall) time, multiplied by 100. Sustained values below approximately
  **100%** indicate that test could not maintain the configured guest speed
  on this host.
- `sample_stop`: `settled` when the host-measured speed settled and the
  sample was released, `capped` when it had not settled after the maximum
  sample time (the result is still usable but noisier; it is also listed as
  `capped:<id>` in `warnings`).
- `settle.window_speed_percent`: the same capacity measured over the settled
  window only, without its leading warm-in blocks; `settle` also reports the
  relative interval and drift that decided the stop, and
  `block_speed_percent` lists each block's speed.
- `under_sampled`: true when the sample ended before it settled or reached
  the cap (for example, the capture stopped); do not treat it as a firm
  performance result.

This is measured capacity, not the GUI speed reading or an automatic
slowdown verdict. Correctness remains separate: check FAIL/FAULT records,
and compare repeated runs under matching host conditions. Accuracy-only
images do not provide sustained benchmark samples.
See [Sustained host profiling](#sustained-host-profiling) for sampling limits,
optional host profiling; `--perf` is not required.

Each run directory also gets `summary.json`, one entry per test with its
status, capacity median and 95% interval, rank and guest tags, described in
[Results](results.md#capacity-comparisons-and-summaryjson). With `--repeat`,
the parent directory's `summary.json` covers all repeats.

**Runtime selection.** With `--only`, `--skip` or `--resume-from`, the runner
waits for the guest's `CONTROL protocol=sel1 ready=1` line (after the CPU
handshake), sends one `@sel1 <only> <skip> <from>` line (each field a
comma-separated list of record names, or `-`) and requires the guest's
`CONTROL protocol=sel1 accepted=1 only=<n> skip=<n> from=<name|->` reply to
match. A guest that never offers selection makes such a run an error
(`selection.state` in `result.json`); without these options nothing is sent.

**Timeouts and hangs.** `--startup-seconds` (default 120) bounds the wait for
86Box's automation socket and `--request-seconds` (default 300) any single
automation request, separately from the `--wall-seconds` deadline. When no
`BEGIN`/`WORK`/`TEST` line arrives for `--hang-seconds` (default 600) guest
seconds, the test is reported as `HANG` in `result.json` (`hangs`, with EIP
and whether interrupts were enabled) and `summary.json`; `--on-hang stop`
(default) ends the run, `continue` keeps waiting for the next line.

### Example: passing and keeping up versus passing but too slow

These are **illustrative excerpts**, not measured results or complete records.
For the same test, both runs might print this in `serial.log`:

```text
TEST r=1 name=int_alu status=PASS ...
```

The assertions passed in both runs. The speed difference appears in
`result.json`, under `workload_capture.workloads`, not in that serial line.
A run with enough capacity to maintain real time might contain:

```json
{
  "name": "int_alu",
  "emulated_ms": 4000,
  "step_wall_seconds": 3.0,
  "process_cpu_seconds": 3.0,
  "full_speed_capacity_percent": 133.33333333333334,
  "sample_stop": "settled",
  "under_sampled": false
}
```

It advanced 4 guest seconds in 3 host seconds: **133% capacity**, with
headroom above the configured speed. Together with PASS, this is the desired
result for the checked workload, not proof of perfect emulation.

A slower run could instead contain:

```json
{
  "name": "int_alu",
  "emulated_ms": 2250,
  "step_wall_seconds": 3.0,
  "process_cpu_seconds": 3.0,
  "full_speed_capacity_percent": 75.0,
  "sample_stop": "settled",
  "under_sampled": false
}
```

It advanced only 2.25 guest seconds in 3 host seconds: **75% capacity**.
The checked results are still correct, but this host cannot sustain the
configured CPU speed during that workload. A FAIL at 133% would mean the
opposite problem: sufficient speed, incorrect results. Capacity near 100%
needs repeated measurements before drawing conclusions.

## Boot and capture results

Insert the built `floppy.img` into drive A: of an i486-or-newer VM and boot
from the floppy. No guest OS or hard disk is required.

To display and save serial output with the supplied Python 3 script, use
an 86Box build with `AUTOMATION=ON`. With 86Box stopped, enable COM1 capture
in the VM's existing `86box.cfg` section:

```ini
[Ports (COM & LPT)]
serial1_enabled = 1
serial1_device = automation
```

On Linux/macOS, start the VM in one terminal (replace the binary and VM paths):

```sh
86Box --vmpath /path/to/disposable-vm \
  --automation "unix:/tmp/cpuburn-$UID.sock" --automation-hold
```

In another terminal, from this project's directory:

```sh
python3 tools/capture_serial.py "unix:/tmp/cpuburn-$UID.sock"
python3 tools/perf_report.py serial.log --output report.json
```

The script displays results and saves `serial.log` until `DONE`. Inspect
`SUMMARY`: successful capture does **not** mean all tests passed. The VM
stays open and held afterward, including if capture fails; partial logs are
retained. Use `--output another-run.log` for subsequent runs; existing files
are not overwritten. Do not run another controller against the same VM
while capturing. See `python3 tools/capture_serial.py --help` for options.

For Windows or local TCP capture, start 86Box with
`--automation tcp:7086 --automation-hold` instead, then use:

```sh
python3 tools/capture_serial.py tcp:127.0.0.1:7086 \
  --token-file /path/to/disposable-vm/automation.token --output serial-tcp.log
```

Keep the socket local; do not expose it to the internet. If no results
appear, check the VGA screen for a BIOS prompt and ensure firmware enables
COM1. For automatic handling of supported BIOS F1 prompts, use
[`tools/run_86box.py --bios-f1`](#sustained-host-profiling).

Every record runs at least twice: a warm-up invocation and then measured
ones. In accuracy mode (`SUITE=accuracy`, `BENCHMARKS=0` or an accepted
`@cpu1 ... accuracy` frame) and for check-only records there is exactly one
measured invocation, so the second run exercises code an emulator has
already compiled (dynamic recompiler) or cached. `TEST` reports
`iterations=` (measured) and `invocations=` (all, including the warm-up).

### Select records at run time

After the `@cpu1` exchange (or when none arrived) the guest prints
`CONTROL protocol=sel1 ready=1` and waits 500 ms for one line:

```text
@sel1 ONLY SKIP FROM
```

Each field is a comma-separated list of record names, or `-` for none;
`FROM` is a single name. `ONLY` runs just those records (intersected with a
build-time `ONLY=`), `SKIP` never runs them, and `FROM` skips records until
that name is reached, to resume an interrupted run. `ONLY` and `FROM` do not
remove the `cpu_model_contract` record; `SKIP` does. After checking every
name against the suite the guest prints `CONTROL protocol=sel1 accepted=1
only=N skip=N from=NAME|-`. A malformed frame, an unknown name or a partial
line is `FATAL`; without input the image runs as built. Filtered records are
counted in the `FILTER ... runtime_selection=N` line, not listed.

## Drive a VM and log its real-time speed

`tools/session_86box.py` runs an existing VM in place, as `run_86box.py`
does, but with no test floppy: it plays a script of keys and waits, and
logs 86Box's own speed reading (`speed_percent`, the window's figure) for
every emulated second. It is for finding where an OS install or boot can't
keep up, and for checking that it gets through:

```sh
python3 tools/session_86box.py --vm "/path/to/My VM" --output-dir /tmp/qi-run \
  --script /path/to/my-session.txt --wipe-hdd 01
```

- **Script:** one command per line: `wait text <timeout_ms> <text>` (run
  until a row of the text screen shows it), `key <key>...` (each pressed and
  released; names such as `enter`, `down`, `f1`, `kpdel`, letters, or set-1
  scancodes, `ctrl+alt+del` for keys held together), `pause <ms>` (emulated),
  `screenshot <name>`, `dump <addr> <len> <name> [<view>]` (saves `len`
  bytes of memory at `addr`, physical or through a `memr` view such as
  `linear`, as `<name>.bin`, with the registers at that moment in
  `<name>.regs.json`), `cd load <drive> <path>` (inserts the image
  at `path`, the rest of the line, in CD-ROM drive `drive` as the machine
  runs) and `mark <label>` (tags the next speed sample). `#` starts a comment. A `wait` that times out ends the run.
- **Paced:** the machine runs in real time, so the log shows what the window
  shows. `--held` runs it unpaced instead, for functional checks.
- **Screens:** a frame is saved every `--record-ms` (default 10000) of
  emulated time in `frames/`. A text screen after a graphical one, error
  words on a text screen (`Exception`, `STOP: 0x`, `Fatal`, `Error`) and CPU
  resets go into `alerts.jsonl` with a screenshot, as they happen.
  `text` reads the VGA text buffer: a framebuffer console that only looks
  like text (Linux's, for example) can't be read or waited
  for, so the script uses `pause` there.
- **Configuration:** `--set "SECTION/KEY=VALUE"` and `--unset "SECTION/KEY"`
  change it for the run only; it is restored byte for byte afterwards.
  `--bios-f1` presses F1 at a text-mode BIOS prompt (fresh CMOS).
- **`--wipe-hdd NN`** empties hard disk NN's image in place, at its size,
  before starting: everything on it is lost. It refuses a VHD, whose
  structure it would destroy; raw images only.

`result.json` has the status, the alerts and the ten slowest seconds (the
first emulated second, which includes 86Box starting, is left out);
`speed.jsonl` has every sample and `automation.jsonl.gz` the requests
(`--transcript`, as for `run_86box.py`: by default without screen bodies, and
`text` only when the screen changed). The screen is read for alerts at most
every `--text-poll-ms` (default 2000) emulated ms; CPU resets are checked at
every sample. `--startup-seconds` and `--request-seconds` bound 86Box's start
and each request; SIGTERM and SIGHUP stop 86Box and restore the configuration
like Ctrl+C.

## Sustained host profiling

The runner uses your VM's CPU settings and automation steps of at most
`--step-ms` (default 250) guest ms, with 24 guest-hour and eight host-hour
overall deadlines. By default (`--boundaries wait-serial`) each step is a
`wait serial` request that stops 86Box within 1 guest ms of the next `WORK`
line, so phase boundaries end a step instead of falling inside one and no
sample loses its edge steps. The rest of that line is completed with 1 ms
steps. An 86Box without `wait serial` falls back to fixed steps
(`--boundaries step`), noted as `boundary_note` in `result.json`, where steps
containing a boundary are discarded. The screen is read at
most every `--text-poll-ms` (default 2000) emulated ms, preferably while the
serial port is idle (at the latest after five intervals). Each sampled workload runs **until its host-measured speed
settles**:

- Interior steps are grouped into blocks of at least `--block-seconds`
  (default 0.2) host wall seconds, and each block's speed is guest time per
  host wall time.
- The leading quarter of the blocks (at least one) is left out as warm-in.
- The sample settles once it has run `--min-sample-seconds` (default 1) host
  seconds, at least five blocks remain, twice the standard error of their
  mean speed is within `--settle-tolerance` (default 0.01, i.e. 1%) of the
  mean, and the means of the first and second halves of those blocks differ
  by no more than that tolerance.
- A sample that has not settled after `--max-sample-seconds` (default 20)
  host seconds stops anyway and is marked `capped`.

Steady workloads therefore stop after a couple of host seconds, while noisy
or drifting ones get more time. Settling uses host wall time, which works on
any host; its interval uses batch means of neighbouring blocks, which keeps
it honest when blocks are correlated. Treat `settled` as "stable enough to
stop", not as a statistical guarantee; compare repeated runs (`--repeat`)
when the difference is small.

Host CPU time is the **emulation thread's** own: on Linux the runner finds
the thread 86Box names `main_thread` in `/proc/PID/task/*/comm` and reads its
`schedstat` (nanoseconds). `workload_capture.cpu_scope` records what was
used: `emulation_thread_schedstat_ns`, else `emulation_thread_stat_ticks`
(10 ms ticks), else `process_all_threads_stat_ticks` (all threads, as older
results), else `unavailable` on hosts without `/proc`. `--pin`, `--perf` and
`--perf-stat` need Linux and are refused elsewhere. `result.json` and
`launch.json` record the host (`host`: CPU model, kernel, pinned core,
governor, boost) and the pinned core's frequency at start and end
(`host_state`). `--perf-stat` counts cycles and instructions of the emulation
thread per step (`perf_event_open`, user-only when the kernel refuses more),
giving an IPC per test.
Runs require the local automation-enabled 86Box build; standalone floppy boot
does not require this interface.

The runner renews a 30-guest-second serial lease until the sample settles
or is capped, then releases it. A release never shortens the guest minimum
(`TEST_SECONDS`). If the
controller disappears, the last lease expires, so the guest does not wait
forever. The timer interrupt drains serial control frames to avoid overrunning
the UART during long invocations. The runner sends at most one frame per
step, only for the sample still active after that step, and repeats renewals
and releases every 250 emulated ms until the phase ends; a short sample that
starts and ends within one step gets no frame. Interrupt-disabled intervals can defer receipt, and complete invocations
can overshoot the requested minimum. Frames are `+IIIIIIII\n` / `-IIIIIIII\n`, using eight
hexadecimal digits for the current workload ID. Malformed and stale frames
cannot extend a different workload.

`result.json` includes `workload_capture`: separate warm-up/sample phases,
interior-step host wall time, process CPU time, guest milliseconds, aggregated
workloads, the sampling settings, per-sample settle state and block speeds,
and explicit undersampling and capped-sample warnings. Steps containing a phase
boundary or partial serial line are excluded, not assigned speculatively.
`full_speed_capacity_percent` is guest time / unthrottled step wall time × 100:
below 100 identifies a path unable to keep up on that host under these
conditions. It is **capacity, not the GUI's actual-speed reading**, and is not
a Pi 5 measurement unless run there. The CPU-normalized ratio additionally
charges every emulator thread; it is not a single-thread cycle estimate.
Boot, serial capture and non-step control time are recorded separately.

`--perf` explicitly attaches Linux `perf record` to the emulation thread
only (`-t TID`) at `--perf-freq` (default 999) Hz, with DWARF user-space
callchains (`--perf-callgraph dwarf`, or `fp`/`none`; `--user-callchains`,
which `perf_event_paranoid` 2 allows) and `CLOCK_MONOTONIC` timestamps.
Recording starts disabled and is switched on through `perf --control` only
while a sample's interior steps run. It saves `perf.data` and diagnostics,
and streams `perf script` into per-ID leaf/inclusive hotpaths aligned to
interior sampling steps, without writing a script file. Unresolved symbols
retain their instruction addresses instead of collapsing every JIT location
into one `[unknown]` bucket; `jit_share` is the fraction of samples whose
leaf lies in an anonymous executable mapping or `[unknown]`, i.e. generated
code. If 86Box wrote `/tmp/perf-<pid>.map`, perf uses it and `perf_map`
names it. Useful callchains still depend
on emulator symbols and host unwinding support; raw samples remain in
`perf.data`.
Missing tools, event permissions and recording failures are explicit errors;
there is no pretend-success fallback. Without `--perf`, the same guest run and
host CPU-time attribution still operate. Host architecture/kernel, CPU,
emulator hash and image hash accompany results.

Use passing, sufficiently sampled workloads to evaluate performance; inspect
FAIL records to fix correctness first. A fail-fast vector may repeatedly
exercise only its failing path, not the complete intended instruction mix.
Unexpected FAULT/early SKIP and missing COM1 output cannot establish a useful
profile. An AL440LX/Pentium II VM may need BIOS serial setup before
per-workload host attribution is available, although the floppy itself can
still complete with VGA output. No Pi 5 or physical Pentium II result is
claimed from local desktop-emulator validation.
