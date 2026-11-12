#!/usr/bin/env python3
"""qnx_bridge.py — attach to a QEMU serial unix socket and serve it on TCP.

QEMU's serial chardev is a one-shot stream: a client that connects late sees
nothing. The bench therefore keeps ONE reader attached to the guest for the
whole life of the VM, logs everything to a file, and hands the stream to
whoever asks on the TCP port (scripts/qnx_console.py). A TCP client sees the
guest's output from the moment it connects.

Usage: qnx_bridge.py <unix-socket> <tcp-port> <log-file>
"""

from __future__ import annotations

import os
import socket
import sys
import time


class Clients:
    """Every TCP client gets its own copy of the stream, from its own start."""

    def __init__(self) -> None:
        self.peers: list[tuple[socket.socket, bytearray]] = []

    def add(self, sock: socket.socket) -> None:
        self.peers.append((sock, bytearray()))

    def drop(self, sock: socket.socket) -> None:
        self.peers = [(s, b) for s, b in self.peers if s is not sock]
        try:
            sock.close()
        except OSError:
            pass

    def broadcast(self, data: bytes) -> None:
        for sock, buf in list(self.peers):
            buf.extend(data)
            try:
                if buf:
                    sock.sendall(bytes(buf))
                    buf.clear()
            except OSError:
                self.drop(sock)


def main() -> int:
    if len(sys.argv) < 4:
        print(__doc__)
        return 2
    path, port, log_path = sys.argv[1], int(sys.argv[2]), sys.argv[3]
    for _ in range(100):
        if os.path.exists(path):
            break
        time.sleep(0.1)

    guest = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
    guest.connect(path)
    guest.settimeout(0.3)

    srv = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    srv.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    srv.bind(("127.0.0.1", port))
    srv.listen(8)
    srv.settimeout(0.3)

    clients = Clients()
    with open(log_path, "ab") as log:
        while True:
            try:
                conn, _ = srv.accept()
                clients.add(conn)
                continue
            except socket.timeout:
                pass
            except OSError:
                break
            try:
                data = guest.recv(65536)
            except socket.timeout:
                continue
            except OSError:
                break
            if not data:
                break
            log.write(data)
            log.flush()
            clients.broadcast(data)
    return 0


if __name__ == "__main__":
    sys.exit(main())
