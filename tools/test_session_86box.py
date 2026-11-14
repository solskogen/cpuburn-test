import hashlib
import json
from pathlib import Path
import random
import struct
import tempfile
import unittest
import zlib

from session_86box import (SCREEN_POLL_MS, SCREEN_POLL_SLOW_AFTER_MS, SCREEN_POLL_SLOW_MS, Watch, key_codes,
                           parse_script, png_region, region_digest, run_session, wipe_hdd, worst_seconds)


def encode_png(width, rows, filters, colour=2):
    """A PNG of the pixel rows (width x 3 or 4 bytes each), row n filtered
    with filters[n % len(filters)], as an encoder that picks filters might."""
    bpp = 3 if colour == 2 else 4
    above = bytes(width * bpp)
    lines = []
    for n, row in enumerate(rows):
        kind = filters[n % len(filters)]
        line = bytearray([kind])
        for i in range(len(row)):
            a = row[i - bpp] if i >= bpp else 0
            b = above[i]
            c = above[i - bpp] if i >= bpp else 0
            p = a + b - c
            paeth = min((abs(p - a), 0, a), (abs(p - b), 1, b), (abs(p - c), 2, c))[2]
            line.append((row[i] - (0, a, b, (a + b) >> 1, paeth)[kind]) & 255)
        lines.append(bytes(line))
        above = row

    def chunk(kind, body):
        return struct.pack(">I", len(body)) + kind + body + struct.pack(">I", zlib.crc32(kind + body))
    return (b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", width, len(rows), 8, colour, 0, 0, 0))
            + chunk(b"IDAT", zlib.compress(b"".join(lines))) + chunk(b"IEND", b""))


class FakeApi:
    """Answers like 86Box's automation socket, with a scripted screen."""

    def __init__(self, screens, match_after=None):
        self.ms = 0
        self.requests = []
        self.screens = screens
        self.match_after = match_after

    def request(self, command, allow_failure=False):
        self.requests.append(command)
        if command.startswith("step "):
            self.ms += int(command.split()[1])
            return {"ok": True, "ms": self.ms}
        if command.startswith("wait text "):
            part = int(command.split()[2])
            if self.match_after is not None and self.ms + part >= self.match_after:
                self.ms = self.match_after
                return {"ok": True, "ms": self.ms, "matched": True}
            self.ms += part
            return {"ok": True, "ms": self.ms, "matched": False}
        if command == "info":
            w, h, _ = self.screens(self.ms)
            return {"ok": True, "ms": self.ms, "speed_percent": 100, "w": w, "h": h,
                    "hard_resets": 0, "soft_resets": 0}
        if command == "text":
            text = self.screens(self.ms)[2]
            if text is None:
                return {"ok": False, "error": "graphics mode"}
            return {"ok": True, "chars": [text.encode("cp437").hex()]}
        return {"ok": True, "ms": self.ms}


class ScriptTest(unittest.TestCase):
    def test_parse(self):
        commands = parse_script("""# comment
wait text 30000 ENTER to start QuickInstall
key enter down y+e ctrl+alt+del 0x148
pause 2000
mark copy
screenshot menu-1
""")
        self.assertEqual(commands[0], ("wait", 30000, "ENTER to start QuickInstall"))
        self.assertEqual(commands[1], ("key", [[0x1c], [0x150], [0x15, 0x12], [0x1d, 0x38, 0x153], [0x148]]))
        self.assertEqual(commands[2:], [("pause", 2000), ("mark", "copy"), ("screenshot", "menu-1")])

    def test_bad_lines(self):
        for line in ("wait mem 10 x", "key", "key nosuchkey", "pause soon", "jump 3",
                     "screenshot ../x", "key 0x200"):
            with self.assertRaises(ValueError, msg=line):
                parse_script(line)

    def test_cd_lines(self):
        commands = parse_script("cd load 0 /iso/Windows NT/disc one.iso\ncd eject 1\n")
        self.assertEqual(commands, [("cd", 0, "/iso/Windows NT/disc one.iso"), ("cd", 1, None)])
        for line in ("cd load 0", "cd load x /a.iso", "cd eject", "cd eject x", "cd eject 0 /a.iso", "cd open 0"):
            with self.assertRaises(ValueError, msg=line):
                parse_script(line)

    def test_cd_requests(self):
        api = FakeApi(lambda ms: (720, 400, "menu"))
        with tempfile.TemporaryDirectory() as tmp:
            commands = parse_script("cd load 0 /iso/a b.iso\npause 1000\ncd eject 0\n")
            self.assertEqual(run_session(api, commands, Path(tmp), 0), "done")
            self.assertIn("cd load 0 /iso/a b.iso", api.requests)
            self.assertIn("cd eject 0", api.requests)
            self.assertLess(api.requests.index("cd load 0 /iso/a b.iso"), api.requests.index("cd eject 0"))

    def test_key_names(self):
        self.assertEqual(key_codes("F1"), [0x3b])
        self.assertEqual(key_codes("a"), [0x1e])
        self.assertEqual(key_codes("0"), [0x0b])


class WatchTest(unittest.TestCase):
    def test_text_after_graphics_and_words(self):
        watch = Watch()
        info = {"w": 720, "h": 400, "hard_resets": 0, "soft_resets": 0}
        self.assertEqual(watch.alerts(info, "Starting"), [])
        # an EGA text screen (640x350) is text, whatever its size
        self.assertEqual(watch.alerts(dict(info, w=640, h=350), "BIOS"), [])
        self.assertEqual(watch.alerts(dict(info, w=640, h=480), None), [])
        self.assertEqual(watch.alerts(info, "C:\\>"), ["text mode after a graphical screen"])
        self.assertEqual(watch.alerts(info, "A fatal exception 0E has occurred"),
                         ["text shows 'exception', 'fatal'"])
        # the same words again are not a new alert
        self.assertEqual(watch.alerts(info, "A fatal exception 0E has occurred"), [])
        self.assertEqual(watch.alerts(dict(info, hard_resets=1), ""),
                         ["CPU reset (hard 1, soft 0)"])


class SessionTest(unittest.TestCase):
    def test_wait_key_pause_after(self):
        api = FakeApi(lambda ms: (720, 400, "menu"), match_after=2500)
        with tempfile.TemporaryDirectory() as tmp:
            out = Path(tmp)
            commands = parse_script("wait text 10000 menu\nmark go\nkey enter\npause 1500\n")
            self.assertEqual(run_session(api, commands, out, after_ms=1000), "done")
            self.assertIn("key down 0x1c", api.requests)
            self.assertIn("key up 0x1c", api.requests)
            rows = [json.loads(l) for l in (out / "speed.jsonl").read_text().splitlines()]
            self.assertEqual([r["ms"] for r in rows], [1000, 2000, 2500, 3500, 4000, 5000])
            self.assertEqual(len(worst_seconds(out)), 5)
            self.assertEqual([r.get("mark") for r in rows].count("go"), 1)
            self.assertEqual(worst_seconds(out)[0]["speed"], 100)
            self.assertEqual((out / "alerts.jsonl").read_text(), "")

    def test_wait_times_out(self):
        api = FakeApi(lambda ms: (720, 400, "nothing"))
        with tempfile.TemporaryDirectory() as tmp:
            status = run_session(api, parse_script("wait text 2500 menu\nkey enter"), Path(tmp), 0)
            self.assertEqual(status, "timed out waiting for 'menu'")
            self.assertEqual(api.ms, 2500)
            self.assertFalse(any(r.startswith("key") for r in api.requests))

    def test_alert_saves_screenshot(self):
        api = FakeApi(lambda ms: (640, 480, None) if ms < 2000 else (720, 400, "Windows protection error"))
        with tempfile.TemporaryDirectory() as tmp:
            out = Path(tmp)
            run_session(api, [], out, after_ms=3000)
            alerts = [json.loads(l) for l in (out / "alerts.jsonl").read_text().splitlines()]
            self.assertEqual([a["ms"] for a in alerts], [2000, 2000])
            self.assertTrue(any(r.startswith("screenshot ") and "alert-2000" in r for r in api.requests))


class ScreenshotTest(unittest.TestCase):
    def test_no_frame_is_an_alert_not_an_error(self):
        class NoFrame(FakeApi):
            def request(self, command, allow_failure=False):
                if command.startswith("screenshot "):
                    self.requests.append(command)
                    if not allow_failure:
                        raise RuntimeError("no frame")
                    return {"ok": False, "error": "no frame rendered yet", "ms": self.ms}
                return super().request(command, allow_failure)
        api = NoFrame(lambda ms: (0, 0, None))
        with tempfile.TemporaryDirectory() as tmp:
            out = Path(tmp)
            self.assertEqual(run_session(api, parse_script("pause 1000\nscreenshot early\npause 1000"), out, 0), "done")
            alerts = (out / "alerts.jsonl").read_text()
            self.assertIn("screenshot early: no frame rendered yet", alerts)


class PngRegionTest(unittest.TestCase):
    def setUp(self):
        rand = random.Random(1)
        self.width = 13
        self.pixels = [bytes(rand.randrange(256) for _ in range(self.width * 4)) for _ in range(11)]

    def test_every_filter_and_region(self):
        for colour, bpp in ((2, 3), (6, 4)):
            rows = [p[:self.width * bpp] for p in self.pixels]
            for filters in ([4, 2, 3, 1, 0, 4, 4, 2, 3], [2], [3, 4], [0]):
                png = encode_png(self.width, rows, filters, colour)
                for x, y, w, h in ((0, 0, 13, 11), (5, 7, 3, 2), (12, 10, 1, 1), (0, 3, 1, 8), (4, 0, 9, 1)):
                    want = b"".join(bytes(b for i, b in enumerate(r[x * bpp:(x + w) * bpp]) if i % bpp < 3)
                                    for r in rows[y:y + h])
                    self.assertEqual(png_region(png, x, y, w, h), want, (colour, filters, x, y, w, h))

    def test_digest_and_outside(self):
        png = encode_png(self.width, [p[:self.width * 3] for p in self.pixels], [4, 1])
        rgb = png_region(png, 2, 3, 4, 5)
        self.assertEqual(region_digest(png, (2, 3, 4, 5)), hashlib.sha256(rgb).hexdigest()[:16])
        self.assertIsNone(region_digest(png, (10, 0, 4, 1)))
        self.assertIsNone(region_digest(png, (0, 9, 1, 3)))
        with self.assertRaises(ValueError):
            png_region(b"GIF89a" + png[6:], 0, 0, 1, 1)


class ScreenWaitTest(unittest.TestCase):
    """A 4x4 screen that turns from black to white at 1200 ms (or turn_ms)."""

    class Screens(FakeApi):
        turn_ms = 1200

        def request(self, command, allow_failure=False):
            if command.startswith("screenshot "):
                self.requests.append(command)
                level = 255 if self.ms >= self.turn_ms else 0
                Path(command.split(" ", 1)[1]).write_bytes(encode_png(4, [bytes([level]) * 12] * 4, [0]))
                return {"ok": True, "ms": self.ms, "w": 4, "h": 4}
            return super().request(command, allow_failure)

    white = hashlib.sha256(b"\xff" * 12).hexdigest()[:16]

    def test_lines(self):
        self.assertEqual(parse_script(f"wait screen 60000 1 2 2 2 {self.white}"),
                         [("screen", 60000, (1, 2, 2, 2), self.white)])
        for line in (f"wait screen 60000 1 2 2 {self.white}", "wait screen 60000 1 2 2 2 0123",
                     f"wait screen 60000 -1 2 2 2 {self.white}", f"wait screen 60000 1 2 0 2 {self.white}",
                     f"wait screen soon 1 2 2 2 {self.white}", "wait mem 10 x"):
            with self.assertRaises(ValueError, msg=line):
                parse_script(line)

    def test_waits_for_the_region(self):
        api = self.Screens(lambda ms: (640, 480, None))
        with tempfile.TemporaryDirectory() as tmp:
            out = Path(tmp)
            commands = parse_script(f"wait screen 5000 1 1 2 2 {self.white}\nkey enter")
            self.assertEqual(run_session(api, commands, out, 0), "done")
            self.assertEqual(api.ms, 1250)
            self.assertTrue(all(r == f"step {SCREEN_POLL_MS}" for r in api.requests if r.startswith("step")))
            self.assertIn("key down 0x1c", api.requests)
            self.assertFalse((out / "wait-screen.png").exists())
            rows = [json.loads(l) for l in (out / "speed.jsonl").read_text().splitlines()]
            self.assertEqual([r["ms"] for r in rows], [1000])

    def test_slower_after_a_while(self):
        api = self.Screens(lambda ms: (640, 480, None))
        api.turn_ms = SCREEN_POLL_SLOW_AFTER_MS + 2400
        with tempfile.TemporaryDirectory() as tmp:
            self.assertEqual(run_session(api, parse_script(f"wait screen 60000 0 0 4 4 {hashlib.sha256(b'\xff' * 48).hexdigest()[:16]}"),
                                         Path(tmp), 0), "done")
        steps = [int(r.split()[1]) for r in api.requests if r.startswith("step ")]
        fast = SCREEN_POLL_SLOW_AFTER_MS // SCREEN_POLL_MS
        self.assertEqual(steps, [SCREEN_POLL_MS] * fast + [SCREEN_POLL_SLOW_MS] * 3)
        self.assertEqual(api.ms, SCREEN_POLL_SLOW_AFTER_MS + 3 * SCREEN_POLL_SLOW_MS)

    def test_times_out(self):
        api = self.Screens(lambda ms: (640, 480, None))
        black = hashlib.sha256(bytes(12)).hexdigest()[:16]
        with tempfile.TemporaryDirectory() as tmp:
            status = run_session(api, parse_script(f"wait screen 1000 0 0 2 2 {self.white[:15]}0\nkey enter"),
                                 Path(tmp), 0)
            self.assertTrue(status.startswith("timed out waiting for screen"), status)
            self.assertEqual(api.ms, 1000)
            self.assertNotIn("key down 0x1c", api.requests)
            # Already there: no step at all
            api = self.Screens(lambda ms: (640, 480, None))
            self.assertEqual(run_session(api, parse_script(f"wait screen 1000 0 0 2 2 {black}"), Path(tmp), 0), "done")
            self.assertEqual(api.ms, 0)


class BiosF1Test(unittest.TestCase):
    def test_f1_pressed_once_per_five_seconds(self):
        api = FakeApi(lambda ms: (720, 400, "Press F1 to continue, DEL to enter SETUP"))
        with tempfile.TemporaryDirectory() as tmp:
            run_session(api, [], Path(tmp), after_ms=7000, bios_f1=True)
        self.assertEqual(api.requests.count("key down 0x3b"), 2)

    def test_not_without_the_option(self):
        api = FakeApi(lambda ms: (720, 400, "Press F1 to continue"))
        with tempfile.TemporaryDirectory() as tmp:
            run_session(api, [], Path(tmp), after_ms=3000)
        self.assertNotIn("key down 0x3b", api.requests)


class WipeTest(unittest.TestCase):
    def config(self, tmp, name):
        cfg = Path(tmp) / "86box.cfg"
        cfg.write_text(f"[Hard disks]\nhdd_01_fn = {name}\n")
        return cfg

    def test_raw_image_emptied_at_size(self):
        with tempfile.TemporaryDirectory() as tmp:
            disk = Path(tmp) / "disk.img"
            disk.write_bytes(b"x" * 4096)
            path, size = wipe_hdd(self.config(tmp, "disk.img"), "01")
            self.assertEqual((path, size), (disk, 4096))
            self.assertEqual(disk.read_bytes(), bytes(4096))

    def test_vhd_refused(self):
        with tempfile.TemporaryDirectory() as tmp:
            disk = Path(tmp) / "disk.vhd"
            disk.write_bytes(bytes(1024) + b"conectix" + bytes(504))
            with self.assertRaises(RuntimeError):
                wipe_hdd(self.config(tmp, "disk.vhd"), "01")
            self.assertIn(b"conectix", disk.read_bytes())


if __name__ == "__main__":
    unittest.main()
