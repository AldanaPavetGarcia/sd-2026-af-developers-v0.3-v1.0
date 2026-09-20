#!/bin/bash
# =============================================================================
# run_benchmark.sh — Benchmark de OpenMP para el HPC Engine (US-202)
#
# Compila el engine en Release y delega en benchmark_report.py la medición, el
# análisis y el reporte (BENCHMARK_REPORT.md). Criterios de US-202 que evalúa:
#   AC2: speedup ≥ 2.0× con 4 hilos en grafos de ≥ 50.000 nodos
#   AC3: speedup ≥ 3.0× con 8 hilos en grafos de ≥ 100.000 nodos
#        (necesita 8 núcleos FÍSICOS; con menos se informa como "no medible")
#   AC5: mediciones con 1/2/4/8 hilos en 10K/50K/100K nodos + análisis de Amdahl
#   AC6: tamaño mínimo a partir del cual paralelizar da ganancia
#   AC8: el proceso no usa más CPU que OMP_NUM_THREADS
# AC4 (data races) no se mide acá: lo cubren `make tsan` y el job de CI.
#
# Variables opcionales:
#   HPC_ENGINE_BIN  binario ya compilado a medir; saltea el chequeo de vcpkg y el build
#   BENCH_SIZES     tamaños de grafo, separados por espacio
#   BENCH_THREADS   cantidades de hilos, separadas por espacio
#   BENCH_REPEATS   corridas por medición (se informa la mediana)
#   BENCH_MAX_REPEAT_SECONDS  no repetir una medición cuya corrida pase de estos segundos
#   BENCH_COOLDOWN_BELOW      antes de cada corrida, esperar a que la CPU baje de estos °C
#   (los valores por defecto están en benchmark_report.py: DEFAULT_SIZES, etc.)
#   BENCH_REPORT    archivo de salida   (default: BENCHMARK_REPORT.md junto al script)
# El reporte commiteado debe salir de la matriz completa por defecto; las
# variables sirven para probar el script sin esperar horas.
# =============================================================================
set -euo pipefail

# ---------------------------------------------------------------------------
# 1. Build del engine en modo Release (sin sanitizers, máximo rendimiento)
# ---------------------------------------------------------------------------
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_ROOT="$(cd "$SCRIPT_DIR/../.." && pwd)"

cd "$PROJECT_ROOT"

VCPKG_ROOT="${VCPKG_ROOT:-$HOME/vcpkg}"
BUILD_DIR="build-bench"

# ---------------------------------------------------------------------------
# vcpkg debe estar en el tag pinneado (el mismo que usan los dos Dockerfiles y,
# por lo tanto, el job docker-image-build de CI). Con otro commit de vcpkg, la
# variante *release* de civetweb se instala como bitcode LTO al que le falta
# CivetServer::getListeningPorts(), que prometheus-cpp-pull necesita: el link
# del engine muere con decenas de "undefined reference" que no tienen nada que
# ver con este repo. Se chequea antes de compilar porque el error aparece
# recién al final, tras compilar todo.
# ---------------------------------------------------------------------------
VCPKG_PINNED_TAG="2026.06.24"

if [ -z "${HPC_ENGINE_BIN:-}" ] && [ -d "$VCPKG_ROOT/.git" ]; then
    VCPKG_CURRENT="$(git -C "$VCPKG_ROOT" describe --tags 2>/dev/null || echo "desconocido")"

    if [ "$VCPKG_CURRENT" != "$VCPKG_PINNED_TAG" ]; then
        echo "ERROR: vcpkg en '$VCPKG_ROOT' está en '$VCPKG_CURRENT', se esperaba '$VCPKG_PINNED_TAG'." >&2
        echo "       El build Release no va a linkear. Corregilo con:" >&2
        echo "         git -C \"$VCPKG_ROOT\" fetch --depth 1 origin tag $VCPKG_PINNED_TAG" >&2
        echo "         git -C \"$VCPKG_ROOT\" checkout $VCPKG_PINNED_TAG" >&2
        echo "       o cloná uno nuevo:" >&2
        echo "         git clone --branch $VCPKG_PINNED_TAG --depth 1 https://github.com/microsoft/vcpkg.git" >&2
        exit 1
    fi
fi

echo "=========================================================="
echo "HPC Engine Benchmark (OpenMP Parallelization — US-202)"
echo "Project root: $PROJECT_ROOT"
echo "=========================================================="

if [ -n "${HPC_ENGINE_BIN:-}" ]; then
    ENGINE="$(realpath "$HPC_ENGINE_BIN")"
    BUILD_LABEL="binario externo (HPC_ENGINE_BIN) — verificar que esté optimizado"
else
    cmake -S . -B "$BUILD_DIR" \
        -G Ninja \
        -DCMAKE_BUILD_TYPE=Release \
        -DEXODUS_BUILD_TESTS=OFF \
        -DHPC_BUILD_TESTS=OFF \
        -DCMAKE_TOOLCHAIN_FILE="${VCPKG_ROOT}/scripts/buildsystems/vcpkg.cmake" \
        -DCMAKE_C_COMPILER=clang \
        -DCMAKE_CXX_COMPILER=clang++

    cmake --build "$BUILD_DIR" --target hpc_engine -j "$(nproc)"
    ENGINE="$PROJECT_ROOT/$BUILD_DIR/hpc-engine/hpc_engine"
    BUILD_LABEL="Release"
fi

if [ ! -x "$ENGINE" ]; then
    echo "❌ Error: hpc_engine no encontrado en $ENGINE"
    exit 1
fi
echo "✅ Engine: $ENGINE"

# ---------------------------------------------------------------------------
# 2. Medición, análisis y reporte
# ---------------------------------------------------------------------------
# El análisis vive en Python y no en bash para poder testearlo sin correr el engine
# (tests/smoke/test_benchmark_report.py).
cd "$SCRIPT_DIR"
PYTHON="${PYTHON:-python3}"

# Los valores por defecto viven solo en benchmark_report.py; acá se pasan únicamente los que el
# usuario haya cambiado, para no tener dos listas que se desincronicen.
ARGS=(--engine "$ENGINE"
      --output "${BENCH_REPORT:-$SCRIPT_DIR/BENCHMARK_REPORT.md}"
      --compiler "$(clang++ --version 2>/dev/null | head -1)"
      --build "$BUILD_LABEL")
# shellcheck disable=SC2206  # las listas se separan en varios argumentos a propósito
[ -n "${BENCH_SIZES:-}" ]   && ARGS+=(--sizes $BENCH_SIZES)
# shellcheck disable=SC2206
[ -n "${BENCH_THREADS:-}" ] && ARGS+=(--threads $BENCH_THREADS)
[ -n "${BENCH_REPEATS:-}" ] && ARGS+=(--repeats "$BENCH_REPEATS")
[ -n "${BENCH_MAX_REPEAT_SECONDS:-}" ] && ARGS+=(--max-repeat-seconds "$BENCH_MAX_REPEAT_SECONDS")
[ -n "${BENCH_COOLDOWN_BELOW:-}" ] && ARGS+=(--cooldown-below "$BENCH_COOLDOWN_BELOW")

exec "$PYTHON" benchmark_report.py "${ARGS[@]}"
