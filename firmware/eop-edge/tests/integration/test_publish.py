#!/usr/bin/env python3
"""US-401 against a real broker: the native_sim firmware publishing to Eclipse Mosquitto.

  AC2  samples arrive on eop/bunker/{node_id}/telemetry with QoS 1
  AC3  sampling -> broker delivery under 100 ms (P99 over the run)
  AC4  the payload is the JSON of docs/edge-telemetry-protocol-v1.0.md §2.1
  AC5  broker down: no crash, retries 1 s, 2 s, 4 s, 8 s … and publishing resumes when it is back
  AC6  first sample at the broker within 10 s of start

Usage: test_publish.py ZEPHYR_EXE   (make firmware-integration-test builds and passes it)
Needs Docker (the broker runs in a container) and the image named by EOP_MOSQUITTO_IMAGE.
"""

import calendar
import json
import os
import re
import socket
import subprocess
import sys
import tempfile
import time
import unittest

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from mqtt_sub import Subscriber  # noqa: E402

EXE = None
IMAGE = os.environ.get("EOP_MOSQUITTO_IMAGE", "eclipse-mosquitto:2.0.18")
CONF = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "..", "..", "..",
                    "mosquitto", "mosquitto.conf")
NODE_ID = "vault-it-1"
BUNKER = "Vault-IT-1"
TIMESTAMP = re.compile(r"^\d{4}-\d{2}-\d{2}T\d{2}:\d{2}:\d{2}\.\d{3}Z$")
BOOT_ID = re.compile(r"^[0-9a-f]{8}$")
LATENCY_LIMIT_S = 0.100
BOOT_LIMIT_S = 10.0
PERIOD_S = 1.0
PERIOD_TOLERANCE_S = 0.05


def free_port():
    with socket.socket() as s:
        s.bind(("127.0.0.1", 0))
        return s.getsockname()[1]


def wait_for(predicate, timeout, step=0.1):
    deadline = time.time() + timeout
    while time.time() < deadline:
        if predicate():
            return True
        time.sleep(step)
    return False


def parse_ts(text):
    seconds = calendar.timegm(time.strptime(text[:19], "%Y-%m-%dT%H:%M:%S"))
    return seconds + int(text[20:23]) / 1000


class Broker:
    def __init__(self):
        self.port = free_port()
        self.name = f"eop-fw-it-{os.getpid()}"
        subprocess.run(["docker", "rm", "-f", self.name], capture_output=True, check=False)
        subprocess.run(["docker", "run", "-d", "--name", self.name, "-p", f"127.0.0.1:{self.port}:1883",
                        "-v", f"{os.path.abspath(CONF)}:/mosquitto/config/mosquitto.conf:ro", IMAGE],
                       capture_output=True, check=True)
        if not wait_for(self.accepting, 20):
            raise RuntimeError("broker did not start")

    def accepting(self):
        try:
            Subscriber("127.0.0.1", self.port, "probe/#", client_id="eop-probe").close()
            return True
        except OSError:
            return False

    def stop(self):
        subprocess.run(["docker", "stop", "-t", "1", self.name], capture_output=True, check=True)

    def start(self):
        subprocess.run(["docker", "start", self.name], capture_output=True, check=True)
        if not wait_for(self.accepting, 20):
            raise RuntimeError("broker did not come back")

    def remove(self):
        subprocess.run(["docker", "rm", "-f", self.name], capture_output=True, check=False)


class FirmwarePublishTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.broker = Broker()

    @classmethod
    def tearDownClass(cls):
        cls.broker.remove()

    def start_firmware(self):
        self.log = tempfile.NamedTemporaryFile("w+", suffix=".log", delete=False)
        started = time.time()
        proc = subprocess.Popen([EXE, "--broker-host=127.0.0.1", f"--broker-port={self.broker.port}",
                                 f"--node-id={NODE_ID}", f"--bunker-name={BUNKER}"],
                                stdin=subprocess.DEVNULL, stdout=self.log, stderr=subprocess.STDOUT)
        self.addCleanup(self.stop_firmware, proc)
        return proc, started

    def stop_firmware(self, proc):
        if proc.poll() is None:
            proc.kill()
            proc.wait()

    def subscribe(self):
        sub = Subscriber("127.0.0.1", self.broker.port, "eop/bunker/+/telemetry")
        self.addCleanup(sub.close)
        return sub

    def logs(self):
        self.log.flush()
        with open(self.log.name, encoding="utf-8", errors="replace") as f:
            return f.read()

    def test_publishes_the_contract_every_second(self):
        sub = self.subscribe()
        proc, started = self.start_firmware()
        self.assertTrue(wait_for(lambda: len(sub.snapshot()) >= 12, 30), self.logs())
        self.assertIsNone(proc.poll(), "firmware exited")
        messages = sub.snapshot()

        # AC6: first sample at the broker within 10 s of start.
        self.assertLess(messages[0][0] - started, BOOT_LIMIT_S)

        latencies, previous = [], None
        for received, topic, raw in messages:
            payload = json.loads(raw)
            # AC2 + AC4
            self.assertEqual(topic, f"eop/bunker/{NODE_ID}/telemetry")
            self.assertEqual(set(payload), {"node_id", "bunker_name", "timestamp", "temperature",
                                            "status", "message_id", "boot_id"})
            self.assertEqual(payload["node_id"], NODE_ID)
            self.assertEqual(payload["bunker_name"], BUNKER)
            self.assertRegex(payload["timestamp"], TIMESTAMP)
            self.assertIsInstance(payload["temperature"], float)
            self.assertTrue(-50.0 <= payload["temperature"] <= 150.0)
            self.assertIn(payload["status"], ("OK", "WARN", "ALERT"))
            self.assertRegex(payload["boot_id"], BOOT_ID)
            sampled = parse_ts(payload["timestamp"])
            latencies.append(received - sampled)
            if previous:
                self.assertEqual(payload["message_id"], previous[0] + 1, "message ids are consecutive")
                self.assertEqual(payload["boot_id"], previous[1])
                self.assertAlmostEqual(sampled - previous[2], PERIOD_S, delta=PERIOD_TOLERANCE_S)
            else:
                self.assertEqual(payload["message_id"], 1)
            previous = (payload["message_id"], payload["boot_id"], sampled)

        # AC3: sampling -> broker under 100 ms. The receive time is taken by the subscriber
        # after the broker forwards it, so this is an upper bound of sample -> broker.
        latencies.sort()
        p99 = latencies[min(len(latencies) - 1, int(0.99 * len(latencies)))]
        print(f"\n  {len(messages)} samples, latency max {max(latencies) * 1000:.1f} ms, "
              f"p99 {p99 * 1000:.1f} ms, first sample {(messages[0][0] - started):.2f} s after start")
        self.assertLess(p99, LATENCY_LIMIT_S)

    def test_survives_the_broker_going_away(self):
        proc, _ = self.start_firmware()
        self.assertTrue(wait_for(lambda: "first sample acknowledged" in self.logs(), 15), self.logs())

        self.broker.stop()
        # Long enough for the 1, 2, 4 and 8 s attempts to be logged (AC5).
        self.assertTrue(wait_for(lambda: "next attempt in 8 s" in self.logs(), 30), self.logs())
        self.assertIsNone(proc.poll(), "firmware exited while the broker was down")
        delays = [int(d) for d in re.findall(r"next attempt in (\d+) s", self.logs())]
        self.assertEqual(delays[:4], [1, 2, 4, 8], "exponential backoff")

        self.broker.start()
        sub = self.subscribe()
        # The next attempt is at most 16 s away; then samples flow again.
        self.assertTrue(wait_for(lambda: len(sub.snapshot()) >= 3, 40), self.logs())
        self.assertIsNone(proc.poll())


if __name__ == "__main__":
    if len(sys.argv) < 2:
        sys.exit(__doc__)
    EXE = os.path.abspath(sys.argv.pop(1))
    unittest.main(verbosity=2)
