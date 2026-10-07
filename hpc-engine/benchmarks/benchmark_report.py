#!/usr/bin/env python3
"""
benchmark_report.py — mide el HPC Engine y genera BENCHMARK_REPORT.md (US-202).

Lo invoca run_benchmark.sh después de compilar el engine. Separa tres cosas que antes vivían
mezcladas en bash: correr la matriz de mediciones, analizarla, y escribir el reporte. El análisis
son funciones puras (sin I/O) para que tests/smoke/test_benchmark_report.py pueda verificarlo
con datos sintéticos, sin compilar ni correr el engine.

Dos reglas guían el análisis, porque la primera versión del reporte las violaba:

- Amdahl modela procesadores reales. Los hilos por encima de los núcleos físicos son
  hyperthreading: comparten caché y ancho de banda con otro hilo del mismo núcleo, y su speedup
  no dice nada sobre la fracción serial del algoritmo. Por eso quedan fuera del ajuste.
- Ninguna conclusión se escribe si no sale de los datos medidos.
"""
import argparse
import glob
import json
import os
import platform
import random
import resource
import statistics
import subprocess
import sys
import time
from dataclasses import dataclass, field
from typing import Dict, List, Optional, Sequence, Tuple

import generate_graph

# --- Criterios de US-202 (PRD) ------------------------------------------------------------------
AC2_THREADS = 4
AC2_MIN_NODES = 50_000
AC2_MIN_SPEEDUP = 2.0
AC3_THREADS = 8
AC3_MIN_NODES = 100_000
AC3_MIN_SPEEDUP = 3.0
AC5_SIZES = (10_000, 50_000, 100_000)
AC5_THREADS = (1, 2, 4, 8)

# Speedup mínimo para contar como "ganancia" en el AC6. Por debajo de ~10%, la diferencia entre
# 1 hilo y N hilos no se distingue de la variación entre corridas de una misma configuración
# (medida entre 1% y 16% en esta clase de máquina, según tamaño).
MIN_POSITIVE_SPEEDUP = 1.10

# Por debajo de esto, arrancar y sincronizar los hilos pesa demasiado frente al trabajo como para
# atribuirle un empeoramiento a la memoria (ver memory_bound).
MEMORY_CEILING_MIN_SERIAL_S = 1.0

# AC8: el proceso no puede usar más CPU que los hilos configurados. El margen cubre el costo
# de medir (el propio arranque del proceso y el runtime de OpenMP), no una tolerancia de diseño.
AC8_CPU_TOLERANCE = 0.10

# Grafos grandes con menos aristas extra: es la densidad que ya usaba el benchmark para que el
# 100K entre en la memoria de una laptop. Se documenta por tamaño en el reporte porque cambia
# la forma del grafo, y con ella los tiempos.
DENSE_GRAPH_MAX_NODES = 50_000
DENSITY_SMALL = 2.0
DENSITY_LARGE = 0.5

# Los tamaños chicos están para que el AC6 tenga dónde encontrar el umbral: por debajo de ~100
# nodos crear el equipo de hilos cuesta más que el trabajo (10 nodos: 0.29× con 2 hilos), y desde
# ~200 la ganancia ya es clara. Cuestan milisegundos. 10K/50K/100K son los que exige el AC5.
DEFAULT_SIZES = (10, 50, 100, 200, 500, 1_000, 5_000, 10_000, 50_000, 100_000)

# El mismo pedido en todos los tamaños, o las filas no son comparables entre sí. generate_graph
# agrega shortest_paths por debajo de 500 nodos, y ese algoritmo además tiene un límite propio
# (EOP_MAX_SHORTEST_PATHS_NODES, 300 por defecto) porque su salida es O(N³).
BENCHMARK_ALGORITHMS = ["connected_components", "centrality"]
DEFAULT_THREADS = (1, 2, 4, 8)
DEFAULT_REPEATS = 3

# Una medición que ya tarda más que esto no se repite: a 100K nodos con centralidad exacta una sola
# corrida lleva decenas de minutos, y 3 repeticiones de toda la matriz serían más de un día en una
# laptop. Las repeticiones importan donde el ruido es comparable a la medida (milisegundos), no ahí.
DEFAULT_MAX_REPEAT_SECONDS = 60.0

# Enfriamiento opcional antes de cada corrida. En una laptop, una corrida que arranca caliente mide
# el throttling térmico, no el algoritmo: con la muestreada a 100K nodos, 4 hilos dio *peor* que 2
# (1,35× contra 1,63×) con el procesador a 94 °C y bajando la frecuencia.
COOLDOWN_MAX_WAIT_S = 300
COOLDOWN_POLL_S = 2
DEFAULT_SEED = 20260918


# --- Modelo de datos ----------------------------------------------------------------------------
@dataclass
class Run:
    processing_s: float
    wall_s: float
    cpu_s: float


@dataclass
class Cell:
    """Todas las corridas de un (tamaño, hilos)."""

    size: int
    threads: int
    runs: List[Run]

    @property
    def median_s(self) -> float:
        return statistics.median(r.processing_s for r in self.runs)

    @property
    def min_s(self) -> float:
        return min(r.processing_s for r in self.runs)

    @property
    def max_s(self) -> float:
        return max(r.processing_s for r in self.runs)

    @property
    def avg_cores(self) -> float:
        """CPU consumida / tiempo de pared: cuántos núcleos usó el proceso en promedio (AC8)."""
        return statistics.median(r.cpu_s / r.wall_s for r in self.runs if r.wall_s > 0)


@dataclass
class SizeResult:
    size: int
    edges: int
    density: float
    centrality_mode: str
    cells: Dict[int, Cell] = field(default_factory=dict)
    # Hilos cuya medición falló, con el motivo. Se informan en su fila en vez de abortar: una
    # corrida completa tarda horas y un error en 100K no debería tirar lo ya medido.
    errors: Dict[int, str] = field(default_factory=dict)

    def speedup(self, threads: int) -> Optional[float]:
        base = self.cells.get(1)
        cell = self.cells.get(threads)
        if base is None or cell is None or cell.median_s <= 0:
            return None
        return base.median_s / cell.median_s


# --- Análisis (funciones puras) -----------------------------------------------------------------
def serial_fraction(speedups: Dict[int, float]) -> Optional[float]:
    """
    Ajuste de mínimos cuadrados de la ley de Amdahl.

    Amdahl: 1/S(p) = s + (1 - s)/p  ⇔  1/S(p) - 1/p = s · (1 - 1/p).
    Con x = 1 - 1/p e y = 1/S(p) - 1/p es una recta por el origen de pendiente s, así que
    s = Σxy / Σx². Usar varios p en vez de uno solo hace que un punto ruidoso no domine.

    Recibe solo p > 1 y dentro de los núcleos físicos; el llamador decide cuáles entran.
    """
    points = [(1 - 1 / p, 1 / sp - 1 / p) for p, sp in speedups.items() if p > 1 and sp > 0]
    if not points:
        return None
    sxx = sum(x * x for x, _ in points)
    sxy = sum(x * y for x, y in points)
    return min(1.0, max(0.0, sxy / sxx))


def amdahl_speedup(s: float, p: int) -> float:
    return 1.0 / (s + (1.0 - s) / p)


def physical_thread_counts(threads: Sequence[int], physical_cores: int) -> List[int]:
    return [t for t in threads if 1 < t <= physical_cores]


def slows_down_beyond_physical(result: SizeResult, physical_cores: int) -> bool:
    """
    True si agregar hilos por encima de los núcleos físicos empeora el tiempo *sin ambigüedad*:
    la corrida más rápida con hyperthreading es más lenta que la más lenta de la mejor
    configuración con núcleos reales. Comparar medianas no alcanza: en grafos de milisegundos
    dos medianas a 3% de distancia con rangos superpuestos son ruido, no un empeoramiento.
    """
    inside = [result.cells[t] for t in result.cells if t <= physical_cores]
    outside = [result.cells[t] for t in result.cells if t > physical_cores]
    if not inside or not outside:
        return False
    best_inside = min(inside, key=lambda c: c.median_s)
    return any(c.min_s > best_inside.max_s for c in outside)


def gains_in_parallel(result: SizeResult, physical_cores: int) -> bool:
    """True si con algún número de hilos dentro de los núcleos físicos se llega a MIN_POSITIVE_SPEEDUP."""
    speedups = [result.speedup(t) for t in physical_thread_counts(result.cells, physical_cores)]
    return any(sp is not None and sp >= MIN_POSITIVE_SPEEDUP for sp in speedups)


def memory_bound(result: SizeResult, physical_cores: int) -> bool:
    """
    Techo de memoria: el tamaño gana con núcleos reales, pero los hyperthreads lo empeoran.

    Hacen falta las tres condiciones. En un grafo que no gana con núcleos reales, o cuyo trabajo
    dura milisegundos, que 8 hilos sean más lentos es el costo de coordinar el equipo de hilos —
    lo que el AC6 ya informa —, no un límite de memoria: un grafo de 100 nodos entra entero en la
    caché. Medido: 100 nodos gana 1,1× con 4 hilos y pierde con 8, en una corrida de 2 ms.
    """
    base = result.cells.get(1)
    long_enough = base is not None and base.median_s >= MEMORY_CEILING_MIN_SERIAL_S
    return long_enough and gains_in_parallel(result, physical_cores) and slows_down_beyond_physical(
        result, physical_cores)


def min_parallel_size(results: Sequence[SizeResult], physical_cores: int) -> Tuple[Optional[int], List[int]]:
    """
    AC6: el menor tamaño medido a partir del cual paralelizar da ganancia real.

    Devuelve (umbral, tamaños_sin_ganancia). Un tamaño "gana" si con algún número de hilos
    dentro de los núcleos físicos el speedup llega a MIN_POSITIVE_SPEEDUP. El umbral es el menor
    tamaño que gana y a partir del cual todos los mayores también ganan: un tamaño chico que
    gane por ruido, con uno mayor que no, no cuenta como umbral.
    """
    ordered = sorted(results, key=lambda r: r.size)
    wins = {r.size: gains_in_parallel(r, physical_cores) for r in ordered}
    threshold = None
    for r in reversed(ordered):
        if not wins[r.size]:
            break
        threshold = r.size
    no_gain = [r.size for r in ordered if not wins[r.size]]
    return threshold, no_gain


@dataclass
class Verdict:
    label: str
    description: str
    measured: str
    required: str
    status: str  # "pass" | "fail" | "unmeasurable" | "external"
    note: str = ""


def speedup_verdict(label, results, threads, min_nodes, min_speedup, physical_cores) -> Verdict:
    description = f"{threads} hilos, grafos de ≥ {fmt_n(min_nodes)} nodos"
    required = f"≥ {min_speedup:.1f}×"
    if physical_cores < threads:
        return Verdict(label, description, "—", required, "unmeasurable",
                       f"la máquina tiene {physical_cores} núcleos físicos: con {threads} hilos se "
                       f"mide hyperthreading, no {threads} núcleos")
    relevant = [(r.size, r.speedup(threads)) for r in results if r.size >= min_nodes]
    relevant = [(n, sp) for n, sp in relevant if sp is not None]
    if not relevant:
        return Verdict(label, description, "—", required, "unmeasurable",
                       f"no se midió ningún grafo de ≥ {fmt_n(min_nodes)} nodos con {threads} hilos")
    worst_size, worst = min(relevant, key=lambda x: x[1])
    measured = f"{worst:.2f}× ({fmt_n(worst_size)} nodos)"
    return Verdict(label, description, measured, required, "pass" if worst >= min_speedup else "fail")


def ac5_verdict(results: Sequence[SizeResult]) -> Verdict:
    measured = {(r.size, t) for r in results for t in r.cells}
    missing = [(n, t) for n in AC5_SIZES for t in AC5_THREADS if (n, t) not in measured]
    required = "1/2/4/8 hilos × 10K/50K/100K + Amdahl"
    if missing:
        faltan = ", ".join(f"{n // 1000}K/{t}h" for n, t in missing)
        return Verdict("AC5", "Matriz de mediciones y análisis de Amdahl", f"faltan {len(missing)}",
                       required, "fail", f"sin medir: {faltan}")
    total = len(AC5_SIZES) * len(AC5_THREADS)
    return Verdict("AC5", "Matriz de mediciones y análisis de Amdahl", f"{total}/{total}", required, "pass")


def ac8_verdict(results: Sequence[SizeResult]) -> Verdict:
    worst = None
    for r in results:
        for t, cell in r.cells.items():
            excess = cell.avg_cores - t
            if worst is None or excess > worst[0]:
                worst = (excess, r.size, t, cell.avg_cores)
    if worst is None:
        return Verdict("AC8", "CPU usada ≤ OMP_NUM_THREADS", "—", "≤ hilos", "unmeasurable")
    excess, size, t, cores = worst
    measured = f"{cores:.2f} núcleos con {t} {'hilo' if t == 1 else 'hilos'} ({fmt_n(size)} nodos)"
    status = "pass" if excess <= AC8_CPU_TOLERANCE else "fail"
    return Verdict("AC8", "CPU usada ≤ OMP_NUM_THREADS", measured, "≤ hilos", status,
                   "promedio del proceso completo (CPU consumida / tiempo de pared), en el peor caso medido")


# --- Medición -----------------------------------------------------------------------------------
def density_for(size: int) -> float:
    return DENSITY_LARGE if size >= DENSE_GRAPH_MAX_NODES else DENSITY_SMALL


def build_graph(size: int, seed: int) -> Tuple[bytes, int]:
    random.seed(seed)
    payload = generate_graph.generate_graph(size, density_for(size))
    payload["payload"]["algorithms"] = list(BENCHMARK_ALGORITHMS)
    return json.dumps(payload).encode(), len(payload["payload"]["edges"])


def max_temp_c() -> Optional[float]:
    temps = []
    for path in glob.glob("/sys/class/thermal/thermal_zone*/temp"):
        try:
            with open(path) as f:
                temps.append(int(f.read().strip()) / 1000)
        except (OSError, ValueError):
            pass
    return max(temps) if temps else None


def cool_down(below_c: Optional[float], log=print, sleep=time.sleep, read_temp=max_temp_c) -> None:
    """Espera a que la temperatura baje de below_c, con un máximo de COOLDOWN_MAX_WAIT_S."""
    if below_c is None:
        return
    waited = 0
    while waited < COOLDOWN_MAX_WAIT_S:
        temp = read_temp()
        if temp is None or temp < below_c:
            return
        sleep(COOLDOWN_POLL_S)
        waited += COOLDOWN_POLL_S
    log(f"   ⚠️  la temperatura no bajó de {below_c:.0f} °C en {COOLDOWN_MAX_WAIT_S} s; se mide igual")


def run_engine(engine: str, graph: bytes, threads: int) -> Tuple[dict, Run]:
    env = dict(os.environ, OMP_NUM_THREADS=str(threads))
    before = resource.getrusage(resource.RUSAGE_CHILDREN)
    start = time.perf_counter()
    proc = subprocess.run([engine], input=graph, capture_output=True, env=env)
    wall = time.perf_counter() - start
    after = resource.getrusage(resource.RUSAGE_CHILDREN)
    try:
        response = json.loads(proc.stdout)
    except ValueError:
        response = None
    if response is None or response.get("status") != "ACK":
        detail = (f"{response.get('error_code')}: {response.get('message')}" if response
                  else proc.stdout.decode(errors="replace").strip()[:200] or "sin respuesta")
        raise RuntimeError(f"código de salida {proc.returncode} — {detail}")
    cpu = (after.ru_utime - before.ru_utime) + (after.ru_stime - before.ru_stime)
    result = response["result"]
    return result, Run(result["processing_time_ms"] / 1000.0, wall, cpu)


def centrality_mode(result: dict) -> str:
    meta = result.get("algorithm_metadata", {}).get("centrality")
    if meta and meta.get("approximate"):
        return f"muestreada ({meta.get('sampled_sources')} de {meta.get('total_sources')} fuentes, ADR-024)"
    return "exacta"


def measure_size(engine: str, size: int, threads, repeats: int, seed: int, log=print,
                 max_repeat_seconds: float = DEFAULT_MAX_REPEAT_SECONDS,
                 cooldown_below: Optional[float] = None) -> SizeResult:
    graph, edges = build_graph(size, seed)
    sr = SizeResult(size, edges, density_for(size), centrality_mode="sin datos")
    for t in threads:
        runs = []
        try:
            for i in range(repeats):
                cool_down(cooldown_below, log)
                result, run = run_engine(engine, graph, t)
                runs.append(run)
                sr.centrality_mode = centrality_mode(result)
                log(f"   {size:>7} nodos  {t} hilos  corrida {i + 1}/{repeats}: {run.processing_s:9.3f} s")
                if run.wall_s > max_repeat_seconds:
                    break
        except (RuntimeError, OSError, ValueError, KeyError) as exc:
            sr.errors[t] = str(exc)
            log(f"   ⚠️  {size} nodos con {t} hilos falló: {exc}")
            continue
        sr.cells[t] = Cell(size, t, runs)
    return sr


# --- Reporte ------------------------------------------------------------------------------------
STATUS_ICON = {"pass": "✅", "fail": "❌", "unmeasurable": "⚠️ no medible", "external": "⚙️"}


def fmt_n(n: int) -> str:
    return f"{n:,}".replace(",", ".")


def fmt_s(seconds: float) -> str:
    if seconds < 1:
        return f"{seconds * 1000:.2f} ms"
    return f"{seconds:.3f} s" if seconds < 10 else f"{seconds:.1f} s"


def render_report(results: Sequence[SizeResult], env: dict) -> Tuple[str, List[Verdict]]:
    physical = env["physical_cores"]
    lines = [
        "# HPC Engine — Benchmark Report",
        "",
        "> Generado automáticamente por `run_benchmark.sh` (`make benchmark`).",
        f"> Fecha: {env['date']}",
        "",
        "## Entorno de ejecución",
        "",
        "| Parámetro | Valor |",
        "|---|---|",
        f"| Commit | `{env['commit']}` |",
        f"| CPU | {env['cpu_model']} |",
        f"| Núcleos físicos | **{physical}** |",
        f"| Hilos lógicos | {env['logical_cpus']} |",
        f"| Energía | {env.get('power', 'no registrado')} |",
        f"| Compilador | {env['compiler']} |",
        f"| Build | {env['build']} |",
        f"| Repeticiones por medición | hasta {env['repeats']} (mediana); una sola si la corrida pasa de "
        f"{env.get('max_repeat_seconds', DEFAULT_MAX_REPEAT_SECONDS):.0f} s — ver columna n |",
        f"| Semilla del grafo | {env['seed']} |",
        f"| Enfriamiento | {env.get('cooldown', 'no registrado')} |",
        f"| Algoritmos | {', '.join(BENCHMARK_ALGORITHMS)} (el mismo pedido en todos los tamaños) |",
        "",
    ]
    ht = sorted({t for r in results for t in r.cells if t > physical})
    if ht:
        lines += [
            f"> ⚠️ **Esta máquina tiene {physical} núcleos físicos.** Las mediciones con "
            f"{', '.join(map(str, ht))} hilos usan hyperthreading: dos hilos comparten un núcleo, su "
            "caché y su ancho de banda. Se informan, pero no miden paralelismo real y quedan fuera "
            "del análisis de Amdahl.",
            "",
        ]

    lines += ["## Mediciones", "",
              "El tiempo es `processing_time_ms` del engine: solo los algoritmos, sin parseo ni "
              "serialización. La columna CPU es el promedio de núcleos que usó el proceso completo "
              "(AC8).", ""]
    for r in results:
        lines += [f"### {fmt_n(r.size)} nodos — {fmt_n(r.edges)} aristas (densidad {r.density})", "",
                  f"Centralidad {r.centrality_mode}.", "",
                  "| Hilos | n | Tiempo (mediana) | Rango | Speedup | Eficiencia | CPU (núcleos) |",
                  "|---:|---:|---:|---:|---:|---:|---:|"]
        for t in sorted(set(r.cells) | set(r.errors)):
            if t in r.errors:
                lines.append(f"| {t} | — | **ERROR** | {r.errors[t][:120]} | — | — | — |")
                continue
            c = r.cells[t]
            sp = r.speedup(t)
            tag = " (HT)" if t > physical else ""
            eff = f"{100 * sp / t:.0f}%" if sp else "—"
            n = len(c.runs)
            lines.append(f"| {t}{tag} | {n} | {fmt_s(c.median_s)} | {fmt_s(c.min_s)} – {fmt_s(c.max_s)} | "
                         f"{sp:.2f}× | {eff} | {c.avg_cores:.2f} |" if sp else
                         f"| {t}{tag} | {n} | {fmt_s(c.median_s)} | — | — | — | {c.avg_cores:.2f} |")
        lines.append("")

    # Amdahl
    lines += ["## Análisis — Ley de Amdahl", "",
              "`S(p) = 1 / (s + (1 − s)/p)`, con `s` la fracción serial. `s` se ajusta por "
              f"mínimos cuadrados con las mediciones de 2 a {physical} hilos (los núcleos físicos). "
              "`S(8)` es la predicción del modelo para 8 núcleos **físicos**, no una medición.", "",
              "| Nodos | Hilos usados en el ajuste | s | Speedup máximo (p→∞) | S(8) predicho |",
              "|---:|---|---:|---:|---:|"]
    memory_bound_sizes = []
    no_gain_sizes = []
    for r in results:
        used = physical_thread_counts(r.cells, physical)
        if not gains_in_parallel(r, physical):
            # Donde paralelizar no gana, el ajuste daría s ≈ 1 y parecería "todo serial"; en
            # realidad domina el costo de arrancar los hilos, que Amdahl no modela.
            lines.append(f"| {fmt_n(r.size)} | {', '.join(map(str, used)) or '—'} | n/a | — | — |")
            no_gain_sizes.append(r.size)
            continue
        s = serial_fraction({t: r.speedup(t) for t in used if r.speedup(t)})
        if s is None:
            lines.append(f"| {fmt_n(r.size)} | — | — | — | — |")
            continue
        smax = "∞" if s == 0 else f"{1 / s:.2f}×"
        lines.append(f"| {fmt_n(r.size)} | {', '.join(map(str, used))} | {s:.3f} | {smax} | "
                     f"{amdahl_speedup(s, 8):.2f}× |")
        if memory_bound(r, physical):
            memory_bound_sizes.append(r.size)
    lines.append("")
    if no_gain_sizes:
        lines += [f"> {', '.join(fmt_n(n) for n in no_gain_sizes)} nodos: **n/a** porque ninguna cantidad "
                  "de hilos gana. El costo de crear el equipo de hilos supera al del trabajo; es el "
                  "resultado del AC6, no una fracción serial.", ""]
    if memory_bound_sizes:
        lines += [f"> En {', '.join(fmt_n(n) for n in memory_bound_sizes)} nodos, agregar hilos por encima "
                  "de los núcleos físicos **empeora** el tiempo. Amdahl no puede explicar eso: una "
                  "fracción serial frena la ganancia, pero no la invierte. Es la firma de un límite "
                  "de memoria compartida (caché y ancho de banda), no de código serial.", ""]

    # Criterios
    verdicts = [
        speedup_verdict("AC2", results, AC2_THREADS, AC2_MIN_NODES, AC2_MIN_SPEEDUP, physical),
        speedup_verdict("AC3", results, AC3_THREADS, AC3_MIN_NODES, AC3_MIN_SPEEDUP, physical),
        Verdict("AC4", "Cero data races (ThreadSanitizer)", "—", "0 races", "external",
                "no se mide acá: `make tsan` y el job `thread-sanitizer` de CI"),
        ac5_verdict(results),
    ]
    threshold, no_gain = min_parallel_size(results, physical)
    if threshold is None:
        ac6 = Verdict("AC6", "Tamaño mínimo con ganancia al paralelizar", "ninguno",
                      f"speedup ≥ {MIN_POSITIVE_SPEEDUP:.2f}×", "fail",
                      "ningún tamaño medido gana de forma sostenida")
    else:
        ac6 = Verdict("AC6", "Tamaño mínimo con ganancia al paralelizar", f"{fmt_n(threshold)} nodos",
                      f"speedup ≥ {MIN_POSITIVE_SPEEDUP:.2f}×", "pass")
    verdicts += [ac6, ac8_verdict(results)]

    lines += ["## Criterios de aceptación (US-202)", "",
              "| AC | Qué pide | Medido | Requerido | Estado |", "|---|---|---|---|:---:|"]
    for v in verdicts:
        lines.append(f"| {v.label} | {v.description} | {v.measured} | {v.required} | {STATUS_ICON[v.status]} |")
    lines.append("")
    for v in verdicts:
        if v.note:
            lines.append(f"- **{v.label}**: {v.note}.")
    lines.append("")

    # Conclusión — solo afirmaciones que salen de las tablas de arriba.
    lines += ["## Conclusión", ""]
    smallest = min(r.size for r in results)
    if threshold is None:
        lines.append("- En ningún tamaño medido la paralelización da una ganancia sostenida.")
    elif threshold == smallest:
        lines.append(f"- **Cuándo no paralelizar (AC6):** todos los tamaños medidos ganan con más de un "
                     f"hilo. El umbral está en {fmt_n(threshold)} nodos o por debajo; para ubicarlo hay "
                     "que medir tamaños menores.")
    else:
        lines.append(f"- **Cuándo no paralelizar (AC6):** por debajo de {fmt_n(threshold)} nodos la ganancia "
                     f"no supera {MIN_POSITIVE_SPEEDUP:.2f}× (sin ganancia: "
                     f"{', '.join(fmt_n(n) for n in no_gain)} nodos); conviene correr con 1 hilo.")
    failed = [v.label for v in verdicts if v.status == "fail"]
    unmeasurable = [v.label for v in verdicts if v.status == "unmeasurable"]
    if failed:
        lines.append(f"- **No se cumplen:** {', '.join(failed)}.")
    if unmeasurable:
        lines.append(f"- **No se pudieron medir en esta máquina:** {', '.join(unmeasurable)} — ver notas arriba.")
    if memory_bound_sizes:
        lines.append(f"- En {', '.join(fmt_n(n) for n in memory_bound_sizes)} nodos el techo es la memoria, "
                     "no la fracción serial: más hilos sobre los mismos núcleos no agregan ancho de banda.")
    if not failed and not unmeasurable:
        lines.append("- Todos los criterios medibles se cumplen.")
    lines.append("")
    return "\n".join(lines), verdicts


def results_to_json(r: SizeResult) -> dict:
    return {"size": r.size, "edges": r.edges, "density": r.density, "centrality_mode": r.centrality_mode,
            "errors": {str(t): e for t, e in r.errors.items()},
            "cells": {str(t): [[run.processing_s, run.wall_s, run.cpu_s] for run in c.runs] for t, c in r.cells.items()}}


def results_from_json(data: dict) -> List[SizeResult]:
    out = []
    for d in data["results"]:
        r = SizeResult(d["size"], d["edges"], d["density"], d["centrality_mode"],
                       errors={int(t): e for t, e in d.get("errors", {}).items()})
        for t, runs in d["cells"].items():
            r.cells[int(t)] = Cell(d["size"], int(t), [Run(*run) for run in runs])
        out.append(r)
    return out


def detect_environment(args) -> dict:
    def sh(cmd):
        try:
            return subprocess.run(cmd, capture_output=True, text=True, check=True).stdout.strip()
        except (OSError, subprocess.CalledProcessError):
            return ""

    cores = sh(["lscpu", "-p=Core,Socket"])
    physical = len({line for line in cores.splitlines() if line and not line.startswith("#")})
    model = ""
    for line in sh(["lscpu"]).splitlines():
        if "model name" in line.lower() or "nombre del modelo" in line.lower():
            model = line.split(":", 1)[1].strip()
    # El estado de energía cambia los tiempos más que cualquier optimización: el mismo binario sobre
    # el mismo grafo midió 3,2 veces más lento en una sesión que en otra de la misma laptop. Sin
    # registrarlo, dos reportes no se pueden comparar.
    def read(path):
        try:
            with open(path) as f:
                return f.read().strip()
        except OSError:
            return ""

    ac = next((read(p) for p in glob.glob("/sys/class/power_supply/A*/online")), "")
    power = {"1": "enchufada", "0": "a batería"}.get(ac, "desconocido")
    profile = sh(["powerprofilesctl", "get"]) or "sin powerprofilesctl"
    governor = read("/sys/devices/system/cpu/cpu0/cpufreq/scaling_governor") or "?"
    no_turbo = read("/sys/devices/system/cpu/intel_pstate/no_turbo")
    turbo = {"0": "turbo activo", "1": "turbo desactivado"}.get(no_turbo, "turbo ?")

    commit = sh(["git", "rev-parse", "--short", "HEAD"]) or "desconocido"
    if sh(["git", "status", "--porcelain", "--untracked-files=no"]):
        commit += " (con cambios sin commitear)"
    return {
        "date": time.strftime("%Y-%m-%d %H:%M UTC", time.gmtime()),
        "commit": commit,
        "cpu_model": model or platform.processor() or "desconocido",
        "physical_cores": physical or os.cpu_count() or 1,
        "logical_cpus": os.cpu_count() or 1,
        "compiler": args.compiler,
        "build": args.build,
        "power": f"{power}, perfil {profile}, governor {governor}, {turbo}",
        "repeats": args.repeats,
        "max_repeat_seconds": args.max_repeat_seconds,
        "cooldown": (f"antes de cada corrida, bajo {args.cooldown_below:.0f} °C"
                     if args.cooldown_below else "sin enfriamiento entre corridas"),
        "seed": args.seed,
    }


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    parser.add_argument("--engine", default=None)
    parser.add_argument("--output", required=True)
    parser.add_argument("--sizes", type=int, nargs="+", default=list(DEFAULT_SIZES))
    parser.add_argument("--threads", type=int, nargs="+", default=list(DEFAULT_THREADS))
    parser.add_argument("--repeats", type=int, default=DEFAULT_REPEATS)
    parser.add_argument("--max-repeat-seconds", type=float, default=DEFAULT_MAX_REPEAT_SECONDS)
    parser.add_argument("--cooldown-below", type=float, default=None,
                        help="antes de cada corrida, esperar a que la temperatura baje de estos °C")
    parser.add_argument("--seed", type=int, default=DEFAULT_SEED)
    parser.add_argument("--compiler", default="desconocido")
    parser.add_argument("--build", default="desconocido")
    parser.add_argument("--rerender", metavar="JSON",
                        help="regenerar el reporte desde las mediciones guardadas, sin correr el engine")
    args = parser.parse_args()

    if args.rerender:
        with open(args.rerender, encoding="utf-8") as f:
            data = json.load(f)
        report, verdicts = render_report(results_from_json(data), data["env"])
        with open(args.output, "w", encoding="utf-8") as f:
            f.write(report)
        print(f"Reporte regenerado: {args.output}")
        return 0 if all(v.status in ("pass", "external") for v in verdicts) else 2

    if not args.engine:
        parser.error("--engine es obligatorio salvo con --rerender")

    env = detect_environment(args)
    if env["physical_cores"] < max(args.threads):
        print(f"⚠️  {env['physical_cores']} núcleos físicos: las mediciones con más hilos usan hyperthreading.")
    results: List[SizeResult] = []
    for size in sorted(args.sizes):
        results.append(measure_size(args.engine, size, sorted(args.threads), args.repeats, args.seed,
                                    max_repeat_seconds=args.max_repeat_seconds,
                                    cooldown_below=args.cooldown_below))
        # Se reescribe después de cada tamaño: si una corrida de horas se corta, lo ya medido
        # queda en el reporte (con los tamaños faltantes marcados por el AC5).
        report, verdicts = render_report(results, env)
        with open(args.output, "w", encoding="utf-8") as f:
            f.write(report)
        # Las mediciones crudas, para poder regenerar el análisis sin repetir horas de medición.
        with open(os.path.splitext(args.output)[0] + ".json", "w", encoding="utf-8") as f:
            json.dump({"env": env, "results": [results_to_json(r) for r in results]}, f, indent=1)

    print(f"\nReporte: {args.output}")
    for v in verdicts:
        print(f"   {v.label}: {v.measured:<40} {STATUS_ICON[v.status]}")
    # 0 solo si todo lo medible pasa y no quedó nada sin medir: un AC que no se pudo medir
    # tampoco está demostrado.
    return 0 if all(v.status in ("pass", "external") for v in verdicts) else 2


if __name__ == "__main__":
    sys.exit(main())
