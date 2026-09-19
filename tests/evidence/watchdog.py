#!/usr/bin/env python3
"""
Guardián para pruebas de carga largas en una máquina chica.

Corre el comando en su propio grupo de procesos y, cada INTERVAL segundos, registra memoria
disponible, swap, temperatura, frecuencia de CPU y carga. Si la máquina se acerca a quedarse sin
memoria o se recalienta, termina el grupo entero (SIGTERM y, si no alcanza, SIGKILL) antes de que
el sistema se cuelgue: ya pasó dos veces en este proyecto (minikube + Grafana, y shortest_paths a
1.000 nodos). El CSV queda además como evidencia del estado de la máquina durante la medición.

Uso: watchdog.py --log muestras.csv [--min-avail-mb N] [--max-swap-growth-mb N] [--max-temp C] -- cmd...
Sale con el código del comando, o 3 si el guardián tuvo que cortarlo.
"""
import argparse
import glob
import os
import signal
import subprocess
import sys
import time

INTERVAL_S = 5
KILL_GRACE_S = 10


def meminfo():
    info = {}
    with open("/proc/meminfo") as f:
        for line in f:
            key, value = line.split(":", 1)
            info[key] = int(value.split()[0]) // 1024  # MB
    return info


def max_temp_c():
    temps = []
    for path in glob.glob("/sys/class/thermal/thermal_zone*/temp"):
        try:
            with open(path) as f:
                temps.append(int(f.read().strip()) / 1000)
        except (OSError, ValueError):
            pass
    return max(temps) if temps else float("nan")


def ac_online():
    for path in glob.glob("/sys/class/power_supply/A*/online"):
        try:
            with open(path) as f:
                return f.read().strip()
        except OSError:
            pass
    return "?"


def avg_mhz():
    mhz = []
    with open("/proc/cpuinfo") as f:
        for line in f:
            if line.startswith("cpu MHz"):
                mhz.append(float(line.split(":")[1]))
    return sum(mhz) / len(mhz) if mhz else float("nan")


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--log", required=True)
    parser.add_argument("--min-avail-mb", type=int, default=700)
    parser.add_argument("--max-swap-growth-mb", type=int, default=1500)
    parser.add_argument("--max-temp", type=float, default=95.0)
    parser.add_argument("cmd", nargs=argparse.REMAINDER)
    args = parser.parse_args()
    cmd = args.cmd[1:] if args.cmd and args.cmd[0] == "--" else args.cmd
    if not cmd:
        parser.error("falta el comando")

    base = meminfo()
    swap_base = base["SwapTotal"] - base["SwapFree"]
    proc = subprocess.Popen(cmd, start_new_session=True)
    reason = None
    with open(args.log, "w") as log:
        log.write("t_s,mem_avail_mb,swap_used_mb,temp_max_c,cpu_mhz_avg,load1,ac_online\n")
        start = time.time()
        while proc.poll() is None:
            m = meminfo()
            avail, swap = m["MemAvailable"], m["SwapTotal"] - m["SwapFree"]
            temp = max_temp_c()
            load1 = os.getloadavg()[0]
            log.write(f"{time.time() - start:.0f},{avail},{swap},{temp:.0f},{avg_mhz():.0f},{load1:.2f},{ac_online()}\n")
            log.flush()
            if avail < args.min_avail_mb:
                reason = f"memoria disponible {avail} MB < {args.min_avail_mb} MB"
            elif swap - swap_base > args.max_swap_growth_mb:
                reason = f"swap creció {swap - swap_base} MB > {args.max_swap_growth_mb} MB"
            elif temp >= args.max_temp:
                reason = f"temperatura {temp:.0f} °C >= {args.max_temp:.0f} °C"
            if reason:
                break
            time.sleep(INTERVAL_S)

    if reason:
        print(f"\n[watchdog] CORTE: {reason}. Terminando la prueba.", file=sys.stderr, flush=True)
        os.killpg(proc.pid, signal.SIGTERM)
        try:
            proc.wait(timeout=KILL_GRACE_S)
        except subprocess.TimeoutExpired:
            os.killpg(proc.pid, signal.SIGKILL)
            proc.wait()
        with open(args.log, "a") as log:
            log.write(f"# CORTE: {reason}\n")
        return 3
    return proc.returncode


if __name__ == "__main__":
    sys.exit(main())
