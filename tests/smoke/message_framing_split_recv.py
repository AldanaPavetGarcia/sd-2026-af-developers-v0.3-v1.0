#!/usr/bin/env python3

"""Regression test for issue #55: messages split across multiple recv() calls.

ADR-003 frames messages with a trailing '\\n', but a single recv() returns whatever has arrived,
not a whole message. The server used to dispatch any leftover without a '\\n' as if it were a
complete command, so a payload larger than its read buffer (MESSAGE_BUFFER_SIZE, 256 bytes) was
split into two bogus commands: a truncated prefix and a garbage tail.

Each case below sends a message the server cannot possibly read in one recv(), then asserts it is
still handled as exactly one complete command.
"""

import json
import os
import socket
import subprocess
import sys
import time

SERVER_READ_BUFFER_BYTES = 256

# El primer tope del buffer de reensamblado. Un ANALYZE_GRAPH válido de 100.000 nodos pesa 13 MiB,
# así que este tope rechazaba trabajo que el motor sí acepta; ahora el tope es EOP_MAX_REQUEST_BYTES
# (64 MiB por defecto). El caso 5 manda un pedido más grande que el tope viejo.
FIRST_PENDING_INPUT_CAP_BYTES = 8 * 1024 * 1024


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


def discard_welcome(sock):
    sock.settimeout(0.5)
    try:
        while True:
            if not sock.recv(4096):
                break
    except socket.timeout:
        pass
    sock.settimeout(5)


def connect(port):
    sock = socket.create_connection(("127.0.0.1", port), timeout=5)
    discard_welcome(sock)
    return sock


def check(label, condition, detail=""):
    if condition:
        print(f"  OK: {label}")
        return True

    print(f"  FAIL: {label} {detail}")
    return False


def register_command(node_id, bunker_name, ip_address="10.0.0.1", capacity=5):
    """Builds a REGISTER exactly as eopServer.cpp's sscanf expects it (4 fields, %63s each)."""
    return (
        f"REGISTER node_id={node_id} bunker_name={bunker_name} "
        f"ip_address={ip_address} capacity={capacity}\n"
    )


def analyze_graph_envelope(request_id, node_count):
    """Builds an ANALYZE_GRAPH envelope large enough to span several recv() calls."""
    nodes = [f"N{i}" for i in range(node_count)]
    edges = [{"from": nodes[i], "to": nodes[i + 1], "weight": 1} for i in range(len(nodes) - 1)]
    request = {
        "type": "ANALYZE_GRAPH",
        "request_id": request_id,
        "payload": {
            "nodes": nodes,
            "edges": edges,
            "algorithms": ["connected_components"],
        },
    }
    return json.dumps(request, separators=(",", ":")) + "\n"


def case_many_times_the_buffer(port):
    """A payload many times the read buffer needs many recv() calls before its '\\n' shows up."""
    print("Case 1: single send() many times larger than the read buffer")

    # ~8 KB: over 30 read-buffer's worth, so the '\n' only appears on the last of many recv()s.
    payload = analyze_graph_envelope("framing-huge-1", node_count=250)

    passed = check(
        "payload spans many read buffers",
        len(payload) > SERVER_READ_BUFFER_BYTES * 20,
        f"(payload={len(payload)}, buffer={SERVER_READ_BUFFER_BYTES})",
    )

    with connect(port) as sock:
        sock.sendall(payload.encode())
        response = recv_line(sock)

    print(f"    response: {response[:90]}")
    passed &= check(
        "multi-KB envelope is still parsed as one command",
        "Unknown command" not in response and "ERROR_BAD_REQUEST" not in response,
        f"(got {response[:80]!r})",
    )

    return passed


def case_split_sends(port):
    """A message split across two send() calls with a delay must still be one command."""
    print("Case 2: message split across two send() calls")

    command = register_command(node_id="framing-split-2", bunker_name="SPLIT", capacity=3)
    midpoint = len(command) // 2

    with connect(port) as sock:
        sock.sendall(command[:midpoint].encode())
        # Long enough that the server's poll loop certainly reads the first half on its own.
        time.sleep(0.3)
        sock.sendall(command[midpoint:].encode())
        response = recv_line(sock)

    print(f"    response: {response[:80]}")
    return check(
        "split REGISTER is reassembled into one command",
        response.startswith("ACK"),
        f"(got {response[:60]!r})",
    )


def case_oversized_analyze_graph(port):
    """The exact shape from issue #55, made worse: split mid-JSON so no '\\n' is in the first read."""
    print("Case 3: oversized ANALYZE_GRAPH split mid-JSON across two sends (the #55 shape)")

    payload = analyze_graph_envelope("framing-analyze-3", node_count=40)

    passed = check(
        "payload is larger than the server read buffer",
        len(payload) > SERVER_READ_BUFFER_BYTES,
        f"(payload={len(payload)}, buffer={SERVER_READ_BUFFER_BYTES})",
    )

    # Split mid-JSON across two sends: the first half ends nowhere near a '\n', so the server
    # must hold it rather than dispatch it.
    midpoint = len(payload) // 2
    with connect(port) as sock:
        sock.sendall(payload[:midpoint].encode())
        time.sleep(0.3)
        sock.sendall(payload[midpoint:].encode())
        response = recv_line(sock)

    print(f"    response: {response[:100]}")

    # No hpc-engine is running here, so the correct answer is the protocol's
    # "no engine available" error -- what matters is that the server parsed the whole envelope
    # as one ANALYZE_GRAPH instead of splitting it into unknown commands.
    passed &= check(
        "oversized envelope is recognized as ANALYZE_GRAPH, not split into garbage",
        "Unknown command" not in response and "ERROR_BAD_REQUEST" not in response,
        f"(got {response[:80]!r})",
    )
    # Reaching the dispatch path at all is the proof: before the fix this same payload produced
    # "ERROR_BAD_REQUEST desc=Unknown command" from a truncated prefix.
    passed &= check(
        "envelope reaches the ANALYZE_GRAPH dispatch path",
        "ERROR_SERVICE_UNAVAILABLE" in response or "RESPONSE" in response,
        f"(got {response[:80]!r})",
    )

    return passed


def case_larger_than_the_first_cap(port):
    """Un ANALYZE_GRAPH valido mas grande que el tope original de 8 MiB tiene que despacharse."""
    print("Case 5: valid ANALYZE_GRAPH larger than the original 8 MiB buffer cap")

    # ~55 bytes por nodo entre el nombre y su arista: 200.000 nodos pasan holgadamente los 8 MiB.
    payload = analyze_graph_envelope("framing-analyze-5", node_count=200_000)

    passed = check(
        "payload is larger than the original cap",
        len(payload) > FIRST_PENDING_INPUT_CAP_BYTES,
        f"(payload={len(payload)} bytes, original cap={FIRST_PENDING_INPUT_CAP_BYTES})",
    )

    with connect(port) as sock:
        sock.sendall(payload.encode())
        response = recv_line(sock, timeout=30)

    print(f"    response: {response[:100]}")

    passed &= check(
        "the message is not rejected for being long",
        "Message too long" not in response,
        f"(got {response[:80]!r})",
    )
    # Sin hpc-engine, llegar al despacho es la prueba de que el framing lo acepto entero.
    passed &= check(
        "envelope reaches the ANALYZE_GRAPH dispatch path",
        "ERROR_SERVICE_UNAVAILABLE" in response or "RESPONSE" in response,
        f"(got {response[:80]!r})",
    )

    return passed


def case_pipelined_commands(port):
    """Several commands in one send() must all be dispatched, none dropped or merged."""
    print("Case 4: multiple commands pipelined in one send()")

    commands = register_command(node_id="framing-pipe-a", bunker_name="A", capacity=1) + register_command(
        node_id="framing-pipe-b", bunker_name="B", capacity=1
    )

    with connect(port) as sock:
        sock.sendall(commands.encode())
        first = recv_line(sock)
        second = recv_line(sock)

    print(f"    responses: {first[:40]} | {second[:40]}")
    return check(
        "both pipelined REGISTERs are answered",
        first.startswith("ACK") and second.startswith("ACK"),
        f"(got {first[:40]!r} and {second[:40]!r})",
    )


def main():
    if len(sys.argv) < 2:
        print("usage: message_framing_split_recv.py <path-to-eop_server>")
        return 2

    server_bin = sys.argv[1]
    port = free_port()

    env = os.environ.copy()
    env["EOP_PORT"] = str(port)
    env["EOP_IDLE_TIMEOUT"] = "60"

    server = subprocess.Popen(
        [server_bin],
        env=env,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        text=True,
    )

    try:
        if not wait_for_port(port):
            print("ERROR: server did not start listening")
            return 1

        passed = True
        passed &= case_many_times_the_buffer(port)
        passed &= case_split_sends(port)
        passed &= case_oversized_analyze_graph(port)
        passed &= case_pipelined_commands(port)
        passed &= case_larger_than_the_first_cap(port)

        if not passed:
            print("FAILED: framing regression detected")
            return 1

        print("OK: messages split across recv() calls are reassembled correctly")
        return 0
    finally:
        server.terminate()
        try:
            server.wait(timeout=5)
        except subprocess.TimeoutExpired:
            server.kill()


if __name__ == "__main__":
    sys.exit(main())
