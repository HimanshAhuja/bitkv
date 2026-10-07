"""Minimal Redis-protocol client and metrics reader shared by the ops tools.

Standard library only, so the tools run on a bare host or inside a minimal
container without pip.
"""
import socket
import urllib.request


class ServerError(Exception):
    """The server replied with a RESP error ("-ERR ...")."""


class Client:
    def __init__(self, addr="127.0.0.1:6380", timeout=3.0):
        host, port = addr.rsplit(":", 1)
        self.sock = socket.create_connection((host, int(port)), timeout=timeout)
        self.buf = self.sock.makefile("rb")

    def close(self):
        self.buf.close()
        self.sock.close()

    def __enter__(self):
        return self

    def __exit__(self, *exc):
        self.close()

    def do(self, *args):
        out = [b"*%d\r\n" % len(args)]
        for a in args:
            a = a if isinstance(a, bytes) else str(a).encode()
            out.append(b"$%d\r\n%s\r\n" % (len(a), a))
        self.sock.sendall(b"".join(out))
        return self._read()

    def _line(self):
        line = self.buf.readline()
        if not line.endswith(b"\r\n"):
            raise ConnectionError("connection closed mid-reply")
        return line[:-2]

    def _read(self):
        line = self._line()
        kind, rest = line[:1], line[1:]
        if kind == b"+":
            return rest.decode()
        if kind == b"-":
            raise ServerError(rest.decode())
        if kind == b":":
            return int(rest)
        if kind == b"$":
            n = int(rest)
            if n < 0:
                return None
            data = self.buf.read(n + 2)
            return data[:-2].decode(errors="replace")
        if kind == b"*":
            return [self._read() for _ in range(int(rest))]
        raise ConnectionError(f"unexpected reply type {kind!r}")


def read_metrics(base_url="http://127.0.0.1:9121", timeout=3.0):
    """Returns {metric_name_with_labels: float} from a /metrics endpoint."""
    with urllib.request.urlopen(base_url + "/metrics", timeout=timeout) as r:
        text = r.read().decode()
    out = {}
    for line in text.splitlines():
        if line and not line.startswith("#"):
            name, _, value = line.rpartition(" ")
            out[name] = float(value)
    return out
