#!/usr/bin/env python3
import os
import signal
import socket
import subprocess
import sys
import time


HOST = "127.0.0.1"
PORT = 9093
IDLE_TIMEOUT = 60
HEARTBEAT_INTERVAL = 5


def count_fds(pid: int) -> int:
    return len(os.listdir(f"/proc/{pid}/fd"))


def wait_for_server(host: str, port: int, timeout_s: float = 5.0) -> None:
    deadline = time.time() + timeout_s
    while time.time() < deadline:
        try:
            with socket.create_connection((host, port), timeout=1):
                return
        except OSError:
            time.sleep(0.05)
    raise RuntimeError("Server did not start in time")


def run_cycle_connect_disconnect(host: str, port: int) -> None:
    sock = socket.create_connection((host, port), timeout=2)
    try:
        # Read welcome if present
        try:
            sock.recv(1024)
        except OSError:
            pass
    finally:
        sock.close()


def run_cycle_register_disconnect(host: str, port: int, idx: int) -> None:
    sock = socket.create_connection((host, port), timeout=2)
    try:
        try:
            sock.recv(1024)
        except OSError:
            pass

        msg = (
            f"REGISTER node_id=fdtest-{idx} "
            f"bunker_name=FDTest ip_address=10.0.0.{(idx % 250) + 1} "
            f"capacity=1"
        )
        sock.sendall(msg.encode())
        sock.recv(1024)
    finally:
        sock.close()


def main() -> int:
    cycles = int(sys.argv[1]) if len(sys.argv) > 1 else 10000
    mode = sys.argv[2] if len(sys.argv) > 2 else "register"

    env = os.environ.copy()
    env["EOP_PORT"] = str(PORT)
    env["EOP_IDLE_TIMEOUT"] = str(IDLE_TIMEOUT)
    env["EOP_HEARTBEAT_INTERVAL"] = str(HEARTBEAT_INTERVAL)

    server = subprocess.Popen(
        ["./build/eop_server"],
        stdout=subprocess.DEVNULL,
        stderr=subprocess.DEVNULL,
        env=env,
    )

    try:
        wait_for_server(HOST, PORT)
        pid = server.pid

        baseline_fds = count_fds(pid)
        print(f"Server PID: {pid}")
        print(f"Initial FD count: {baseline_fds}")

        for i in range(cycles):
            if mode == "connect":
                run_cycle_connect_disconnect(HOST, PORT)
            else:
                run_cycle_register_disconnect(HOST, PORT, i)

            if (i + 1) % 10000 == 0:
                print(f"Completed {i + 1} cycles")

        time.sleep(1.0)
        final_fds = count_fds(pid)
        print(f"Final FD count: {final_fds}")

        growth = final_fds - baseline_fds
        print(f"FD delta: {growth}")

        if growth > 2:
            print("FAIL: Possible file descriptor leak detected")
            return 1

        print("PASS: No significant file descriptor leak detected")
        return 0

    finally:
        try:
            server.send_signal(signal.SIGINT)
            server.wait(timeout=3)
        except Exception:
            server.kill()


if __name__ == "__main__":
    raise SystemExit(main())