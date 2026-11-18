"""Shared pieces of the firmware integration tests: a Mosquitto broker in Docker and a test case
that starts the native_sim firmware, types on its shell and collects what it publishes."""

import calendar
import json
import os
import socket
import subprocess
import sys
import tempfile
import time
import unittest

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from mqtt_sub import Subscriber  # noqa: E402

IMAGE = os.environ.get("EOP_MOSQUITTO_IMAGE", "eclipse-mosquitto:2.0.18")
CONF = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "..", "..", "..",
                    "mosquitto", "mosquitto.conf")
NODE_ID = "vault-it-1"
BUNKER = "Vault-IT-1"


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


class FirmwareTestCase(unittest.TestCase):
    """Starts EXE (set by the test script) against one broker shared by the class."""

    EXE = None

    @classmethod
    def setUpClass(cls):
        cls.broker = Broker()

    @classmethod
    def tearDownClass(cls):
        cls.broker.remove()

    def start_firmware(self):
        self.log = tempfile.NamedTemporaryFile("w+", suffix=".log", delete=False)
        started = time.time()
        proc = subprocess.Popen([self.EXE, "--broker-host=127.0.0.1", f"--broker-port={self.broker.port}",
                                 f"--node-id={NODE_ID}", f"--bunker-name={BUNKER}"],
                                stdin=subprocess.PIPE, stdout=self.log, stderr=subprocess.STDOUT)
        self.addCleanup(self.stop_firmware, proc)
        return proc, started

    def stop_firmware(self, proc):
        if proc.poll() is None:
            proc.kill()
            proc.wait()
        proc.stdin.close()

    def shell(self, proc, command):
        """Types a command on the device shell (stdin of the native_sim process)."""
        proc.stdin.write(command.encode() + b"\n")
        proc.stdin.flush()

    def subscribe(self, topic_filter="eop/bunker/+/telemetry"):
        sub = Subscriber("127.0.0.1", self.broker.port, topic_filter)
        self.addCleanup(sub.close)
        return sub

    @staticmethod
    def on(sub, suffix):
        """Payloads received on eop/bunker/{NODE_ID}/{suffix}, in arrival order."""
        topic = f"eop/bunker/{NODE_ID}/{suffix}"
        return [json.loads(raw) for _, t, raw in sub.snapshot() if t == topic]

    def reconnected(self, sub):
        return [e for e in self.on(sub, "events") if e["event"] == "RECONNECTED"]

    def go_offline(self, proc, sub, seconds):
        """Cuts the simulated link for `seconds`, checks nothing is published meanwhile, and
        brings it back; returns how many telemetry messages had arrived before the cut."""
        self.assertTrue(wait_for(lambda: len(self.on(sub, "telemetry")) >= 3, 15), self.logs())
        self.shell(proc, "eop net down")
        self.assertTrue(wait_for(lambda: "link down: closing" in self.logs(), 5), self.logs())
        before = len(self.on(sub, "telemetry"))
        time.sleep(seconds)
        self.assertEqual(len(self.on(sub, "telemetry")), before, "published while offline")
        self.assertIsNone(proc.poll(), "firmware exited while offline")
        self.shell(proc, "eop net up")
        self.assertTrue(wait_for(lambda: self.reconnected(sub), 15), self.logs())
        return before

    def logs(self):
        self.log.flush()
        with open(self.log.name, encoding="utf-8", errors="replace") as f:
            return f.read()
