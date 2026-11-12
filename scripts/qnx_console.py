#!/usr/bin/env python3
"""qnx_console.py - run commands on the QNX 8.0 guest through its serial
console (exposed by QEMU as a TCP socket).

The SDP quickstart image has no sshd running, so the serial console is the
control channel: it carries the boot messages AND the login shell. This helper
logs in as root/root (the password the image ships with), runs the commands and
prints what the guest answered, waiting for the shell prompt after each one.

Usage:
    scripts/qnx_console.py 4555 'uname -sr'
    scripts/qnx_console.py 4555 --timeout 20 'ls /dev/hd*' 'pidin ar | head'
    scripts/qnx_console.py 4555 --wait-login        # just wait for the shell
"""

from __future__ import annotations

import argparse
import socket
import sys
import time

HOST = "127.0.0.1"
USER = "root"
PASSWORD = "root"
PROMPTS = ("]# ", "$ ", "# ")


class Console:
    """One serial-console session: log in, run commands, read the answers."""

    def __init__(self, port: int, timeout: float = 5.0) -> None:
        self.timeout = timeout
        self.sock = socket.create_connection((HOST, port), timeout=5)
        self.sock.settimeout(0.3)
        self.buffer: list[str] = []

    def drain(self, seconds: float) -> str:
        end = time.time() + seconds
        while time.time() < end:
            try:
                data = self.sock.recv(65536)
            except socket.timeout:
                continue
            if not data:
                break
            self.buffer.append(data.decode("utf-8", "replace"))
        return "".join(self.buffer)

    def send(self, line: str, wait: float | None = None) -> str:
        self.sock.sendall((line + "\n").encode())
        return self.drain(self.timeout if wait is None else wait)

    def text(self) -> str:
        return "".join(self.buffer)

    def wait_prompt(self, seconds: float) -> str:
        """Read until the shell prompt comes back (or the deadline passes)."""
        end = time.time() + seconds
        while time.time() < end:
            tail = self.text()[-40:]
            if any(tail.endswith(p) for p in PROMPTS):
                break
            self.drain(0.3)
        return self.text()

    def login(self) -> bool:
        """Reach a shell, logging in if the guest asks for it.

        The guest may still be booting, so this waits for the prompt (or for
        the login banner) instead of giving up after one attempt.
        """
        deadline = time.time() + max(20.0, self.timeout * 6)
        sent_user = False
        while time.time() < deadline:
            self.drain(0.4)
            text = self.text()
            tail = text[-300:]
            if "Password:" in tail and sent_user:
                self.send(PASSWORD, 1.0)
                sent_user = False
                continue
            if "login:" in tail:
                self.send(USER, 1.2)
                sent_user = True
                continue
            if "Login incorrect" in tail:
                return False
            if any(text[-60:].endswith(p) for p in PROMPTS):
                return True
            self.send("", 0.6)
        return False

    def run(self, command: str) -> str:
        before = len(self.text())
        self.send(command)
        self.wait_prompt(self.timeout)
        return clean(self.text()[before:])

    def close(self) -> None:
        try:
            self.sock.close()
        except OSError:
            pass


def clean(text: str) -> str:
    """Strip the terminal control sequences the guest's prompt leaves around."""
    for junk in ("\x1b[?2004h", "\x1b[?2004l", "\x1b[?7h", "\x1b[?7l", "\x1b[?25h",
                 "\x1b[?25l", "\x1b[K", "\x1b[1;31m", "\x1b[1;33m", "\x1b[0m"):
        text = text.replace(junk, "")
    lines = [line for line in text.replace("\r\n", "\n").split("\n") if line.strip()]
    return "\n".join(lines)


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("port", type=int)
    parser.add_argument("commands", nargs="*")
    parser.add_argument("--timeout", type=float, default=5.0,
                        help="seconds to wait for each answer")
    parser.add_argument("--wait-login", action="store_true",
                        help="only wait until the shell answers")
    args = parser.parse_intermixed_args(args=sys.argv[1:])

    console = Console(args.port, args.timeout)
    try:
        if not console.login():
            print("qnx_console: no shell on the console", file=sys.stderr)
            print(clean(console.text()[-600:]), file=sys.stderr)
            return 2
        if args.wait_login:
            print("shell ready")
            return 0
        for command in args.commands:
            print(f"$ {command}")
            print(console.run(command))
        return 0
    finally:
        console.close()


if __name__ == "__main__":
    sys.exit(main())
