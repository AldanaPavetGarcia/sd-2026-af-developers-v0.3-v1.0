"""Minimal MQTT 3.1.1 subscriber for the firmware integration tests.

Only what the tests need (CONNECT, SUBSCRIBE with QoS 1, PUBLISH + PUBACK), so the tests run
with the Python standard library alone, in CI and on any laptop.
"""

import socket
import struct
import threading
import time

CONNECT, CONNACK, PUBLISH, PUBACK, SUBSCRIBE, SUBACK = 1, 2, 3, 4, 8, 9
PROTOCOL_LEVEL_311 = 4
CLEAN_SESSION = 0x02
SUBSCRIBE_FLAGS = 0x02
QOS_1 = 1


def _utf8(text):
    data = text.encode()
    return struct.pack("!H", len(data)) + data


def _packet(kind, flags, body):
    length, encoded = len(body), bytearray()
    while True:
        byte, length = length % 128, length // 128
        encoded.append(byte | (0x80 if length else 0))
        if not length:
            break
    return bytes([kind << 4 | flags]) + bytes(encoded) + body


class Subscriber:
    """Collects (receive_time, topic, payload) for every message on a topic filter."""

    def __init__(self, host, port, topic_filter, client_id="eop-firmware-test"):
        self.messages = []
        self.lock = threading.Lock()
        self.sock = socket.create_connection((host, port), timeout=10)
        try:
            self._handshake(topic_filter, client_id)
        except (ConnectionError, OSError):
            self.sock.close()
            raise
        self.sock.settimeout(None)
        self.thread = threading.Thread(target=self._loop, daemon=True)
        self.thread.start()

    def _handshake(self, topic_filter, client_id):
        # Keep-alive 0: the broker never expects a ping from this short-lived test client.
        body = _utf8("MQTT") + bytes([PROTOCOL_LEVEL_311, CLEAN_SESSION]) + struct.pack("!H", 0)
        self.sock.sendall(_packet(CONNECT, 0, body + _utf8(client_id)))
        kind, _, payload = self._read()
        if kind != CONNACK or payload[1] != 0:
            raise ConnectionError(f"CONNACK refused: {payload!r}")
        sub = struct.pack("!H", 1) + _utf8(topic_filter) + bytes([QOS_1])
        self.sock.sendall(_packet(SUBSCRIBE, SUBSCRIBE_FLAGS, sub))
        kind, _, payload = self._read()
        if kind != SUBACK or payload[-1] != QOS_1:
            raise ConnectionError(f"SUBACK refused: {payload!r}")

    def _read_exact(self, n):
        data = b""
        while len(data) < n:
            chunk = self.sock.recv(n - len(data))
            if not chunk:
                raise ConnectionError("broker closed the connection")
            data += chunk
        return data

    def _read(self):
        header = self._read_exact(1)[0]
        length, shift = 0, 0
        while True:
            byte = self._read_exact(1)[0]
            length |= (byte & 0x7F) << shift
            shift += 7
            if not byte & 0x80:
                break
        return header >> 4, header & 0x0F, self._read_exact(length)

    def _loop(self):
        try:
            while True:
                kind, flags, body = self._read()
                if kind != PUBLISH:
                    continue
                received = time.time()
                topic_len = struct.unpack("!H", body[:2])[0]
                topic = body[2:2 + topic_len].decode()
                offset = 2 + topic_len
                if (flags >> 1) & 0x03:
                    packet_id = body[offset:offset + 2]
                    offset += 2
                    self.sock.sendall(_packet(PUBACK, 0, packet_id))
                with self.lock:
                    self.messages.append((received, topic, body[offset:].decode()))
        except (ConnectionError, OSError):
            return

    def snapshot(self):
        with self.lock:
            return list(self.messages)

    def close(self):
        try:
            self.sock.close()
        except OSError:
            pass
