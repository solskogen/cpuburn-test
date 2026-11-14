#!/usr/bin/env python3
"""Run an existing 86Box VM in place through a script of inputs and waits,
logging its speed every emulated second and watching its screen.

The machine runs paced to real time (or, with --held or --ff, as fast as the host
allows), so the speed log shows what the 86Box window shows. Frames are
recorded on emulated time for looking through afterwards, and a screen that
turns to text mode after a graphical one, error words on a text screen, and
CPU resets are flagged as they happen."""

import argparse
import hashlib
from itertools import accumulate
import json
from operator import add
import os
from pathlib import Path
import re
import socket
import struct
import subprocess
import sys
import time
import zlib

from run_86box import (Automation, bios_f1_prompt, display_environment, edited_config,
                       emulator_binary, refuse_running, rom_directory, stop_process, vm_config)


# Set-1 make codes; 0x100 marks an E0-prefixed key, as `key` takes them.
# BIOS setup prompts usually want the keypad's Del (kpdel), not the other.
KEYS = {"esc": 0x01, "backspace": 0x0e, "tab": 0x0f, "enter": 0x1c, "ctrl": 0x1d,
        "shift": 0x2a, "rshift": 0x36, "alt": 0x38, "space": 0x39, "capslock": 0x3a,
        "f11": 0x57, "f12": 0x58, "home": 0x147, "up": 0x148, "pgup": 0x149,
        "left": 0x14b, "right": 0x14d, "end": 0x14f, "down": 0x150, "pgdn": 0x151,
        "ins": 0x152, "del": 0x153, "kpdel": 0x53, "-": 0x0c, "=": 0x0d, "[": 0x1a, "]": 0x1b,
        ";": 0x27, "'": 0x28, "`": 0x29, "\\": 0x2b, ",": 0x33, ".": 0x34, "/": 0x35}
KEYS.update({f"f{n}": 0x3a + n for n in range(1, 11)})
KEYS.update({c: 0x02 + i for i, c in enumerate("1234567890")})
for row, first in (("qwertyuiop", 0x10), ("asdfghjkl", 0x1e), ("zxcvbnm", 0x2c)):
    KEYS.update({c: first + i for i, c in enumerate(row)})

ERROR_WORDS = ("STOP: 0x", "Exception", "exception", "Fatal", "fatal", "Error", "error")

# How often `wait screen` looks at the screen, in emulated ms: often for a
# screen that comes soon, as a dialog after a key does, then every second (a
# screenshot is a PNG encoded on the emulation thread, which a long wait
# would otherwise keep doing four times a second)
SCREEN_POLL_MS = 250
SCREEN_POLL_SLOW_AFTER_MS = 5000
SCREEN_POLL_SLOW_MS = 1000


def unfilter_row(kind, line, above, bpp):
    """A PNG scanline without its filter (PNG specification, section 9),
    given the unfiltered line above it."""
    if kind == 0:
        return bytes(line)
    if kind == 1:  # Sub: a running sum along each channel
        out = bytearray(len(line))
        for channel in range(bpp):
            out[channel::bpp] = bytes(map((255).__and__, accumulate(line[channel::bpp])))
        return bytes(out)
    if kind == 2:  # Up
        return bytes(map((255).__and__, map(add, line, above)))
    out = bytearray(line)
    if kind == 3:  # Average
        for i in range(len(out)):
            left = out[i - bpp] if i >= bpp else 0
            out[i] = (out[i] + ((left + above[i]) >> 1)) & 255
        return bytes(out)
    if kind == 4:  # Paeth
        for i in range(len(out)):
            if i >= bpp:
                a, c = out[i - bpp], above[i - bpp]
            else:
                a = c = 0
            b = above[i]
            pa, pb, pc = abs(b - c), abs(a - c), abs(a + b - 2 * c)
            out[i] = (out[i] + (a if pa <= pb and pa <= pc else b if pb <= pc else c)) & 255
        return bytes(out)
    raise ValueError(f"PNG filter type {kind}")


def png_region(data, x, y, w, h):
    """The RGB bytes, row after row, of the w x h region at x, y of a PNG
    (8-bit RGB or RGBA, not interlaced: what 86Box writes). Only what the
    region depends on is unfiltered: the rows from the nearest one at or
    above it whose filter does not use the row above, and in each row the
    pixels up to the region's right edge."""
    if data[:8] != b"\x89PNG\r\n\x1a\n":
        raise ValueError("not a PNG file")
    pos, header, chunks = 8, None, []
    while pos + 8 <= len(data):
        length, kind = struct.unpack(">I4s", data[pos:pos + 8])
        if kind == b"IHDR":
            header = struct.unpack(">IIBBBBB", data[pos + 8:pos + 21])
        elif kind == b"IDAT":
            chunks.append(data[pos + 8:pos + 8 + length])
        elif kind == b"IEND":
            break
        pos += 12 + length
    if header is None:
        raise ValueError("PNG without a header")
    width, height, depth, colour, _, _, interlace = header
    if depth != 8 or colour not in (2, 6) or interlace:
        raise ValueError("only 8-bit RGB or RGBA PNGs, not interlaced")
    if w <= 0 or h <= 0 or x < 0 or y < 0 or x + w > width or y + h > height:
        raise ValueError(f"region {w}x{h} at {x},{y} is not inside the {width}x{height} image")
    bpp = 3 if colour == 2 else 4
    stride = width * bpp + 1
    raw = zlib.decompress(b"".join(chunks))
    first = y
    while first > 0 and raw[first * stride] > 1:
        first -= 1
    end = (x + w) * bpp
    line = bytes(end)
    rows = []
    for row in range(first, y + h):
        start = row * stride
        line = unfilter_row(raw[start], raw[start + 1:start + 1 + end], line, bpp)
        if row >= y:
            part = line[x * bpp:]
            if bpp == 4:
                rgb = bytearray(w * 3)
                for channel in range(3):
                    rgb[channel::3] = part[channel::4]
                part = bytes(rgb)
            rows.append(part)
    return b"".join(rows)


def region_digest(data, region):
    """What `wait screen` compares: the first 16 hex digits of the SHA-256 of
    the region's RGB bytes. None when the region is not inside the image."""
    try:
        return hashlib.sha256(png_region(data, *region)).hexdigest()[:16]
    except ValueError:
        return None


def key_codes(word):
    """A key name, a scancode, or keys held together joined by '+'."""
    codes = []
    for part in word.lower().split("+"):
        if part in KEYS:
            codes.append(KEYS[part])
        else:
            try:
                code = int(part, 0)
            except ValueError:
                raise ValueError(f"unknown key {part!r}") from None
            if not 1 <= code <= 0x1ff:
                raise ValueError(f"scancode out of range: {part}")
            codes.append(code)
    return codes


def parse_script(text):
    """Script lines: wait text <timeout_ms> <text> |
    wait screen <timeout_ms> <x> <y> <w> <h> <digest> | key <key>... |
    pause <ms> | screenshot <name> | dump <addr> <len> <name> [<view>] |
    cd load <drive> <path> | cd eject <drive> | mark <label>. '#' starts a
    comment line. wait screen runs until the w x h region at x, y of the
    displayed frame has the digest (see region_digest; screen_digest.py
    prints it for a screenshot), looking every SCREEN_POLL_MS, and after
    SCREEN_POLL_SLOW_AFTER_MS every SCREEN_POLL_SLOW_MS: for graphical
    screens, which wait text cannot read. dump saves <len> bytes of memory
    at <addr> (physical, or through a memr view such as linear) as
    <name>.bin, with the registers at that moment in <name>.regs.json. cd
    load inserts the image at <path> (the rest of the line) in CD-ROM drive
    <drive> as the machine runs, and cd eject empties the drive."""
    commands = []
    for number, line in enumerate(text.splitlines(), 1):
        line = line.strip()
        if not line or line.startswith("#"):
            continue
        word, _, rest = line.partition(" ")
        try:
            if word == "wait":
                kind, _, rest = rest.partition(" ")
                if kind == "screen":
                    parts = rest.split()
                    if len(parts) != 6 or not re.fullmatch(r"[0-9a-f]{16}", parts[5]):
                        raise ValueError("use: wait screen <timeout_ms> <x> <y> <w> <h> <digest of 16 hex digits>")
                    timeout, x, y, w, h = (int(p, 0) for p in parts[:5])
                    if x < 0 or y < 0 or w <= 0 or h <= 0:
                        raise ValueError("a wait screen region needs x, y >= 0 and w, h > 0")
                    commands.append(("screen", timeout, (x, y, w, h), parts[5]))
                    continue
                timeout, _, wanted = rest.partition(" ")
                if kind != "text" or not wanted:
                    raise ValueError("use: wait text <timeout_ms> <text>")
                commands.append(("wait", int(timeout, 0), wanted))
            elif word == "key":
                if not rest:
                    raise ValueError("use: key <key>...")
                commands.append(("key", [key_codes(k) for k in rest.split()]))
            elif word == "pause":
                commands.append(("pause", int(rest, 0)))
            elif word == "screenshot":
                if not re.fullmatch(r"[A-Za-z0-9_.-]+", rest):
                    raise ValueError("screenshot names are letters, digits, '_', '-' and '.'")
                commands.append(("screenshot", rest))
            elif word == "dump":
                parts = rest.split()
                if len(parts) not in (3, 4) or not re.fullmatch(r"[A-Za-z0-9_.-]+", parts[2]):
                    raise ValueError("use: dump <addr> <len> <name> [<view>], names of letters, digits, '_', '-' and '.'")
                length = int(parts[1], 0)
                if not 0 < length <= 65536:
                    raise ValueError("dump lengths are 1 to 65536 bytes")
                if len(parts) == 4 and not re.fullmatch(r"[a-z]+", parts[3]):
                    raise ValueError("a dump view is a word such as bus or linear")
                commands.append(("dump", int(parts[0], 0), length, parts[2], parts[3] if len(parts) == 4 else None))
            elif word == "cd":
                verb, _, rest = rest.partition(" ")
                drive, _, path = rest.partition(" ")
                if verb == "load" and drive.isdigit() and path:
                    commands.append(("cd", int(drive), path))
                elif verb == "eject" and drive.isdigit() and not path:
                    commands.append(("cd", int(drive), None))
                else:
                    raise ValueError("use: cd load <drive> <path> or cd eject <drive>")
            elif word == "mark":
                commands.append(("mark", rest))
            else:
                raise ValueError(f"unknown command {word!r}")
        except ValueError as error:
            raise ValueError(f"script line {number}: {error}") from None
    return commands


class Watch:
    """What the screen and the CPU did, sample by sample. A screen is text
    when the `text` request can read it, graphical when it cannot."""

    def __init__(self):
        self.graphical = False
        self.resets = None
        self.last_words = ()

    def alerts(self, info, text):
        found = []
        if text is None:
            self.graphical = True
        elif self.graphical:
            found.append("text mode after a graphical screen")
            self.graphical = False
        words = tuple(w for w in ERROR_WORDS if text and w in text)
        if words and words != self.last_words:
            found.append("text shows " + ", ".join(repr(w) for w in words))
        self.last_words = words
        resets = (info.get("hard_resets"), info.get("soft_resets"))
        if self.resets is not None and resets != self.resets:
            found.append(f"CPU reset (hard {resets[0]}, soft {resets[1]})")
        self.resets = resets
        return found


def run_session(api, commands, out, after_ms, slice_ms=1000, clock=time.monotonic, bios_f1=False):
    """Play the script, then run after_ms more. Each request runs at most
    slice_ms of emulated time, so a sample is taken at least that often.
    With bios_f1, F1 is pressed at a text-mode BIOS prompt asking for it."""
    watch = Watch()
    status = "done"
    last_f1 = -5000
    with (out / "speed.jsonl").open("w") as speed, (out / "alerts.jsonl").open("w") as alerts:
        mark = None

        def sample():
            nonlocal mark, last_f1
            info = api.request("info")
            screen = api.request("text", allow_failure=True)
            text = ("\n".join(bytes.fromhex(r).decode("cp437") for r in screen["chars"])
                    if screen.get("ok") else None)
            row = {"wall": round(clock(), 3), "ms": info["ms"], "speed": info.get("speed_percent"),
                   "w": info.get("w"), "h": info.get("h")}
            if mark is not None:
                row["mark"], mark = mark, None
            speed.write(json.dumps(row) + "\n")
            speed.flush()
            if bios_f1 and text and bios_f1_prompt(text) and info["ms"] - last_f1 >= 5000:
                api.request("key down 0x3b")
                api.request("key up 0x3b")
                last_f1 = info["ms"]
            for alert in watch.alerts(info, text):
                api.request(f"screenshot {out / ('alert-%d.png' % info['ms'])}", allow_failure=True)
                alerts.write(json.dumps({"ms": info["ms"], "alert": alert}) + "\n")
                alerts.flush()
                print(f"ALERT at {info['ms']} ms: {alert}", file=sys.stderr)

        def run_for(ms):
            while ms > 0:
                part = min(ms, slice_ms)
                api.request(f"step {part}")
                ms -= part
                sample()

        for command in commands:
            if command[0] == "wait":
                _, timeout, wanted = command
                while True:
                    part = min(timeout, slice_ms)
                    reply = api.request(f"wait text {part} {wanted}")
                    sample()
                    if reply.get("matched"):
                        break
                    timeout -= part
                    if timeout <= 0:
                        status = f"timed out waiting for {wanted!r}"
                        break
                if status != "done":
                    break
            elif command[0] == "screen":
                _, timeout, region, digest = command
                shot = out / "wait-screen.png"
                waited = unsampled = 0
                while True:
                    reply = api.request(f"screenshot {shot}", allow_failure=True)
                    if reply.get("ok") and region_digest(shot.read_bytes(), region) == digest:
                        break
                    if waited >= timeout:
                        status = f"timed out waiting for screen {digest} at {region}"
                        break
                    poll = SCREEN_POLL_MS if waited < SCREEN_POLL_SLOW_AFTER_MS else SCREEN_POLL_SLOW_MS
                    api.request(f"step {poll}")
                    waited += poll
                    unsampled += poll
                    if unsampled >= slice_ms:
                        sample()
                        unsampled = 0
                shot.unlink(missing_ok=True)
                if status != "done":
                    break
            elif command[0] == "key":
                for codes in command[1]:
                    api.request("key down " + " ".join(hex(c) for c in codes))
                    api.request("key up " + " ".join(hex(c) for c in reversed(codes)))
            elif command[0] == "pause":
                run_for(command[1])
            elif command[0] == "screenshot":
                # No frame yet (a card the firmware never started) is noted,
                # not the end of the run
                shot = api.request(f"screenshot {out / (command[1] + '.png')}", allow_failure=True)
                if not shot.get("ok"):
                    alerts.write(json.dumps({"ms": shot.get("ms"), "alert": f"screenshot {command[1]}: {shot.get('error')}"}) + "\n")
                    alerts.flush()
            elif command[0] == "dump":
                _, addr, length, name, view = command
                memory = api.request(f"memr {addr:#x} {length}" + (f" {view}" if view else ""))
                (out / (name + ".bin")).write_bytes(bytes.fromhex(memory["data"]))
                (out / (name + ".regs.json")).write_text(json.dumps(api.request("regs"), indent=1) + "\n")
            elif command[0] == "cd":
                if command[2] is None:
                    api.request(f"cd eject {command[1]}")
                else:
                    api.request(f"cd load {command[1]} {command[2]}")
            elif command[0] == "mark":
                mark = command[1]
        if status == "done":
            run_for(after_ms)
    return status


def worst_seconds(out, limit=10):
    """The slowest emulated seconds of a run, from speed.jsonl."""
    rows = [json.loads(line) for line in (out / "speed.jsonl").read_text().splitlines()]
    # The first emulated second also counts 86Box starting up
    rows = [r for r in rows[1:] if isinstance(r.get("speed"), int) and r["speed"] >= 0]
    return sorted(rows, key=lambda r: r["speed"])[:limit]


def arguments():
    parser = argparse.ArgumentParser(description=__doc__, epilog=(
        "Needs an AUTOMATION-enabled 86Box. The VM runs in place: its disks and NVRAM "
        "change as the guest writes them; its configuration is restored afterwards. "
        "--set and --unset take SECTION/KEY, e.g. --unset 'Floppy and CD-ROM drives/fdd_01_fn'."))
    parser.add_argument("--vm", type=Path, required=True,
                        help="VM directory (containing 86box.cfg) or configuration file")
    parser.add_argument("--output-dir", type=Path, required=True)
    parser.add_argument("--script", type=Path, help="inputs and waits to play (see parse_script)")
    parser.add_argument("--after-ms", type=int, default=0,
                        help="emulated ms to run on after the script (default 0)")
    parser.add_argument("--emulator", default="86Box")
    parser.add_argument("--rompath", type=Path)
    parser.add_argument("--held", action="store_true",
                        help="run unpaced (as fast as the host allows), for functional checks")
    parser.add_argument("--ff", action="store_true",
                        help="run released in fast forward, unpaced: each second's speed is what 86Box "
                             "can do there, so dips show undamped by pacing")
    parser.add_argument("--record-ms", type=int, default=10000,
                        help="save a frame every this many emulated ms (default 10000, 0 for none)")
    parser.add_argument("--bios-f1", action="store_true",
                        help="press F1 at a text-mode BIOS prompt to resume or continue (fresh CMOS)")
    parser.add_argument("--seed", default="1", help="--automation-seed for 86Box (default 1)")
    parser.add_argument("--set", action="append", default=[], metavar="SECTION/KEY=VALUE")
    parser.add_argument("--unset", action="append", default=[], metavar="SECTION/KEY")
    parser.add_argument("--wipe-hdd", metavar="NN",
                        help="replace hard disk NN's image (hdd_NN_fn) with an empty file of "
                             "the same size before starting: everything on it is lost")
    parser.add_argument("--wall-seconds", type=float, default=3600)
    return parser.parse_args()


def config_edits(args):
    edits = []
    for item in args.set:
        name, sep, value = item.partition("=")
        section, slash, key = name.rpartition("/")
        if not sep or not slash or not key:
            raise ValueError(f"--set needs SECTION/KEY=VALUE: {item!r}")
        edits.append((section, key, value))
    for item in args.unset:
        section, slash, key = item.rpartition("/")
        if not slash or not key:
            raise ValueError(f"--unset needs SECTION/KEY: {item!r}")
        edits.append((section, key, None))

    def edit(config):
        for section, key, value in edits:
            if value is None:
                if config.has_section(section):
                    config.remove_option(section, key)
            else:
                if not config.has_section(section):
                    config.add_section(section)
                config[section][key] = value
    return edit


def wipe_hdd(source, number):
    """Hard disk NN's image, emptied in place at its own size (a sparse file)."""
    import configparser
    config = configparser.ConfigParser(interpolation=None, strict=False)
    config.read_string(source.read_text(encoding="utf-8-sig"))
    name = config.get("Hard disks", f"hdd_{number}_fn", fallback=None)
    if not name:
        raise RuntimeError(f"no hdd_{number}_fn in [Hard disks]")
    path = Path(name) if Path(name).is_absolute() else source.parent / name
    size = path.stat().st_size
    with path.open("r+b") as handle:
        # A VHD keeps its geometry in a footer (and, if dynamic, a copy at the
        # start): emptied, it would be no image at all. Raw images only.
        head = handle.read(8)
        handle.seek(max(0, size - 512))
        if head == b"conectix" or handle.read(8) == b"conectix":
            raise RuntimeError(f"{path} is a VHD; --wipe-hdd empties raw images only")
        handle.truncate(0)
        handle.truncate(size)
    return path, size


def main():
    args = arguments()
    try:
        emulator = emulator_binary(args.emulator)
        roms = rom_directory(args.rompath)
        source = vm_config(args.vm)
        commands = parse_script(args.script.read_text()) if args.script else []
        out = args.output_dir.expanduser().absolute()
        out.mkdir(parents=True, exist_ok=True)
        if any(out.iterdir()):
            raise RuntimeError(f"output directory is not empty: {out}")
        sock = out / "automation.sock"
        if len(os.fsencode(sock)) >= 108:
            raise RuntimeError("output path is too long for a UNIX automation socket")
        refuse_running(source)
        if args.held and args.ff:
            raise ValueError("--held and --ff are two ways of running unpaced: pick one")
        result = {"vm": str(source), "emulator": str(emulator), "held": args.held, "ff": args.ff}
        with edited_config(source, config_edits(args)) as (_, source_hash):
            result["source_config_sha256"] = source_hash
            if args.wipe_hdd:
                path, size = wipe_hdd(source, args.wipe_hdd)
                result["wiped_hdd"] = {"path": str(path), "bytes": size}
            command = [str(emulator), "--automation", f"unix:{sock}", "--automation-hold",
                       "--automation-seed", args.seed, "--noconfirm", "--vmpath", str(source.parent),
                       "--rompath", str(roms), "--logfile", str(out / "emulator.log"), str(source)]
            environment = os.environ.copy()
            environment.update(display_environment())
            emulator_process = connection = None
            started = time.monotonic()
            try:
                with (out / "process.log").open("wb") as log:
                    emulator_process = subprocess.Popen(command, cwd=source.parent, env=environment,
                                                        stdout=log, stderr=log, start_new_session=True)
                while connection is None:
                    if emulator_process.poll() is not None:
                        raise RuntimeError(f"86Box exited {emulator_process.returncode}; see process.log")
                    if time.monotonic() - started > 60:
                        raise TimeoutError("86Box automation socket startup timed out")
                    connection = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
                    try:
                        connection.connect(str(sock))
                    except (FileNotFoundError, ConnectionRefusedError):
                        connection.close()
                        connection = None
                        time.sleep(0.05)
                with (out / "automation.jsonl").open("w") as transcript:
                    api = Automation(connection, transcript, started + args.wall_seconds)
                    result["info"] = api.request("info")
                    if args.record_ms:
                        (out / "frames").mkdir()
                        api.request(f"record start {args.record_ms} {out / 'frames'}")
                    if not args.held:
                        api.request("release")
                        if args.ff:
                            api.request("ff on")
                    result["status"] = run_session(api, commands, out, args.after_ms, bios_f1=args.bios_f1)
                    if args.record_ms:
                        result["frames"] = api.request("record stop").get("frames")
                    api.request(f"screenshot {out / 'screen.png'}", allow_failure=True)
                    result["final_info"] = api.request("info")
                    result["registers"] = api.request("regs")
                    api.request("hold")
                    api.request("quit", allow_failure=True)
            except TimeoutError as error:
                result["status"] = f"timeout: {error}"
            finally:
                if connection is not None:
                    connection.close()
                if emulator_process is not None:
                    try:
                        emulator_process.wait(timeout=15)
                    except subprocess.TimeoutExpired:
                        stop_process(emulator_process)
        if (out / "speed.jsonl").exists():
            result["worst_seconds"] = worst_seconds(out)
        alerts = out / "alerts.jsonl"
        result["alerts"] = ([json.loads(l) for l in alerts.read_text().splitlines()]
                            if alerts.exists() else [])
        (out / "result.json").write_text(json.dumps(result, indent=2) + "\n")
        print(json.dumps({"status": result["status"], "alerts": len(result["alerts"]),
                          "worst": [(r["ms"], r["speed"]) for r in result.get("worst_seconds", [])[:5]]}))
        return 0 if result["status"] == "done" and not result["alerts"] else 1
    except (OSError, RuntimeError, ValueError) as error:
        print(f"session_86box: {error}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    sys.exit(main())
