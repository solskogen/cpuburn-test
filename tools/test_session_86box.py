import json
from pathlib import Path
import tempfile
import unittest

from session_86box import Watch, key_codes, parse_script, run_session, wipe_hdd, worst_seconds


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
