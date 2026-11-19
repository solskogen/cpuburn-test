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

Nothing is copied: the VM uses its own NVRAM, disks, devices and relative paths.
The runner temporarily inserts the test image in drive A: (write-protected),
enables COM1 capture and applies any explicit CPU/RAM/FPU overrides. After
86Box exits, the original configuration is restored byte-for-byte, including
the previous floppy and COM1 settings, also on ordinary errors, Ctrl+C,
SIGTERM or SIGHUP (closed terminal or dropped SSH session). The runner asks
86Box to quit and waits for it, so firmware can save NVRAM, before it stops
the process. NVRAM and disks remain live and can be updated by firmware or the
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

- `full_speed_capacity_percent`: guest time advanced divided by unthrottled
  host step time, multiplied by 100. Sustained values below approximately
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
`speed.jsonl` has every sample, `automation.jsonl` every request.

## Sustained host profiling

The runner uses your VM's CPU settings and 250 guest-ms automation steps,
with 24 guest-hour and eight host-hour overall deadlines; `--step-ms`
controls boundary uncertainty. Each sampled workload runs **until its
host-measured speed settles**:

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
any host. Linux `/proc/PID/stat` process CPU time, which accounts for **all
emulator threads**, is still reported but is too coarse (10 ms ticks) to
decide when to stop. Blocks are not independent when a workload repeats a
long iteration, so treat `settled` as "stable enough to stop", not as a
statistical guarantee; compare repeated runs when the difference is small.
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

`--perf` explicitly attaches Linux `perf record` at 99 Hz with user-space
callchains (`--user-callchains`, which `perf_event_paranoid` 2 allows, where
kernel callchains are refused) and `CLOCK_MONOTONIC` timestamps. It saves `perf.data`, `perf.script`, diagnostics,
and per-ID leaf/inclusive hotpaths aligned only to interior sampling steps.
Unresolved symbols retain their instruction addresses instead of collapsing
every JIT location into one `[unknown]` bucket. Useful callchains still depend
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
