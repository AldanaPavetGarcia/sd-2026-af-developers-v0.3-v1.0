# HPC Engine — Benchmark Report

> Generado automáticamente por `run_benchmark.sh` (`make benchmark`).
> Fecha: 2026-09-22 02:07 UTC

## Entorno de ejecución

| Parámetro | Valor |
|---|---|
| Commit | `f872dc9 (con cambios sin commitear)` |
| CPU | Intel(R) Core(TM) i7-8550U CPU @ 1.80GHz |
| Núcleos físicos | **4** |
| Hilos lógicos | 8 |
| Energía | enchufada, perfil balanced, governor powersave, turbo activo |
| Compilador | c++ (Debian 14.2.0-19) 14.2.0 |
| Build | Release |
| Repeticiones por medición | hasta 3 (mediana); una sola si la corrida pasa de 60 s — ver columna n |
| Semilla del grafo | 20260918 |
| Enfriamiento | antes de cada corrida, bajo 60 °C |
| Algoritmos | connected_components, centrality (el mismo pedido en todos los tamaños) |

> ⚠️ **Esta máquina tiene 4 núcleos físicos.** Las mediciones con 8 hilos usan hyperthreading: dos hilos comparten un núcleo, su caché y su ancho de banda. Se informan, pero no miden paralelismo real y quedan fuera del análisis de Amdahl.

## Mediciones

El tiempo es `processing_time_ms` del engine: solo los algoritmos, sin parseo ni serialización. La columna CPU es el promedio de núcleos que usó el proceso completo (AC8).

### 10 nodos — 36 aristas (densidad 2.0)

Centralidad exacta.

| Hilos | n | Tiempo (mediana) | Rango | Speedup | Eficiencia | CPU (núcleos) |
|---:|---:|---:|---:|---:|---:|---:|
| 1 | 3 | 0.15 ms | 0.10 ms – 0.18 ms | 1.00× | 100% | 0.90 |
| 2 | 3 | 0.36 ms | 0.34 ms – 0.39 ms | 0.41× | 21% | 1.01 |
| 4 | 3 | 0.62 ms | 0.59 ms – 0.71 ms | 0.24× | 6% | 1.22 |
| 8 (HT) | 3 | 24.60 ms | 0.53 ms – 27.24 ms | 0.01× | 0% | 5.06 |

### 50 nodos — 193 aristas (densidad 2.0)

Centralidad exacta.

| Hilos | n | Tiempo (mediana) | Rango | Speedup | Eficiencia | CPU (núcleos) |
|---:|---:|---:|---:|---:|---:|---:|
| 1 | 3 | 0.23 ms | 0.22 ms – 0.24 ms | 1.00× | 100% | 0.93 |
| 2 | 3 | 0.40 ms | 0.30 ms – 0.68 ms | 0.57× | 28% | 1.03 |
| 4 | 3 | 0.40 ms | 0.39 ms – 0.55 ms | 0.57× | 14% | 1.27 |
| 8 (HT) | 3 | 0.68 ms | 0.65 ms – 0.73 ms | 0.34× | 4% | 1.88 |

### 100 nodos — 396 aristas (densidad 2.0)

Centralidad exacta.

| Hilos | n | Tiempo (mediana) | Rango | Speedup | Eficiencia | CPU (núcleos) |
|---:|---:|---:|---:|---:|---:|---:|
| 1 | 3 | 0.69 ms | 0.67 ms – 0.69 ms | 1.00× | 100% | 0.94 |
| 2 | 3 | 0.62 ms | 0.54 ms – 0.89 ms | 1.11× | 55% | 1.12 |
| 4 | 3 | 0.56 ms | 0.54 ms – 0.63 ms | 1.23× | 31% | 1.34 |
| 8 (HT) | 3 | 23.08 ms | 0.77 ms – 32.61 ms | 0.03× | 0% | 3.88 |

### 200 nodos — 796 aristas (densidad 2.0)

Centralidad exacta.

| Hilos | n | Tiempo (mediana) | Rango | Speedup | Eficiencia | CPU (núcleos) |
|---:|---:|---:|---:|---:|---:|---:|
| 1 | 3 | 4.08 ms | 3.88 ms – 7.99 ms | 1.00× | 100% | 0.97 |
| 2 | 3 | 2.37 ms | 2.37 ms – 2.40 ms | 1.72× | 86% | 1.18 |
| 4 | 3 | 1.70 ms | 1.68 ms – 1.83 ms | 2.40× | 60% | 1.46 |
| 8 (HT) | 3 | 1.74 ms | 1.17 ms – 5.37 ms | 2.35× | 29% | 2.17 |

### 500 nodos — 1.995 aristas (densidad 2.0)

Centralidad exacta.

| Hilos | n | Tiempo (mediana) | Rango | Speedup | Eficiencia | CPU (núcleos) |
|---:|---:|---:|---:|---:|---:|---:|
| 1 | 3 | 17.13 ms | 13.61 ms – 17.94 ms | 1.00× | 100% | 0.99 |
| 2 | 3 | 6.87 ms | 6.78 ms – 7.16 ms | 2.50× | 125% | 1.41 |
| 4 | 3 | 4.81 ms | 4.61 ms – 7.27 ms | 3.56× | 89% | 1.94 |
| 8 (HT) | 3 | 29.56 ms | 6.94 ms – 44.62 ms | 0.58× | 7% | 3.81 |

### 1.000 nodos — 3.997 aristas (densidad 2.0)

Centralidad exacta.

| Hilos | n | Tiempo (mediana) | Rango | Speedup | Eficiencia | CPU (núcleos) |
|---:|---:|---:|---:|---:|---:|---:|
| 1 | 3 | 55.73 ms | 54.12 ms – 56.71 ms | 1.00× | 100% | 0.99 |
| 2 | 3 | 30.87 ms | 28.45 ms – 32.56 ms | 1.81× | 90% | 1.40 |
| 4 | 3 | 17.22 ms | 15.43 ms – 18.33 ms | 3.24× | 81% | 2.53 |
| 8 (HT) | 3 | 13.61 ms | 13.02 ms – 13.80 ms | 4.09× | 51% | 2.59 |

### 5.000 nodos — 19.996 aristas (densidad 2.0)

Centralidad exacta.

| Hilos | n | Tiempo (mediana) | Rango | Speedup | Eficiencia | CPU (núcleos) |
|---:|---:|---:|---:|---:|---:|---:|
| 1 | 3 | 1.490 s | 1.482 s – 1.494 s | 1.00× | 100% | 1.00 |
| 2 | 3 | 764.41 ms | 743.85 ms – 797.75 ms | 1.95× | 97% | 1.89 |
| 4 | 3 | 435.62 ms | 412.43 ms – 472.54 ms | 3.42× | 86% | 3.43 |
| 8 (HT) | 3 | 353.62 ms | 346.63 ms – 356.78 ms | 4.21× | 53% | 6.07 |

### 10.000 nodos — 39.995 aristas (densidad 2.0)

Centralidad exacta.

| Hilos | n | Tiempo (mediana) | Rango | Speedup | Eficiencia | CPU (núcleos) |
|---:|---:|---:|---:|---:|---:|---:|
| 1 | 3 | 6.035 s | 6.024 s – 6.744 s | 1.00× | 100% | 1.00 |
| 2 | 3 | 3.242 s | 3.198 s – 3.322 s | 1.86× | 93% | 1.95 |
| 4 | 3 | 1.970 s | 1.967 s – 2.038 s | 3.06× | 77% | 3.76 |
| 8 (HT) | 3 | 1.677 s | 1.663 s – 1.682 s | 3.60× | 45% | 6.70 |

### 50.000 nodos — 124.998 aristas (densidad 0.5)

Centralidad muestreada (1024 de 50000 fuentes, ADR-019).

| Hilos | n | Tiempo (mediana) | Rango | Speedup | Eficiencia | CPU (núcleos) |
|---:|---:|---:|---:|---:|---:|---:|
| 1 | 3 | 3.781 s | 3.510 s – 3.795 s | 1.00× | 100% | 1.00 |
| 2 | 3 | 2.068 s | 2.047 s – 2.121 s | 1.83× | 91% | 1.77 |
| 4 | 3 | 1.322 s | 1.307 s – 1.343 s | 2.86× | 72% | 3.09 |
| 8 (HT) | 3 | 2.619 s | 2.524 s – 2.699 s | 1.44× | 18% | 5.38 |

### 100.000 nodos — 249.998 aristas (densidad 0.5)

Centralidad muestreada (1024 de 100000 fuentes, ADR-019).

| Hilos | n | Tiempo (mediana) | Rango | Speedup | Eficiencia | CPU (núcleos) |
|---:|---:|---:|---:|---:|---:|---:|
| 1 | 3 | 9.866 s | 9.074 s – 9.946 s | 1.00× | 100% | 1.00 |
| 2 | 3 | 6.125 s | 6.020 s – 6.642 s | 1.61× | 81% | 1.82 |
| 4 | 3 | 7.998 s | 7.965 s – 8.409 s | 1.23× | 31% | 3.59 |
| 8 (HT) | 3 | 12.3 s | 12.0 s – 12.4 s | 0.80× | 10% | 5.61 |

## Análisis — Ley de Amdahl

`S(p) = 1 / (s + (1 − s)/p)`, con `s` la fracción serial. `s` se ajusta por mínimos cuadrados con las mediciones de 2 a 4 hilos (los núcleos físicos). `S(8)` es la predicción del modelo para 8 núcleos **físicos**, no una medición.

| Nodos | Hilos usados en el ajuste | s | Speedup máximo (p→∞) | S(8) predicho |
|---:|---|---:|---:|---:|
| 10 | 2, 4 | n/a | — | — |
| 50 | 2, 4 | n/a | — | — |
| 100 | 2, 4 | 0.770 | 1.30× | 1.25× |
| 200 | 2, 4 | 0.203 | 4.92× | 3.30× |
| 500 | 2, 4 | 0.000 | ∞ | 8.00× |
| 1.000 | 2, 4 | 0.088 | 11.42× | 4.96× |
| 5.000 | 2, 4 | 0.047 | 21.22× | 6.02× |
| 10.000 | 2, 4 | 0.094 | 10.69× | 4.84× |
| 50.000 | 2, 4 | 0.121 | 8.28× | 4.33× |
| 100.000 | 2, 4 | 0.592 | 1.69× | 1.56× |

> 10, 50 nodos: **n/a** porque ninguna cantidad de hilos gana. El costo de crear el equipo de hilos supera al del trabajo; es el resultado del AC6, no una fracción serial.

> En 50.000, 100.000 nodos, agregar hilos por encima de los núcleos físicos **empeora** el tiempo. Amdahl no puede explicar eso: una fracción serial frena la ganancia, pero no la invierte. Es la firma de un límite de memoria compartida (caché y ancho de banda), no de código serial.

## Criterios de aceptación (US-202)

| AC | Qué pide | Medido | Requerido | Estado |
|---|---|---|---|:---:|
| AC2 | 4 hilos, grafos de ≥ 50.000 nodos | 1.23× (100.000 nodos) | ≥ 2.0× | ❌ |
| AC3 | 8 hilos, grafos de ≥ 100.000 nodos | — | ≥ 3.0× | ⚠️ no medible |
| AC4 | Cero data races (ThreadSanitizer) | — | 0 races | ⚙️ |
| AC5 | Matriz de mediciones y análisis de Amdahl | 12/12 | 1/2/4/8 hilos × 10K/50K/100K + Amdahl | ✅ |
| AC6 | Tamaño mínimo con ganancia al paralelizar | 100 nodos | speedup ≥ 1.10× | ✅ |
| AC8 | CPU usada ≤ OMP_NUM_THREADS | 1.00 núcleos con 1 hilo (10.000 nodos) | ≤ hilos | ✅ |

- **AC3**: la máquina tiene 4 núcleos físicos: con 8 hilos se mide hyperthreading, no 8 núcleos.
- **AC4**: no se mide acá: `make tsan` y el job `thread-sanitizer` de CI.
- **AC8**: promedio del proceso completo (CPU consumida / tiempo de pared), en el peor caso medido.

## Conclusión

- **Cuándo no paralelizar (AC6):** por debajo de 100 nodos la ganancia no supera 1.10× (sin ganancia: 10, 50 nodos); conviene correr con 1 hilo.
- **No se cumplen:** AC2.
- **No se pudieron medir en esta máquina:** AC3 — ver notas arriba.
- En 50.000, 100.000 nodos el techo es la memoria, no la fracción serial: más hilos sobre los mismos núcleos no agregan ancho de banda.
