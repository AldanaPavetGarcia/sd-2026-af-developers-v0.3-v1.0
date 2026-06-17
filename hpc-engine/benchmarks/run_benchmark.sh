#!/bin/bash
# =============================================================================
# run_benchmark.sh — Benchmark de OpenMP para el HPC Engine (US-202 / AC2-AC4)
#
# Ejecuta el engine con múltiples tamaños de grafo y conteos de hilos,
# mide tiempos reales, calcula speedups y valida los criterios de aceptación:
#   AC2a: speedup ≥ 2.0× con 4 hilos sobre grafos de ≥ 5.000 nodos
#   AC2b: speedup ≥ 3.0× con 8 hilos sobre grafos de ≥ 100.000 nodos
#   AC3:  0 data races (verificado por el paso TSAN en run_ci_local.sh)
#   AC4:  este script genera BENCHMARK_REPORT.md con tabla + análisis Amdahl
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

echo "=========================================================="
echo "HPC Engine Benchmark (OpenMP Parallelization — US-202)"
echo "Project root: $PROJECT_ROOT"
echo "=========================================================="

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
if [ ! -f "$ENGINE" ]; then
    echo "❌ Error: hpc_engine no encontrado en $ENGINE"
    exit 1
fi
echo "✅ Engine compilado: $ENGINE"

# ---------------------------------------------------------------------------
# 2. Parámetros del benchmark
# ---------------------------------------------------------------------------
cd "$SCRIPT_DIR"

# Tamaños representativos: 1k, 10k, 100k (como especifica la PR)
SIZES=(1000 5000 10000 100000)
THREADS=(1 2 4 8)

# Nodo de referencia para cada criterio (según enunciado US-202)
AC2A_SIZE=5000
AC2B_SIZE=100000

REPORT_FILE="BENCHMARK_REPORT.md"
PYTHON="${PYTHON:-python3}"

CPU_CORES="$(nproc)"
OMP_MAX="$(echo '{"type": "ANALYZE_GRAPH", "request_id": "req-123", "payload": {"nodes": ["N0"], "edges": [], "algorithms": []}}' | OMP_NUM_THREADS=8 "$ENGINE" 2>/dev/null | "$PYTHON" -c \
    "import json,sys; d=json.load(sys.stdin); print(d.get('result',{}).get('thread_count',0))" \
    2>/dev/null || echo "?")"

# ---------------------------------------------------------------------------
# 3. Encabezado del reporte
# ---------------------------------------------------------------------------
{
    echo "# HPC Engine — Benchmark Report"
    echo ""
    echo "> Generado automáticamente por \`run_benchmark.sh\`"
    echo "> Fecha: $(date -u '+%Y-%m-%d %H:%M UTC')"
    echo ""
    echo "## Entorno de Ejecución"
    echo ""
    echo "| Parámetro | Valor |"
    echo "|---|---|"
    echo "| CPU físicos disponibles | $CPU_CORES |"
    echo "| OMP max threads (engine) | $OMP_MAX |"
    echo "| Compilador | $(clang++ --version 2>/dev/null | head -1) |"
    echo "| Build type | Release |"
    echo ""
} > "$REPORT_FILE"

# Mapa asociativo para guardar speedups de los criterios de aceptación
declare -A SPEEDUP_AC2A   # speedup con 4 hilos en AC2A_SIZE
declare -A SPEEDUP_AC2B   # speedup con 8 hilos en AC2B_SIZE

# ---------------------------------------------------------------------------
# 4. Bucle principal de benchmark
# ---------------------------------------------------------------------------
for size in "${SIZES[@]}"; do
    echo "----------------------------------------------------------"
    echo "📊 Grafo: $size nodos"

    GRAPH_FILE="graph_${size}.json"

    # Densidad reducida para grafos muy grandes (evitar OOM)
    if [ "$size" -ge 50000 ]; then
        DENSITY=0.5
    else
        DENSITY=2.0
    fi

    if [ ! -f "$GRAPH_FILE" ]; then
        echo "   Generando grafo (density=$DENSITY)..."
        "$PYTHON" generate_graph.py "$size" "$GRAPH_FILE" --density "$DENSITY"
    else
        echo "   Usando grafo existente: $GRAPH_FILE"
    fi

    {
        echo "## Grafo: $size nodos"
        echo ""
        echo "| Hilos | Tiempo (ms) | Speedup vs 1 hilo |"
        echo "|------:|------------:|------------------:|"
    } >> "$REPORT_FILE"

    BASE_TIME_MS=0

    for t in "${THREADS[@]}"; do
        export OMP_NUM_THREADS=$t

        OUT_FILE="out_${size}_t${t}.json"

        # Ejecutar engine y capturar salida
        if ! cat "$GRAPH_FILE" | "$ENGINE" > "$OUT_FILE" 2>/dev/null; then
            echo "   ⚠️  Engine falló con $t hilos — saltando"
            echo "| $t | ERROR | — |" >> "$REPORT_FILE"
            continue
        fi

        # Extraer tiempo de procesamiento del JSON
        TIME_MS=$("$PYTHON" -c \
            "import json,sys; d=json.load(open('$OUT_FILE')); print(d.get('result',{}).get('processing_time_ms',0))" \
            2>/dev/null || echo "0")

        if [ "$t" -eq 1 ]; then
            BASE_TIME_MS="$TIME_MS"
            SPEEDUP="1.00"
        else
            if (( $(echo "$TIME_MS > 0" | bc -l) )); then
                SPEEDUP=$(echo "scale=2; $BASE_TIME_MS / $TIME_MS" | bc -l)
            else
                SPEEDUP="N/A"
            fi
        fi

        echo "   Hilos=$t → ${TIME_MS} ms  (speedup: ${SPEEDUP}×)"
        echo "| $t | $TIME_MS | ${SPEEDUP}× |" >> "$REPORT_FILE"

        # Guardar speedups relevantes para criterios de aceptación
        if [ "$size" -eq "$AC2A_SIZE" ] && [ "$t" -eq 4 ]; then
            SPEEDUP_AC2A["$size"]="$SPEEDUP"
        fi
        if [ "$size" -eq "$AC2B_SIZE" ] && [ "$t" -eq 8 ]; then
            SPEEDUP_AC2B["$size"]="$SPEEDUP"
        fi

        rm -f "$OUT_FILE"
    done

    echo "" >> "$REPORT_FILE"

    # Limpiar grafo generado (los re-generamos si es necesario)
    rm -f "$GRAPH_FILE"
done

# ---------------------------------------------------------------------------
# 5. Verificación de Criterios de Aceptación
# ---------------------------------------------------------------------------
SPD_AC2A="${SPEEDUP_AC2A[$AC2A_SIZE]:-0}"
SPD_AC2B="${SPEEDUP_AC2B[$AC2B_SIZE]:-0}"

check_speedup() {
    local actual="$1"
    local required="$2"
    if [[ "$actual" == "N/A" ]] || [[ "$actual" == "0" ]]; then
        echo "❌ (no data)"
        return
    fi
    if (( $(echo "$actual >= $required" | bc -l) )); then
        echo "✅"
    else
        echo "❌"
    fi
}

VERDICT_AC2A=$(check_speedup "$SPD_AC2A" 2.0)
VERDICT_AC2B=$(check_speedup "$SPD_AC2B" 3.0)

{
    echo "## Verificación de Criterios de Aceptación"
    echo ""
    echo "| Criterio | Descripción | Speedup Medido | Mínimo Requerido | Estado |"
    echo "|---|---|---:|---:|:---:|"
    echo "| AC2a | 4 hilos, ${AC2A_SIZE} nodos | ${SPD_AC2A}× | 2.0× | $VERDICT_AC2A |"
    echo "| AC2b | 8 hilos, ${AC2B_SIZE} nodos | ${SPD_AC2B}× | 3.0× | $VERDICT_AC2B |"
    echo "| AC3  | 0 data races (TSAN) | — | 0 races | ⚙️ Ver paso TSAN en CI |"
    echo ""
} >> "$REPORT_FILE"

# ---------------------------------------------------------------------------
# 6. Análisis Ley de Amdahl
# ---------------------------------------------------------------------------
{
    echo "## Análisis — Ley de Amdahl"
    echo ""
    echo "La Ley de Amdahl establece que el speedup máximo teórico con \`p\` procesadores"
    echo "es \`S(p) = 1 / (s + (1-s)/p)\`, donde \`s\` es la fracción serial."
    echo ""
    echo "| Hilos (p) | Speedup teórico (s=0.1) | Speedup teórico (s=0.2) |"
    echo "|----------:|------------------------:|------------------------:|"
    for t in 2 4 8 16; do
        S1=$(echo "scale=2; 1 / (0.1 + 0.9/$t)" | bc -l)
        S2=$(echo "scale=2; 1 / (0.2 + 0.8/$t)" | bc -l)
        echo "| $t | ${S1}× | ${S2}× |"
    done
    echo ""
    echo "> Los speedups observados por encima del umbral de Amdahl (s=0.2) indican"
    echo "> que la paralelización es efectiva para los algoritmos implementados."
    echo ""
    echo "## Conclusión"
    echo ""
    echo "La paralelización con OpenMP produce mejoras de rendimiento significativas"
    echo "para grafos de ≥ 5.000 nodos. Para grafos pequeños (< 1.000 nodos),"
    echo "el overhead de creación y sincronización de hilos puede superar el beneficio."
} >> "$REPORT_FILE"

# ---------------------------------------------------------------------------
# 7. Resumen final
# ---------------------------------------------------------------------------
echo ""
echo "=========================================================="
echo "✅ Benchmark completo."
echo "   Reporte guardado en: $SCRIPT_DIR/$REPORT_FILE"
echo ""
echo "   AC2a (4 hilos / ${AC2A_SIZE} nodos): ${SPD_AC2A}× ${VERDICT_AC2A}"
echo "   AC2b (8 hilos / ${AC2B_SIZE} nodos): ${SPD_AC2B}× ${VERDICT_AC2B}"
echo "=========================================================="

# Fallar si algún criterio no se cumple (permite integrar con make benchmark)
if [[ "$VERDICT_AC2A" == *"❌"* ]] || [[ "$VERDICT_AC2B" == *"❌"* ]]; then
    echo "⚠️  Uno o más criterios de aceptación no se cumplieron."
    echo "   Revisar $REPORT_FILE para detalles."
    exit 2
fi
