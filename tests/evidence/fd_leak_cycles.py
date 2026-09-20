#!/usr/bin/env python3
"""
US-102 AC4: "The server has no file descriptor leaks: verified by comparing /proc/PID/fd count
before and after 1M connect/disconnect cycles."

Lanza un eop_server real, cuenta sus descriptores abiertos, hace N ciclos de conectar y
desconectar, y vuelve a contar. Dos modos de desconexión, porque cada uno recorre otro camino de
cierre en el server:

- abrupto: SO_LINGER=0, el cliente cierra con RST (proceso que muere, cable que se corta).
  No deja TIME_WAIT del lado del cliente, que es lo que permite llegar al millón: con cierre
  normal, los ~28K puertos efímeros se agotan a los pocos segundos.
- ordenado: close() normal con FIN. Se corre en menos ciclos por el mismo motivo.

El veredicto exige que TODOS los ciclos intentados se hayan completado: un connect que falla, o un
banner que no llega, no recorrió el camino que el AC mide y no cuenta como evidencia.

Uso: fd_leak_cycles.py <eop_server> [--abrupt N] [--graceful N]
"""
import argparse
import os
import shutil
import socket
import struct
import subprocess
import sys
import tempfile
import time

PROGRESS_EVERY = 100_000
BANNER_TIMEOUT_S = 5
SETTLE_S = 3  # el server libera el fd cuando su hilo detecta el cierre, no en el acto


def free_port():
    with socket.socket() as s:
        s.bind(("127.0.0.1", 0))
        return s.getsockname()[1]


def fd_count(pid):
    return len(os.listdir(f"/proc/{pid}/fd"))


def rss_mb(pid):
    with open(f"/proc/{pid}/status") as f:
        for line in f:
            if line.startswith("VmRSS:"):
                return int(line.split()[1]) / 1024
    return float("nan")


def wait_for_port(port, timeout=10):
    deadline = time.time() + timeout
    while time.time() < deadline:
        try:
            with socket.create_connection(("127.0.0.1", port), timeout=0.5):
                return True
        except OSError:
            time.sleep(0.1)
    return False


def server_answers(port):
    """El server sigue sano si acepta una conexión nueva y manda su banner."""
    try:
        with socket.create_connection(("127.0.0.1", port), timeout=5) as s:
            s.settimeout(5)
            return s.recv(64).startswith(b"WELCOME")
    except OSError:
        return False


def cycles(port, count, abrupt, pid, label):
    linger = struct.pack("ii", 1, 0)
    failures = 0
    completed = 0
    start = time.time()
    for i in range(1, count + 1):
        s = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
        if abrupt:
            s.setsockopt(socket.SOL_SOCKET, socket.SO_LINGER, linger)
        try:
            s.settimeout(BANNER_TIMEOUT_S)
            s.connect(("127.0.0.1", port))
            # Esperar el banner hace que cada ciclo sea una conexión completa a nivel aplicación
            # y que el cliente no pueda adelantarse al accept() del server: sin esto, una ráfaga de
            # connect+RST llena la cola de listen, el kernel descarta SYNs y cada descarte cuesta 1 s
            # de reintento TCP (medido: 624 descartes ≈ 10 minutos perdidos en ~95.000 ciclos).
            if s.recv(64).startswith(b"WELCOME"):
                completed += 1
            else:
                failures += 1
        except OSError:
            failures += 1
        finally:
            s.close()
        if i % PROGRESS_EVERY == 0:
            rate = i / (time.time() - start)
            print(f"   {label}: {i:>9,} ciclos  ({rate:,.0f}/s)  fds del server={fd_count(pid)}  "
                  f"fallas de connect={failures}", flush=True)
    return completed, failures, time.time() - start


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("server")
    parser.add_argument("--abrupt", type=int, default=1_000_000)
    parser.add_argument("--graceful", type=int, default=20_000)
    args = parser.parse_args()

    port = free_port()
    workdir = tempfile.mkdtemp(prefix="eop-fd-leak-")  # el server escribe sus logs acá
    env = dict(os.environ, EOP_PORT=str(port), EOP_TRACING_ENABLED="0")
    server = subprocess.Popen([os.path.abspath(args.server)], cwd=workdir, env=env,
                              stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    try:
        if not wait_for_port(port):
            print("ERROR: el server no arrancó")
            return 1
        time.sleep(SETTLE_S)
        before, rss_before = fd_count(server.pid), rss_mb(server.pid)
        print(f"fds antes: {before}   RSS antes: {rss_before:.1f} MB")

        attempted = 0
        completed_total = 0
        failures = 0
        for label, count, abrupt in (("abrupto (RST)", args.abrupt, True), ("ordenado (FIN)", args.graceful, False)):
            if count <= 0:
                continue
            done, f, elapsed = cycles(port, count, abrupt, server.pid, label)
            attempted += count
            completed_total += done
            failures += f
            print(f"   {label}: {done:,} de {count:,} ciclos completos en {elapsed:.0f} s, {f} fallidos")

        time.sleep(SETTLE_S)
        after, rss_after = fd_count(server.pid), rss_mb(server.pid)
        healthy = server.poll() is None and server_answers(port)
        print(f"fds después: {after}   RSS después: {rss_after:.1f} MB")
        print(f"RESULTADO: {completed_total:,} de {attempted:,} ciclos completos ({failures} fallidos), "
              f"fds {before} -> {after} (diferencia {after - before}), server sano después: {healthy}")
        # Un ciclo fallido no recorrió el camino que el AC mide, así que no puede contarse como
        # evidencia: el veredicto exige que los completos sean todos los intentados.
        ok = completed_total == attempted and after == before and healthy
        print("US-102 AC4:", "CUMPLE" if ok else "NO CUMPLE")
        if completed_total != attempted:
            print(f"  ({attempted - completed_total:,} ciclos no completaron el connect/disconnect a nivel aplicación)")
        return 0 if ok else 1
    finally:
        server.terminate()
        try:
            server.wait(timeout=5)
        except subprocess.TimeoutExpired:
            server.kill()
        shutil.rmtree(workdir, ignore_errors=True)


if __name__ == "__main__":
    sys.exit(main())
