#!/usr/bin/env python3
"""Capture COM output from an already running, automation-enabled 86Box."""

import argparse
import json
import math
from pathlib import Path
import re
import socket
import sys
import time


def positive(value):
    number = float(value)
    if not math.isfinite(number) or number <= 0:
        raise argparse.ArgumentTypeError("must be finite and positive")
    return number


def positive_int(value):
    number = int(value)
    if number <= 0:
        raise argparse.ArgumentTypeError("must be positive")
    return number


def endpoint(value):
    if value.startswith("unix:") and value[5:]:
        return "unix", value[5:]
    if value.startswith("tcp:"):
        host, separator, port = value[4:].rpartition(":")
        if not separator:
            host, port = "127.0.0.1", value[4:]
        try:
            port = int(port)
            if not host or not 1 <= port <= 65535:
                raise ValueError
        except ValueError:
            raise argparse.ArgumentTypeError("use tcp:PORT or tcp:HOST:PORT") from None
        return "tcp", (host, port)
    raise argparse.ArgumentTypeError("use unix:PATH or tcp:[HOST:]PORT")


def capture(connection, output, display, *, token=None, port=1, step_ms=250,
            timeout=3600, request_timeout=120):
    """Capture through a complete DONE line; leave the VM held, even on errors."""
    deadline = time.monotonic() + timeout
    with connection.makefile("rwb") as wire:
        def request(command):
            remaining = deadline - time.monotonic()
            if remaining <= 0:
                raise TimeoutError("capture deadline exceeded; inspect the VM")
            connection.settimeout(min(request_timeout, remaining))
            wire.write((command + "\n").encode("ascii"))
            wire.flush()
            line = wire.readline()
            if not line:
                raise RuntimeError("86Box closed the automation connection")
            reply = json.loads(line)
            if not isinstance(reply, dict) or reply.get("ok") is not True:
                # Do not expose an authentication token through an error reply.
                if command.startswith("auth "):
                    raise RuntimeError("automation authentication failed")
                raise RuntimeError(f"automation command failed: {reply}")
            return reply

        if token is not None:
            request("auth " + token)
        request("hold")
        position = 0
        tail = b""
        while True:
            reply = request(f"serial read {port} {position}")
            if reply.get("lost") != 0:
                raise RuntimeError("COM capture lost bytes; restart with smaller --step-ms")
            data = bytes.fromhex(reply["data"])
            next_position = reply["next"]
            if (not isinstance(next_position, int)
                    or next_position != position + len(data)):
                raise RuntimeError("invalid serial byte positions in automation reply")
            position = next_position
            output.write(data)
            output.flush()
            display.write(data.decode("cp437"))
            display.flush()
            tail = (tail + data)[-65536:]
            if re.search(rb"(?m)^DONE\r?\n", tail):
                return
            step = request(f"step {step_ms}")
            if step.get("stopped"):
                raise RuntimeError(f"guest execution stopped: {step}")


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__, epilog=(
        "Leaves the VM held. Exit 0 means DONE was captured, not that tests passed. "
        "Inspect SUMMARY or run tools/perf_report.py. Output must not already exist."))
    parser.add_argument("endpoint", type=endpoint, help="unix:PATH or tcp:[HOST:]PORT")
    parser.add_argument("--token-file", type=Path, help="required for authenticated TCP")
    parser.add_argument("--output", type=Path, default=Path("serial.log"))
    parser.add_argument("--port", type=positive_int, default=1, help="COM port number (default 1)")
    parser.add_argument("--step-ms", type=positive_int, default=250)
    parser.add_argument("--timeout", type=positive, default=3600, help="host seconds (default 3600)")
    parser.add_argument("--request-timeout", type=positive, default=120,
                        help="maximum seconds per command (default 120)")
    args = parser.parse_args(argv)
    try:
        token = args.token_file.read_text(encoding="ascii").strip() if args.token_file else None
        if token is not None and not re.fullmatch(r"[0-9a-fA-F]{64}", token):
            raise ValueError("token file must contain the 64-digit hexadecimal access token")
        transport, address = args.endpoint
        if transport == "unix":
            connection = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
        else:
            connection = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
        with connection:
            connection.settimeout(min(args.timeout, args.request_timeout))
            connection.connect(address)
            with args.output.open("xb") as output:
                capture(connection, output, sys.stdout, token=token, port=args.port,
                        step_ms=args.step_ms, timeout=args.timeout,
                        request_timeout=args.request_timeout)
        print(f"\nCaptured DONE to {args.output}; inspect SUMMARY for failures. "
              "VM remains held.", file=sys.stderr)
        return 0
    except (OSError, ValueError, RuntimeError, KeyError, TypeError) as error:
        print(f"capture_serial: {error}", file=sys.stderr)
        return 1
    except KeyboardInterrupt:
        print("\ncapture_serial: interrupted; partial log retained, VM not released",
              file=sys.stderr)
        return 130


if __name__ == "__main__":
    sys.exit(main())
