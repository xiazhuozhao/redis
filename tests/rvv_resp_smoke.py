#!/usr/bin/env python3
"""Protocol and fragmentation smoke test for an RVV Redis server."""
import argparse
import socket


def command(*parts):
    encoded = [p if isinstance(p, bytes) else str(p).encode() for p in parts]
    return (b"*%d\r\n" % len(encoded) +
            b"".join(b"$%d\r\n%s\r\n" % (len(p), p) for p in encoded))


class RespReader:
    def __init__(self, sock):
        self.sock = sock
        self.buf = bytearray()

    def read_exact(self, length):
        while len(self.buf) < length:
            chunk = self.sock.recv(65536)
            if not chunk:
                raise EOFError("server closed the connection")
            self.buf.extend(chunk)
        result = bytes(self.buf[:length])
        del self.buf[:length]
        return result

    def line(self):
        while True:
            end = self.buf.find(b"\r\n")
            if end >= 0:
                result = bytes(self.buf[:end])
                del self.buf[:end + 2]
                return result
            self.buf.extend(self.sock.recv(65536))

    def read(self):
        kind = self.read_exact(1)
        if kind in (b"+", b"-", b":", b",", b"("):
            value = self.line()
            return (kind, int(value) if kind == b":" else value)
        if kind in (b"_",):
            assert self.line() == b""
            return (kind, None)
        if kind in (b"#",):
            return (kind, self.line() == b"t")
        if kind in (b"$", b"!", b"="):
            length = int(self.line())
            if length == -1:
                return (kind, None)
            value = self.read_exact(length)
            assert self.read_exact(2) == b"\r\n"
            return (kind, value)
        if kind in (b"*", b"~", b">"):
            length = int(self.line())
            return (kind, None if length == -1 else [self.read() for _ in range(length)])
        if kind == b"%":
            length = int(self.line())
            return (kind, [(self.read(), self.read()) for _ in range(length)])
        raise AssertionError(f"unknown RESP type {kind!r}")


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--host", default="127.0.0.1")
    parser.add_argument("--port", type=int, default=6379)
    args = parser.parse_args()

    with socket.create_connection((args.host, args.port), timeout=10) as sock:
        reader = RespReader(sock)
        sock.sendall(b"PING\r\n")
        assert reader.read() == (b"+", b"PONG")

        binary = bytes(range(256)) * 4096
        packet = command("SET", "rvv:binary", binary)
        for start in range(0, len(packet), 37):
            sock.sendall(packet[start:start + 37])
        assert reader.read() == (b"+", b"OK")
        sock.sendall(command("GET", "rvv:binary"))
        assert reader.read() == (b"$", binary)

        sock.sendall(command("DEL", "rvv:counter") + command("INCR", "rvv:counter"))
        assert reader.read()[0] == b":"
        assert reader.read() == (b":", 1)

        pipeline = b"".join(command("ECHO", f"value-{i}") for i in range(256))
        sock.sendall(pipeline)
        for i in range(256):
            assert reader.read() == (b"$", f"value-{i}".encode())

        sock.sendall(command("GET", "rvv:missing"))
        assert reader.read() == (b"$", None)
        sock.sendall(command("NO-SUCH-RVV-COMMAND"))
        assert reader.read()[0] == b"-"

        sock.sendall(command("HELLO", 3))
        assert reader.read()[0] == b"%"
        sock.sendall(command("SADD", "rvv:set", "a", "b") + command("SMEMBERS", "rvv:set"))
        assert reader.read()[0] == b":"
        assert reader.read()[0] == b"~"

    print("RESP2/RESP3, binary bulk, fragmentation, and pipeline checks passed")


if __name__ == "__main__":
    main()
