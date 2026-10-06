#!/usr/bin/env python3
"""
US-203 AC8: "The integration does not introduce breaking changes to any existing v0.1 messages."

The AC names `buf breaking` as the verification tool, but that's for protobuf -- this protocol
is newline-delimited JSON/plain-text (ADR-003), so it doesn't apply. Instead, this replays the
full v0.1 message corpus (REGISTER, HEARTBEAT, QUERY_NODE, LIST_NODES, an unknown command, and
EXIT) against a real eop_server and asserts each response keeps its documented shape -- not just
that the command still "works", but that the literal wire format nothing else is allowed to
silently drift on. Runs in CI on every change, unlike a one-time protobuf schema diff would.
"""

import os
import socket
import subprocess
import sys
import time


def free_port():
    sock = socket.socket()
    sock.bind(("127.0.0.1", 0))
    port = sock.getsockname()[1]
    sock.close()
    return port


def recv_line(sock, timeout=5):
    sock.settimeout(timeout)
    data = b""
    while not data.endswith(b"\n"):
        chunk = sock.recv(1)
        if not chunk:
            break
        data += chunk
    return data.decode(errors="replace").strip()


def wait_for_port(port, timeout=8):
    deadline = time.time() + timeout
    while time.time() < deadline:
        try:
            with socket.create_connection(("127.0.0.1", port), timeout=0.3):
                return True
        except OSError:
            time.sleep(0.1)
    return False


def send_one(port, message, timeout=5):
    """Opens a fresh connection, discards the WELCOME banner, sends one line, reads one
    response line. Matches how each v0.1 command is documented as a standalone request."""
    with socket.create_connection(("127.0.0.1", port), timeout=timeout) as sock:
        sock.settimeout(timeout)
        welcome = recv_line(sock, timeout=1)
        sock.sendall((message + "\n").encode())
        response = recv_line(sock, timeout=timeout)
        return welcome, response


def check(condition, description):
    if not condition:
        print(f"ERROR: contract broken -- {description}")
        sys.exit(1)
    print(f"OK: {description}")


def main():
    if len(sys.argv) != 2:
        print("Usage: v01_protocol_backward_compat.py <eop_server_binary>")
        return 2

    server_bin = sys.argv[1]
    if not os.path.exists(server_bin):
        print(f"ERROR: eop_server binary not found: {server_bin}")
        return 2

    port = free_port()
    env = os.environ.copy()
    env["EOP_PORT"] = str(port)
    env["EOP_TRACING_ENABLED"] = "0"

    server = subprocess.Popen(
        [server_bin], env=env, stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True
    )

    try:
        if not wait_for_port(port):
            print("ERROR: EOP server did not start")
            return 1

        # 1. WELCOME banner shape on connect (every command below relies on this being present).
        welcome, _ = send_one(port, "HEARTBEAT")
        check(welcome.startswith("WELCOME Server v1.0"), "WELCOME banner keeps its v1.0 prefix")
        check("SESSION_ID" in welcome, "WELCOME banner still carries a SESSION_ID")

        # 2. REGISTER -- successful. Kept open deliberately: per ADR-018, re-registering the
        # same node_id after its original connection already closed is a legitimate RECONNECT,
        # not a duplicate -- ERROR_DUPLICATE only fires while the original session is still
        # live, so the connection has to stay open for step 3 below to exercise that path.
        register_conn = socket.create_connection(("127.0.0.1", port), timeout=5)
        register_conn.settimeout(5)
        recv_line(register_conn, timeout=1)  # WELCOME
        register_conn.sendall(
            b"REGISTER node_id=compat-1 bunker_name=Compat ip_address=10.0.0.1 capacity=5\n"
        )
        resp = recv_line(register_conn, timeout=5)
        check(resp.startswith("ACK Server v1.0"), "successful REGISTER keeps its ACK prefix")
        check("SESSION_ID" in resp, "successful REGISTER response still carries a SESSION_ID")

        # 3. REGISTER -- duplicate node_id while the original session is still online.
        _, resp = send_one(
            port, "REGISTER node_id=compat-1 bunker_name=Compat ip_address=10.0.0.1 capacity=5"
        )
        check(
            resp.startswith("ERROR_DUPLICATE"),
            "duplicate REGISTER (original session still online) keeps its ERROR_DUPLICATE prefix",
        )
        register_conn.close()

        # 4. REGISTER -- malformed (missing required fields).
        _, resp = send_one(port, "REGISTER node_id=compat-missing-fields")
        check(
            resp.startswith("ERROR_BAD_REQUEST"),
            "malformed REGISTER keeps its ERROR_BAD_REQUEST prefix",
        )

        # 5. HEARTBEAT.
        _, resp = send_one(port, "HEARTBEAT")
        check(resp == "ACK HEARTBEAT", "HEARTBEAT response is exactly 'ACK HEARTBEAT'")

        # 6. QUERY_NODE -- existing node. Its response is the raw serialized record, no ACK
        # prefix (unlike REGISTER/HEARTBEAT) -- that's the actual documented shape.
        _, resp = send_one(port, "QUERY_NODE node_id=compat-1")
        check(resp.startswith("node_id=compat-1"), "QUERY_NODE response starts with node_id=")
        check(
            all(field in resp for field in ("bunker_name=", "ip_address=", "capacity=", "status=")),
            "QUERY_NODE response still serializes all record fields",
        )

        # 7. QUERY_NODE -- unknown node.
        _, resp = send_one(port, "QUERY_NODE node_id=does-not-exist")
        check(
            resp.startswith("ERROR_NOT_FOUND"),
            "QUERY_NODE for an unknown node keeps its ERROR_NOT_FOUND prefix",
        )

        # 8. LIST_NODES.
        with socket.create_connection(("127.0.0.1", port), timeout=5) as sock:
            sock.settimeout(5)
            recv_line(sock, timeout=1)  # WELCOME
            sock.sendall(b"LIST_NODES\n")
            lines = []
            while True:
                line = recv_line(sock, timeout=2)
                if not line:
                    break
                lines.append(line)
                if line == "END_LIST":
                    break
        check(lines and lines[-1] == "END_LIST", "LIST_NODES response is still END_LIST-terminated")
        check(any("compat-1" in line for line in lines), "LIST_NODES still lists a registered node")

        # 9. Unknown command.
        _, resp = send_one(port, "NOT_A_REAL_COMMAND")
        check(
            resp.startswith("ERROR_BAD_REQUEST"),
            "an unrecognized command still returns ERROR_BAD_REQUEST, not a crash/hang",
        )

        # 10. Graceful exit -- server must close the connection cleanly, not hang or error.
        with socket.create_connection(("127.0.0.1", port), timeout=5) as sock:
            sock.settimeout(5)
            recv_line(sock, timeout=1)  # WELCOME
            sock.sendall(b"quit\n")
            closed = sock.recv(1) == b""
        check(closed, "'quit' still closes the connection cleanly (no response, no hang)")

        print("OK: full v0.1 message corpus keeps its documented wire shape")
        return 0

    finally:
        if server.poll() is None:
            server.terminate()
            try:
                server.wait(timeout=3)
            except subprocess.TimeoutExpired:
                server.kill()
                server.wait(timeout=3)


if __name__ == "__main__":
    sys.exit(main())
