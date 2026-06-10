# HPC Engine — Benchmark Report

> Generado automáticamente por `run_benchmark.sh`
> Fecha: 2026-06-10 02:59 UTC

## Entorno de Ejecución

| Parámetro | Valor |
|---|---|
| CPU físicos disponibles | 8 |
| OMP max threads (engine) | 8 |
| Compilador | Debian clang version 19.1.7 (3+b1) |
| Build type | Release |

## Grafo: 500 nodos

| Hilos | Tiempo (ms) | Speedup vs 1 hilo |
|------:|------------:|------------------:|
| 1 | 22.739615 | 1.00× |
| 2 | 8.519637 | 2.66× |
| 4 | 5.117967 | 4.44× |
| 8 | 4.264433 | 5.33× |

## Grafo: 1000 nodos

| Hilos | Tiempo (ms) | Speedup vs 1 hilo |
|------:|------------:|------------------:|
| 1 | 62.834202 | 1.00× |
| 2 | 84.232312 | .74× |
| 4 | 36.329343 | 1.72× |
| 8 | 26.001204 | 2.41× |

## Grafo: 2500 nodos

| Hilos | Tiempo (ms) | Speedup vs 1 hilo |
|------:|------------:|------------------:|
| 1 | 407.639718 | 1.00× |
| 2 | 224.309226 | 1.81× |
| 4 | 139.234225 | 2.92× |
| 8 | 119.255731 | 3.41× |

## Grafo: 5000 nodos

| Hilos | Tiempo (ms) | Speedup vs 1 hilo |
|------:|------------:|------------------:|
| 1 | 1849.815801 | 1.00× |
| 2 | 912.11478 | 2.02× |
| 4 | 567.815362 | 3.25× |
| 8 | 483.435518 | 3.82× |

## Grafo: 10000 nodos

| Hilos | Tiempo (ms) | Speedup vs 1 hilo |
|------:|------------:|------------------:|
| 1 | 8054.174501 | 1.00× |
| 2 | 4554.15595 | 1.76× |
| 4 | 2758.640394 | 2.91× |
| 8 | 3187.54795 | 2.52× |

## Verificación de Criterios de Aceptación

| Criterio | Descripción | Speedup Medido | Mínimo Requerido | Estado |
|---|---|---:|---:|:---:|
| AC2a | 4 hilos, 5000 nodos | 3.25× | 2.0× | ✅ |
| AC2b | 8 hilos, 10000 nodos | 2.52× | 2.5× | ✅ |
| AC3  | 0 data races (TSAN) | — | 0 races | ⚙️ Ver paso TSAN en CI |

## Análisis — Ley de Amdahl

La Ley de Amdahl establece que el speedup máximo teórico con `p` procesadores
es `S(p) = 1 / (s + (1-s)/p)`, donde `s` es la fracción serial.

| Hilos (p) | Speedup teórico (s=0.1) | Speedup teórico (s=0.2) |
|----------:|------------------------:|------------------------:|
| 2 | 1.81× | 1.66× |
| 4 | 3.12× | 2.50× |
| 8 | 4.76× | 3.33× |
| 16 | 6.66× | 4.00× |

> Los speedups observados por encima del umbral de Amdahl (s=0.2) indican
> que la paralelización es efectiva para los algoritmos implementados.

## Conclusión

La paralelización con OpenMP produce mejoras de rendimiento significativas
para grafos de ≥ 5.000 nodos. Para grafos pequeños (< 1.000 nodos),
el overhead de creación y sincronización de hilos puede superar el beneficio.
