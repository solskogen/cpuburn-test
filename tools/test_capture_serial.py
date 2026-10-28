from contextlib import redirect_stderr, redirect_stdout
import io
import json
from pathlib import Path
import socket
import tempfile
import threading
import unittest

from capture_serial import capture, endpoint, main


class CaptureTests(unittest.TestCase):
    def run_capture(self, replies, **kwargs):
        client, server = socket.socketpair()
        commands = []
        errors = []

        def serve():
            try:
                with server, server.makefile("rwb") as wire:
                    for reply in replies:
                        line = wire.readline()
                        if not line:
                            break
                        commands.append(line.decode("ascii").strip())
                        if reply is None:
                            break
                        wire.write(json.dumps(reply).encode("ascii") + b"\n")
                        wire.flush()
            except OSError as error:
                errors.append(error)

        thread = threading.Thread(target=serve)
        thread.start()
        output, display = io.BytesIO(), io.StringIO()
        try:
            with client:
                capture(client, output, display, timeout=2, **kwargs)
        finally:
            thread.join(timeout=3)
            self.assertFalse(thread.is_alive())
        self.assertFalse(errors)
        return commands, output.getvalue(), display.getvalue()

    @staticmethod
    def serial(data, position):
        return {"ok": True, "lost": 0, "data": data.hex(), "next": position}

    def test_fragmented_done_and_failed_tests_are_retained(self):
        first = b"TEST example status=FAIL\r\nSUMMARY fail=1\r\nDO"
        last = b"NE\r\n"
        commands, raw, text = self.run_capture([
            {"ok": True}, self.serial(first, len(first)), {"ok": True},
            self.serial(last, len(first + last)),
        ])
        self.assertEqual(raw, first + last)
        self.assertEqual(text, raw.decode("cp437"))
        self.assertEqual(commands, [
            "hold", "serial read 1 0", "step 250", f"serial read 1 {len(first)}"])

    def test_authentication_and_existing_done(self):
        commands, raw, _ = self.run_capture([
            {"ok": True}, {"ok": True}, self.serial(b"DONE\n", 5),
        ], token="a" * 64, port=2)
        self.assertEqual(commands, ["auth " + "a" * 64, "hold", "serial read 2 0"])
        self.assertEqual(raw, b"DONE\n")

    def test_incomplete_done_is_not_completion(self):
        with self.assertRaisesRegex(RuntimeError, "closed"):
            self.run_capture([
                {"ok": True}, self.serial(b"DONE", 4), {"ok": True}, None])

    def test_overflow_is_error(self):
        with self.assertRaisesRegex(RuntimeError, "lost bytes"):
            self.run_capture([{"ok": True}, {"ok": True, "lost": 10}])

    def test_command_error(self):
        with self.assertRaisesRegex(RuntimeError, "backend"):
            self.run_capture([{"ok": True}, {"ok": False, "error": "no backend"}])

    def test_debugger_stop_is_error(self):
        with self.assertRaisesRegex(RuntimeError, "execution stopped"):
            self.run_capture([
                {"ok": True}, self.serial(b"", 0),
                {"ok": True, "stopped": "debugger"}])

    def test_invalid_position_is_error(self):
        with self.assertRaisesRegex(RuntimeError, "positions"):
            self.run_capture([{"ok": True}, self.serial(b"x", 8)])

    def test_endpoint_transports(self):
        self.assertEqual(endpoint("unix:/tmp/vm.sock"), ("unix", "/tmp/vm.sock"))
        self.assertEqual(endpoint("tcp:7086"), ("tcp", ("127.0.0.1", 7086)))
        self.assertEqual(endpoint("tcp:localhost:7086"), ("tcp", ("localhost", 7086)))

    def test_cli_tcp_and_unix(self):
        transports = ["tcp"]
        if hasattr(socket, "AF_UNIX"):
            transports.append("unix")
        for transport in transports:
            with self.subTest(transport=transport), tempfile.TemporaryDirectory() as directory:
                root = Path(directory)
                family = socket.AF_INET if transport == "tcp" else socket.AF_UNIX
                with socket.socket(family, socket.SOCK_STREAM) as listener:
                    listener.bind(("127.0.0.1", 0) if transport == "tcp"
                                  else str(root / "vm.sock"))
                    listener.listen(1)
                    listener.settimeout(3)
                    commands = []
                    failures = []

                    def serve():
                        try:
                            connection, _ = listener.accept()
                            with connection, connection.makefile("rwb") as wire:
                                replies = [
                                    {"ok": True}, {"ok": True},
                                    self.serial(b"SUMMARY fail=1\r\nDONE\r\n", 22),
                                ] if transport == "tcp" else [
                                    {"ok": True}, self.serial(b"DONE\n", 5)]
                                for reply in replies:
                                    commands.append(wire.readline().decode().strip())
                                    wire.write(json.dumps(reply).encode() + b"\n")
                                    wire.flush()
                        except OSError as error:
                            failures.append(error)

                    thread = threading.Thread(target=serve)
                    thread.start()
                    address = (f"tcp:127.0.0.1:{listener.getsockname()[1]}"
                               if transport == "tcp" else f"unix:{root / 'vm.sock'}")
                    args = [address, "--output", str(root / "serial.log")]
                    if transport == "tcp":
                        (root / "token").write_text("a" * 64 + "\n")
                        args += ["--token-file", str(root / "token")]
                    with redirect_stdout(io.StringIO()), redirect_stderr(io.StringIO()):
                        result = main(args)
                    thread.join(timeout=3)
                    self.assertFalse(thread.is_alive())
                    self.assertFalse(failures)
                    self.assertEqual(result, 0)
                    self.assertTrue((root / "serial.log").read_bytes().endswith(b"DONE\n")
                                    or (root / "serial.log").read_bytes().endswith(b"DONE\r\n"))
                    self.assertEqual(commands[-2:], ["hold", "serial read 1 0"])


if __name__ == "__main__":
    unittest.main()
