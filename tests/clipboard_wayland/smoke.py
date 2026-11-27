#!/usr/bin/env python3
"""Exercise the real helper on an isolated Wayland session (never a user's desktop)."""

import argparse
import base64
from collections import deque
import json
import os
from pathlib import Path
import queue
import struct
import subprocess
import threading
import time
import uuid


def b64(data):
    return base64.b64encode(data).decode("ascii")


def paste():
    result = subprocess.run(
        ["wl-paste", "--no-newline", "--type", "text/plain;charset=utf-8"],
        stdout=subprocess.PIPE, stderr=subprocess.DEVNULL, timeout=2,
    )
    return result.stdout if result.returncode == 0 else None


class Helper:
    def __init__(self, executable, data_control):
        env = dict(os.environ, QT_QPA_PLATFORM="wayland", WAYLAND_DEBUG="1",
                   QT_WAYLAND_USE_DATA_CONTROL=data_control)
        self.process = subprocess.Popen(
            [str(executable)], env=env, stdin=subprocess.PIPE,
            stdout=subprocess.PIPE, stderr=subprocess.PIPE,
        )
        self.events = queue.Queue()
        self.log = deque(maxlen=30)
        self.ready = self.started = self.pong = False
        self.bound_seat = self.bound_data_control = False
        self.frames = []
        for name, pipe in (("stdout", self.process.stdout), ("stderr", self.process.stderr)):
            threading.Thread(target=self.read, args=(name, pipe), daemon=True).start()

    def read(self, name, pipe):
        for line in pipe:
            self.events.put((name, line.decode("utf-8", errors="replace")))

    def send(self, kind, sequence, **fields):
        message = dict(version=1, type=kind, sequence=sequence, **fields)
        self.process.stdin.write((json.dumps(message) + "\n").encode())
        self.process.stdin.flush()

    def wait(self, condition, timeout):
        deadline = time.monotonic() + timeout
        while True:
            if self.process.poll() is not None:
                raise RuntimeError(f"helper exited unexpectedly: {self.process.returncode}")
            if condition():
                return True
            remaining = deadline - time.monotonic()
            if remaining <= 0:
                return False
            try:
                channel, line = self.events.get(timeout=min(remaining, 0.05))
            except queue.Empty:
                continue
            if channel == "stderr":
                self.log.append(line.rstrip())
                self.started |= "ClipboardSync: started (" in line
                self.bound_seat |= '.bind(' in line and '"wl_seat"' in line
                self.bound_data_control |= (
                    '.bind(' in line and '"zwlr_data_control_manager_v1"' in line
                )
                continue
            message = json.loads(line)
            if message["type"] == "error":
                raise RuntimeError(f"helper error: {message['code']}: {message['text']}")
            self.ready |= message["type"] == "ready" and message["sequence"] == 1
            self.pong |= message["type"] == "pong" and message["sequence"] == 3
            if message["type"] == "localFrame":
                frame = base64.b64decode(message["payload"], validate=True)
                if len(frame) < 10:
                    raise RuntimeError("truncated clipboard frame")
                version, kind, _, size = struct.unpack("<BBII", frame[:10])
                if version != 1 or kind != 1 or size != len(frame) - 10:
                    raise RuntimeError("unexpected clipboard frame format")
                self.frames.append(frame[10:])

    def close(self):
        if self.process.poll() is None:
            self.process.stdin.close()
            try:
                self.process.wait(timeout=4)
            except subprocess.TimeoutExpired:
                self.process.kill()
                self.process.wait(timeout=2)
        for pipe in (self.process.stdout, self.process.stderr):
            pipe.close()
        return self.process.returncode


def check(condition, message):
    if not condition:
        raise RuntimeError(message)


def run_round(executable, host, value, index, timeout):
    helper = Helper(executable, value)
    owner = None
    try:
        helper.send("configure", 1, host=host)
        check(helper.wait(lambda: helper.ready and helper.started, timeout),
              "helper did not finish configuration and engine startup")
        check(helper.bound_seat, "helper did not bind a real Wayland seat")
        enabled = value == "1"
        check(helper.bound_data_control == enabled,
              "unexpected data-control binding (check Qt version/compositor protocol)")

        marker = f"round {index}: {uuid.uuid4().hex} 中文 🌙\n".encode()
        local = marker + b"local to helper"
        remote = marker + b"helper to local"
        owner = subprocess.Popen(
            ["wl-copy", "--foreground", "--type", "text/plain;charset=utf-8"],
            stdin=subprocess.PIPE, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL,
        )
        owner.stdin.write(local)
        owner.stdin.close()
        check(helper.wait(lambda: paste() == local, timeout), "wl-copy did not set the clipboard")
        outbound = helper.wait(lambda: local in helper.frames, timeout)
        check(outbound == enabled, f"unexpected local → helper result: {outbound}")

        wire = struct.pack("<BBII", 1, 1, 0, len(remote)) + remote
        helper.send("hostFrame", 2, payload=b64(wire))
        inbound = helper.wait(lambda: paste() == remote, timeout)
        check(inbound == enabled, f"unexpected helper → local result: {inbound}")
        if not enabled:
            check(paste() == local, "disabled helper unexpectedly changed the clipboard")
        helper.send("ping", 3)
        check(helper.wait(lambda: helper.pong, timeout), "helper stopped responding")
        print(f"round {index}: data-control={value}, outbound={outbound}, inbound={inbound}", flush=True)
    except Exception:
        print("\n".join(helper.log), flush=True)
        raise
    finally:
        if owner is not None and owner.poll() is None:
            owner.terminate()
            try:
                owner.wait(timeout=2)
            except subprocess.TimeoutExpired:
                owner.kill()
                owner.wait(timeout=2)
        exit_code = helper.close()
    check(exit_code == 0, f"helper did not exit cleanly: {exit_code}")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("helper", type=Path)
    parser.add_argument("--timeout", type=float, default=3)
    args = parser.parse_args()
    check(bool(os.environ.get("WAYLAND_DISPLAY")), "an isolated WAYLAND_DISPLAY is required")
    check(bool(os.environ.get("XDG_RUNTIME_DIR")), "XDG_RUNTIME_DIR is required")
    fixtures = Path(__file__).resolve().parents[1] / "clipboard_payload_routing" / "fixtures"
    # Public fixtures only. Inline text never opens a network connection.
    cert = b64((fixtures / "server.pem").read_bytes())
    host = dict(address="127.0.0.1", httpsPort=9, serverCertPem=cert, clientCertPem=cert,
                clientKeyPem=b64((fixtures / "server-key.pem").read_bytes()))
    for index, value in enumerate(("0", "1", "0"), start=1):
        run_round(args.helper.resolve(), host, value, index, args.timeout)
    print("clipboard_wayland=passed", flush=True)


if __name__ == "__main__":
    main()
