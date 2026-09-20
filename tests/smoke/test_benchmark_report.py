#!/usr/bin/env python3
"""
Tests del análisis de hpc-engine/benchmarks/benchmark_report.py (US-202).

No compilan ni corren el engine: el análisis son funciones puras, así que se verifican con datos
sintéticos de resultado conocido y con los datos reales de la corrida del 2026-09-17, cuyo
reporte original sacaba conclusiones que esos mismos datos no sostenían.
"""
import json
import os
import sys
import unittest

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "..", "hpc-engine", "benchmarks"))

import benchmark_report as br  # noqa: E402


def size_result(size, times_by_threads, cores_by_threads=None, mode="exacta"):
    """SizeResult con una corrida por hilo; times en segundos."""
    r = br.SizeResult(size=size, edges=4 * size, density=2.0, centrality_mode=mode)
    for t, seconds in times_by_threads.items():
        cores = (cores_by_threads or {}).get(t, float(t))
        r.cells[t] = br.Cell(size, t, [br.Run(processing_s=seconds, wall_s=1.0, cpu_s=cores)])
    return r


# Corrida real en i7-10510U (4 núcleos físicos, 8 hilos), 2026-09-17, tiempos en ms.
FEDE_2026_09_17_MS = {
    10_000: {1: 10176.747019, 2: 5057.758326, 4: 3951.549929, 8: 6218.553441},
    50_000: {1: 284345.912156, 2: 203114.941699, 4: 213043.771967, 8: 314691.391271},
    100_000: {1: 2377758.515894, 2: 1734697.960187, 4: 1489926.709168, 8: 1877061.3448},
}
FEDE_PHYSICAL_CORES = 4
# Lo que el reporte original presentó como fracción serial, calculada con 8 hilos de
# hyperthreading sobre 4 núcleos.
FEDE_ORIGINAL_SERIAL_FRACTION = 0.7642


def fede_results():
    return [size_result(n, {t: ms / 1000 for t, ms in row.items()}) for n, row in FEDE_2026_09_17_MS.items()]


class SerialFractionTest(unittest.TestCase):
    def test_recovers_the_serial_fraction_of_exact_amdahl_data(self):
        for s in (0.0, 0.05, 0.2, 0.5):
            speedups = {p: br.amdahl_speedup(s, p) for p in (2, 4, 8)}
            self.assertAlmostEqual(br.serial_fraction(speedups), s, places=9)

    def test_no_speedup_at_all_is_fully_serial(self):
        self.assertEqual(br.serial_fraction({2: 1.0, 4: 1.0}), 1.0)

    def test_superlinear_speedup_is_clamped_to_zero_not_negative(self):
        self.assertEqual(br.serial_fraction({2: 2.2, 4: 4.5}), 0.0)

    def test_without_multi_thread_points_there_is_nothing_to_fit(self):
        self.assertIsNone(br.serial_fraction({}))
        self.assertIsNone(br.serial_fraction({1: 1.0}))


class PhysicalCoresTest(unittest.TestCase):
    def test_only_multi_thread_counts_within_physical_cores_enter_the_fit(self):
        self.assertEqual(br.physical_thread_counts([1, 2, 4, 8], 4), [2, 4])
        self.assertEqual(br.physical_thread_counts([1, 2, 4, 8], 8), [2, 4, 8])

    def test_detects_hyperthreads_making_it_slower(self):
        r = size_result(10_000, {1: 10.0, 2: 5.0, 4: 4.0, 8: 6.0})
        self.assertTrue(br.slows_down_beyond_physical(r, 4))

    def test_thread_overhead_on_a_tiny_graph_is_not_called_a_memory_ceiling(self):
        # 10 nodos: pierde con cualquier cantidad de hilos, y con 8 todavía más. Es el costo de
        # arrancar los hilos, no memoria; la primera versión del análisis lo confundía.
        tiny = size_result(10, {1: 0.18, 2: 0.61, 4: 0.95, 8: 1.65})
        self.assertTrue(br.slows_down_beyond_physical(tiny, 4))
        self.assertFalse(br.memory_bound(tiny, 4))
        big = size_result(10_000, {1: 10.0, 2: 5.0, 4: 4.0, 8: 6.0})
        self.assertTrue(br.memory_bound(big, 4))

    def test_amdahl_is_not_fitted_where_parallelism_never_gains(self):
        env = {"date": "-", "commit": "-", "cpu_model": "-", "physical_cores": 4, "logical_cpus": 8,
               "compiler": "-", "build": "-", "repeats": 1, "seed": 0}
        report, _ = br.render_report([size_result(10, {1: 0.18, 2: 0.61, 4: 0.95, 8: 1.65}),
                                      size_result(10_000, {1: 10.0, 2: 5.0, 4: 4.0, 8: 6.0})], env)
        self.assertIn("| 10 | 2, 4 | n/a |", report)
        self.assertNotIn("| 10 | 2, 4 | 1.000 |", report)
        self.assertIn("En 10.000 nodos el techo es la memoria", report)
        self.assertNotIn("En 10, ", report)

    def test_a_slowdown_within_run_to_run_noise_is_not_flagged(self):
        # 200 nodos, medido: 4 hilos 3.37–3.49 ms, 8 (HT) 3.47–3.70 ms. Mediana más alta, pero
        # los rangos se superponen.
        r = br.SizeResult(size=200, edges=800, density=2.0, centrality_mode="exacta")
        runs = lambda *ms: [br.Run(processing_s=x / 1000, wall_s=1.0, cpu_s=1.0) for x in ms]
        r.cells = {1: br.Cell(200, 1, runs(7.99, 8.05, 8.25)),
                   4: br.Cell(200, 4, runs(3.37, 3.46, 3.49)),
                   8: br.Cell(200, 8, runs(3.47, 3.55, 3.70))}
        self.assertFalse(br.slows_down_beyond_physical(r, 4))
        r.cells[8] = br.Cell(200, 8, runs(3.60, 3.65, 3.70))   # ahora sí, claramente más lento
        self.assertTrue(br.slows_down_beyond_physical(r, 4))

    def test_a_millisecond_graph_that_barely_gains_is_not_called_a_memory_ceiling(self):
        # 100 nodos, medido el 19/09: 1 hilo 2,2 ms, 4 hilos gana 1,17×, 8 hilos claramente peor.
        # Gana y empeora con HT, pero en 2 ms lo que pesa es coordinar los hilos, no la memoria.
        r = size_result(100, {1: 0.0022, 2: 0.0026, 4: 0.0019, 8: 0.0042})
        self.assertTrue(br.gains_in_parallel(r, 4))
        self.assertTrue(br.slows_down_beyond_physical(r, 4))
        self.assertFalse(br.memory_bound(r, 4))

    def test_hyperthreads_that_still_help_are_not_flagged(self):
        r = size_result(10_000, {1: 10.0, 2: 5.0, 4: 3.0, 8: 2.8})
        self.assertFalse(br.slows_down_beyond_physical(r, 4))


class MinParallelSizeTest(unittest.TestCase):
    def test_threshold_is_the_smallest_size_from_which_every_larger_one_gains(self):
        results = [
            size_result(10, {1: 1.0, 2: 3.0, 4: 5.0}),     # pierde
            size_result(50, {1: 1.0, 2: 1.2, 4: 1.1}),     # 0.83x: pierde
            size_result(200, {1: 1.0, 2: 0.7, 4: 0.5}),    # 2x: gana
            size_result(1000, {1: 1.0, 2: 0.55, 4: 0.3}),  # gana
        ]
        threshold, no_gain = br.min_parallel_size(results, 4)
        self.assertEqual(threshold, 200)
        self.assertEqual(no_gain, [10, 50])

    def test_a_small_size_winning_by_noise_does_not_become_the_threshold(self):
        results = [
            size_result(10, {1: 1.0, 2: 0.8}),    # 1.25x, pero...
            size_result(50, {1: 1.0, 2: 0.99}),   # ...el siguiente no gana
            size_result(200, {1: 1.0, 2: 0.5}),
        ]
        threshold, no_gain = br.min_parallel_size(results, 4)
        self.assertEqual(threshold, 200)
        self.assertEqual(no_gain, [50])

    def test_gains_only_through_hyperthreading_do_not_count(self):
        results = [size_result(100, {1: 1.0, 2: 1.0, 4: 1.0, 8: 0.5})]
        threshold, _ = br.min_parallel_size(results, 4)
        self.assertIsNone(threshold)


class VerdictTest(unittest.TestCase):
    def test_eight_thread_criterion_is_unmeasurable_on_four_physical_cores(self):
        results = [size_result(100_000, {1: 30.0, 8: 5.0})]  # 6x: "pasaría" si se lo creyera
        v = br.speedup_verdict("AC3", results, 8, 100_000, 3.0, physical_cores=4)
        self.assertEqual(v.status, "unmeasurable")

    def test_speedup_criterion_uses_the_worst_qualifying_size(self):
        results = [
            size_result(10_000, {1: 10.0, 4: 1.0}),    # por debajo del mínimo: no cuenta
            size_result(50_000, {1: 10.0, 4: 4.0}),    # 2.5x
            size_result(100_000, {1: 10.0, 4: 6.0}),   # 1.67x  <- el peor
        ]
        v = br.speedup_verdict("AC2", results, 4, 50_000, 2.0, physical_cores=8)
        self.assertEqual(v.status, "fail")
        self.assertIn("100.000", v.measured)

    def test_ac5_requires_the_full_matrix(self):
        full = [size_result(n, {t: 1.0 for t in br.AC5_THREADS}) for n in br.AC5_SIZES]
        self.assertEqual(br.ac5_verdict(full).status, "pass")
        self.assertEqual(br.ac5_verdict(full[:2]).status, "fail")

    def test_ac8_fails_when_the_process_uses_more_cpu_than_its_threads(self):
        ok = [size_result(1000, {1: 1.0, 4: 0.3}, cores_by_threads={1: 1.0, 4: 3.9})]
        over = [size_result(1000, {1: 1.0, 4: 0.3}, cores_by_threads={1: 1.0, 4: 5.5})]
        self.assertEqual(br.ac8_verdict(ok).status, "pass")
        self.assertEqual(br.ac8_verdict(over).status, "fail")


class RepeatBudgetTest(unittest.TestCase):
    def test_a_measurement_slower_than_the_budget_is_not_repeated(self):
        calls = []

        def fake_run_engine(engine, graph, threads):
            calls.append(threads)
            seconds = 2.0 if threads == 1 else 0.1   # 1 hilo: lento; el resto: rápido
            return {"processing_time_ms": seconds * 1000}, br.Run(seconds, seconds, seconds)

        original = br.run_engine
        br.run_engine = fake_run_engine
        try:
            r = br.measure_size("engine", 10, [1, 2], repeats=3, seed=0, log=lambda *_: None,
                                max_repeat_seconds=1.0)
        finally:
            br.run_engine = original
        self.assertEqual(len(r.cells[1].runs), 1, "la corrida lenta no se repite")
        self.assertEqual(len(r.cells[2].runs), 3, "la rápida sí, hasta el máximo")


class CooldownTest(unittest.TestCase):
    def test_waits_until_the_cpu_is_below_the_threshold(self):
        temps = iter([92.0, 80.0, 65.0, 58.0])
        slept = []
        br.cool_down(60.0, log=lambda *_: None, sleep=slept.append, read_temp=lambda: next(temps))
        self.assertEqual(len(slept), 3)

    def test_gives_up_after_the_maximum_wait_instead_of_hanging(self):
        logged, slept = [], []
        br.cool_down(60.0, log=logged.append, sleep=slept.append, read_temp=lambda: 90.0)
        self.assertEqual(sum(br.COOLDOWN_POLL_S for _ in slept), br.COOLDOWN_MAX_WAIT_S)
        self.assertTrue(logged)

    def test_is_a_no_op_when_disabled(self):
        br.cool_down(None, read_temp=lambda: self.fail("no debería leer la temperatura"))


class RawMeasurementsTest(unittest.TestCase):
    def test_the_report_can_be_rebuilt_from_the_saved_measurements(self):
        results = fede_results()
        results[0].errors[16] = "falla simulada"
        rebuilt = br.results_from_json(json.loads(json.dumps({"results": [br.results_to_json(r) for r in results]})))
        env = {"date": "-", "commit": "-", "cpu_model": "-", "physical_cores": 4, "logical_cpus": 8,
               "compiler": "-", "build": "-", "repeats": 1, "seed": 0}
        self.assertEqual(br.render_report(rebuilt, env)[0], br.render_report(results, env)[0])


class FailedMeasurementTest(unittest.TestCase):
    def test_a_failed_cell_is_reported_in_its_row_and_counts_as_missing(self):
        results = [size_result(n, {t: 1.0 for t in br.AC5_THREADS}) for n in br.AC5_SIZES]
        del results[-1].cells[8]
        results[-1].errors[8] = "código de salida 2 — ERROR_ANALYSIS_FAILED: out of memory"
        env = {"date": "-", "commit": "-", "cpu_model": "-", "physical_cores": 8, "logical_cpus": 16,
               "compiler": "-", "build": "-", "repeats": 1, "seed": 0}
        report, verdicts = br.render_report(results, env)
        self.assertIn("| 8 | — | **ERROR** | código de salida 2 — ERROR_ANALYSIS_FAILED: out of memory |", report)
        ac5 = next(v for v in verdicts if v.label == "AC5")
        self.assertEqual(ac5.status, "fail")
        self.assertIn("100K/8h", ac5.note)


class RealRunRegressionTest(unittest.TestCase):
    """La corrida del 2026-09-17, con el análisis nuevo."""

    def setUp(self):
        self.results = fede_results()
        env = {"date": "2026-09-17", "commit": "test", "cpu_model": "i7-10510U",
               "physical_cores": FEDE_PHYSICAL_CORES, "logical_cpus": 8, "compiler": "clang 14",
               "build": "Release", "repeats": 1, "seed": 0}
        self.report, self.verdicts = br.render_report(self.results, env)
        self.by_label = {v.label: v for v in self.verdicts}

    def test_serial_fraction_no_longer_comes_from_hyperthreads(self):
        r100k = self.results[-1]
        s = br.serial_fraction({2: r100k.speedup(2), 4: r100k.speedup(4)})
        self.assertLess(s, FEDE_ORIGINAL_SERIAL_FRACTION)
        # La fila del reporte tiene que ser ese ajuste con 2 y 4 hilos, no el de 8 hyperthreads.
        self.assertIn(f"| 100.000 | 2, 4 | {s:.3f} |", self.report)
        self.assertNotIn(f"{FEDE_ORIGINAL_SERIAL_FRACTION:.3f}", self.report)

    def test_eight_thread_ac_is_reported_as_unmeasurable_not_failed(self):
        self.assertEqual(self.by_label["AC3"].status, "unmeasurable")

    def test_four_thread_ac_fails_at_fifty_thousand_nodes(self):
        v = self.by_label["AC2"]
        self.assertEqual(v.status, "fail")
        self.assertIn("1.33×", v.measured)
        self.assertIn("50.000", v.measured)

    def test_memory_ceiling_is_named_instead_of_blamed_on_serial_code(self):
        for r in self.results:
            self.assertTrue(br.slows_down_beyond_physical(r, FEDE_PHYSICAL_CORES), r.size)
        self.assertIn("límite de memoria", self.report)

    def test_conclusion_does_not_claim_a_threshold_the_data_cannot_locate(self):
        # Los tres tamaños ganan con 2 hilos, así que el umbral solo puede estar en 10K o debajo.
        # El reporte original afirmaba "< 1.000 nodos" sin haber medido nada menor a 10K.
        threshold, _ = br.min_parallel_size(self.results, FEDE_PHYSICAL_CORES)
        self.assertEqual(threshold, 10_000)
        self.assertIn("hay que medir tamaños menores", self.report)
        self.assertNotIn("1.000 nodos", self.report)

    def test_hyperthreading_warning_is_shown(self):
        self.assertIn("4 núcleos físicos", self.report)
        self.assertIn("8 (HT)", self.report)


class GraphTest(unittest.TestCase):
    def test_every_size_requests_the_same_algorithms(self):
        # generate_graph agrega shortest_paths debajo de 500 nodos; el benchmark no debe.
        for size in (10, 100, 499, 500, 1000):
            graph, _ = br.build_graph(size, seed=1)
            self.assertEqual(json.loads(graph)["payload"]["algorithms"], br.BENCHMARK_ALGORITHMS)

    def test_the_same_seed_yields_the_same_graph(self):
        self.assertEqual(br.build_graph(1000, seed=7), br.build_graph(1000, seed=7))
        self.assertNotEqual(br.build_graph(1000, seed=7), br.build_graph(1000, seed=8))


if __name__ == "__main__":
    unittest.main(verbosity=1)
