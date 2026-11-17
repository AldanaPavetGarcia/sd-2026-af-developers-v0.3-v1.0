#!/usr/bin/env python3
"""US-403 against a real broker, with the fault-injection build of the firmware (fault_inject.conf).

  AC1  a thread that stops checking in is restarted by the watchdog after ~5 s
  AC2  the restart counter of that thread is published to eop/bunker/{node_id}/health
  AC3  restarting one thread does not interrupt the other
  AC4  a fatal kernel error reboots the device, which publishes again (new boot_id)
  AC6  the fault is injected (`eop fault hang sensor|mqtt`, `eop fault crash`) and the counter is right
  AC7  the health report with the new counter arrives within 10 s of the restart

Usage: test_resilience.py ZEPHYR_EXE   (make firmware-integration-test builds and passes it)
"""

import json
import os
import sys
import time
import unittest

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from harness import NODE_ID, FirmwareTestCase, parse_ts, wait_for  # noqa: E402

WDT_TIMEOUT_S = 5.0
WDT_CHECK_S = 1.0
# A hung thread is restarted between the timeout and the timeout plus one check after its last
# check-in; one more second covers the sensor's sleep before it reaches the fault point.
RESTART_LIMIT_S = WDT_TIMEOUT_S + WDT_CHECK_S + 1.5
HEALTH_LIMIT_S = 10.0
REBOOT_LIMIT_S = 15.0
PERIOD_S = 1.0


class FirmwareResilienceTest(FirmwareTestCase):
    def restarted_at(self, thread):
        """Host time at which the firmware logged the restart of `thread`."""
        marker = f"{thread} thread restarted"
        self.assertTrue(wait_for(lambda: marker in self.logs(), RESTART_LIMIT_S + 2), self.logs())
        return time.time()

    def health_with(self, sub, thread, count):
        return [(t, h) for t, h in self.on_timed(sub, "health") if h["restarts"][thread] == count]

    @staticmethod
    def on_timed(sub, suffix):
        """(receive time, payload) on eop/bunker/{NODE_ID}/{suffix}, in arrival order."""
        topic = f"eop/bunker/{NODE_ID}/{suffix}"
        return [(t, json.loads(raw)) for t, tp, raw in sub.snapshot() if tp == topic]

    def hang(self, thread):
        sub = self.subscribe("eop/bunker/+/+")
        proc, _ = self.start_firmware()
        self.assertTrue(wait_for(lambda: len(self.on(sub, "telemetry")) >= 3, 15), self.logs())
        injected = time.time()
        self.shell(proc, f"eop fault hang {thread}")
        restarted = self.restarted_at(thread)
        # AC1: restarted after the watchdog timeout, not before and not much later.
        self.assertGreaterEqual(restarted - injected, WDT_TIMEOUT_S - PERIOD_S)
        self.assertLess(restarted - injected, RESTART_LIMIT_S + PERIOD_S)
        # AC2, AC6, AC7: the counter, right, on the health topic within 10 s.
        self.assertTrue(wait_for(lambda: self.health_with(sub, thread, 1), HEALTH_LIMIT_S),
                        self.logs())
        received, health = self.health_with(sub, thread, 1)[0]
        self.assertLess(received - restarted, HEALTH_LIMIT_S)
        other = "sensor" if thread == "mqtt" else "mqtt"
        self.assertEqual(health["restarts"][other], 0, "only the hung thread was restarted")
        self.assertIsNone(proc.poll(), "the device kept running")
        # Let a few samples through after the restart.
        count = len(self.on(sub, "telemetry"))
        self.assertTrue(wait_for(lambda: len(self.on(sub, "telemetry")) >= count + 3, 10),
                        self.logs())
        return sub, proc, health

    def test_hung_mqtt_thread_is_restarted_without_losing_samples(self):
        sub, _, _ = self.hang("mqtt")
        # AC3: the sensor kept sampling while the MQTT thread was hung; the samples waited in the
        # buffer and arrived after the restart: every id once, in order, one second apart.
        samples = self.on(sub, "telemetry")
        ids = [m["message_id"] for m in samples]
        self.assertEqual(ids, list(range(1, len(ids) + 1)))
        times = [parse_ts(m["timestamp"]) for m in samples]
        gaps = [b - a for a, b in zip(times, times[1:])]
        self.assertLess(max(gaps), PERIOD_S + 0.5, "the sensor never stopped")
        print(f"\n  mqtt hung: restarted, {len(ids)} samples in order without gaps or duplicates")

    def test_hung_sensor_thread_is_restarted_without_dropping_the_connection(self):
        sub, _, _ = self.hang("sensor")
        # AC3: the MQTT connection stayed up: no reconnection while the sensor was restarted.
        events = [e["event"] for e in self.on(sub, "events")]
        self.assertEqual(events, ["BOOT"])
        # The restarted sensor goes on with the same sequence: no reset to 1, no duplicates.
        samples = self.on(sub, "telemetry")
        ids = [m["message_id"] for m in samples]
        self.assertEqual(ids, list(range(1, len(ids) + 1)))
        times = [parse_ts(m["timestamp"]) for m in samples]
        gap = max(b - a for a, b in zip(times, times[1:]))
        self.assertGreater(gap, WDT_TIMEOUT_S - PERIOD_S, "the sensor really hung")
        print(f"\n  sensor hung: restarted, sampling paused {gap:.1f} s, ids continue to {ids[-1]}")

    def test_fatal_error_reboots_the_device(self):
        sub = self.subscribe("eop/bunker/+/+")
        proc, _ = self.start_firmware()
        self.assertTrue(wait_for(lambda: len(self.on(sub, "telemetry")) >= 3, 15), self.logs())
        first_boot = self.on(sub, "events")[0]["boot_id"]
        crashed = time.time()
        self.shell(proc, "eop fault crash")

        def boots():
            return [e for e in self.on(sub, "events") if e["event"] == "BOOT"]

        # AC4: a second BOOT, from a new boot, and telemetry starting over under it.
        self.assertTrue(wait_for(lambda: len(boots()) >= 2, REBOOT_LIMIT_S), self.logs())
        rebooted = time.time()
        self.assertNotEqual(boots()[1]["boot_id"], first_boot)
        self.assertIn("fatal kernel error", self.logs())
        self.assertTrue(wait_for(lambda: any(m["boot_id"] != first_boot
                                             for m in self.on(sub, "telemetry")), 10), self.logs())
        after = [m for m in self.on(sub, "telemetry") if m["boot_id"] != first_boot]
        self.assertEqual(after[0]["message_id"], 1)
        self.assertIsNone(proc.poll(), "the same process restarted itself")
        print(f"\n  fatal error: device back publishing {rebooted - crashed:.1f} s later")


if __name__ == "__main__":
    if len(sys.argv) < 2:
        sys.exit(__doc__)
    FirmwareTestCase.EXE = os.path.abspath(sys.argv.pop(1))
    unittest.main(verbosity=2)
