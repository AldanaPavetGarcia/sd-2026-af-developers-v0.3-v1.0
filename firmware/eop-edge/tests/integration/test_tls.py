#!/usr/bin/env python3
"""TLS to the broker's device listener (ADR-013): the native_sim firmware built with tls.conf.

  - with a broker certificate signed by the built-in CA and naming the host, samples arrive
  - a certificate that does not name the host is refused (hostname check)
  - a certificate signed by another CA is refused (chain check)
  - a refused broker is retried with backoff; nothing is published and the firmware keeps running

Usage: test_tls.py ZEPHYR_EXE CA_DIR   (make firmware-integration-test builds both)
CA_DIR holds ca.crt (built into ZEPHYR_EXE) and ca.key; the broker certificates are made here.
Needs Docker and openssl.
"""
import os
import shutil
import ssl
import subprocess
import sys
import tempfile
import unittest

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from harness import BUNKER, IMAGE, NODE_ID, free_port, wait_for  # noqa: E402
from mqtt_sub import Subscriber  # noqa: E402

CERT_DAYS = "2"
MOSQUITTO_UID_READABLE = 0o755


def openssl(*args):
    subprocess.run(["openssl", *args], capture_output=True, check=True)


def server_cert(directory, ca_crt, ca_key, san):
    """A broker key and certificate signed by (ca_crt, ca_key) with the given subjectAltName."""
    key, csr, crt = (os.path.join(directory, name) for name in ("tls.key", "tls.csr", "tls.crt"))
    ext = os.path.join(directory, "san.ext")
    with open(ext, "w", encoding="utf-8") as f:
        f.write(f"subjectAltName={san}\nbasicConstraints=CA:FALSE\n"
                "keyUsage=critical,digitalSignature,keyEncipherment\nextendedKeyUsage=serverAuth\n"
                "subjectKeyIdentifier=hash\nauthorityKeyIdentifier=keyid\n")
    openssl("req", "-newkey", "rsa:2048", "-nodes", "-sha256", "-keyout", key, "-out", csr,
            "-subj", "/CN=mosquitto")
    openssl("x509", "-req", "-in", csr, "-CA", ca_crt, "-CAkey", ca_key, "-CAcreateserial",
            "-days", CERT_DAYS, "-sha256", "-out", crt, "-extfile", ext)
    shutil.copy(ca_crt, os.path.join(directory, "ca.crt"))
    for name in os.listdir(directory):
        os.chmod(os.path.join(directory, name), 0o644)
    os.chmod(directory, MOSQUITTO_UID_READABLE)


class TlsBroker:
    """Mosquitto with only a TLS 1.2 listener, using the certificates in `certs`."""

    def __init__(self, certs, tag):
        self.port = free_port()
        self.certs = certs
        self.subscribers = 0
        self.name = f"eop-fw-tls-{tag}-{os.getpid()}"
        conf = os.path.join(certs, "mosquitto.conf")
        with open(conf, "w", encoding="utf-8") as f:
            f.write("listener 8883\nallow_anonymous true\ncafile /certs/ca.crt\n"
                    "certfile /certs/tls.crt\nkeyfile /certs/tls.key\ntls_version tlsv1.2\n")
        os.chmod(conf, 0o644)
        subprocess.run(["docker", "rm", "-f", self.name], capture_output=True, check=False)
        subprocess.run(["docker", "run", "-d", "--name", self.name, "-p", f"127.0.0.1:{self.port}:8883",
                        "-v", f"{certs}:/certs:ro",
                        "-v", f"{conf}:/mosquitto/config/mosquitto.conf:ro", IMAGE],
                       capture_output=True, check=True)
        if not wait_for(self.accepting, 20):
            raise RuntimeError("TLS broker did not start: " + self.docker_logs())

    def context(self):
        ctx = ssl.create_default_context(cafile=os.path.join(self.certs, "ca.crt"))
        ctx.check_hostname = False  # the test's own subscriber checks the chain only
        return ctx

    def accepting(self):
        try:
            Subscriber("127.0.0.1", self.port, "probe/#", client_id="eop-probe",
                       ssl_context=self.context()).close()
            return True
        except (OSError, ssl.SSLError):
            return False

    def subscribe(self, topic_filter):
        # A client id per subscriber: the broker must not take over the previous test's session.
        self.subscribers += 1
        return Subscriber("127.0.0.1", self.port, topic_filter, client_id=f"eop-tls-sub-{self.subscribers}",
                          ssl_context=self.context())

    def docker_logs(self):
        out = subprocess.run(["docker", "logs", self.name], capture_output=True, text=True, check=False)
        return out.stdout + out.stderr

    def remove(self):
        subprocess.run(["docker", "rm", "-f", self.name], capture_output=True, check=False)


class FirmwareTlsTest(unittest.TestCase):
    EXE = None
    CA_DIR = None

    @classmethod
    def setUpClass(cls):
        cls.tmp = tempfile.mkdtemp(prefix="eop-fw-tls-")
        ca_crt, ca_key = (os.path.join(cls.CA_DIR, n) for n in ("ca.crt", "ca.key"))
        other = os.path.join(cls.tmp, "other-ca")
        os.makedirs(other)
        openssl("req", "-x509", "-newkey", "rsa:2048", "-nodes", "-sha256", "-days", CERT_DAYS,
                "-keyout", os.path.join(other, "ca.key"), "-out", os.path.join(other, "ca.crt"),
                "-subj", "/CN=Another CA", "-addext", "basicConstraints=critical,CA:TRUE",
                "-addext", "keyUsage=critical,keyCertSign,cRLSign")
        dirs = {name: os.path.join(cls.tmp, name) for name in ("good", "foreign")}
        for d in dirs.values():
            os.makedirs(d)
        # Names localhost only: reaching it as 127.0.0.1 must fail the hostname check.
        server_cert(dirs["good"], ca_crt, ca_key, "DNS:localhost")
        server_cert(dirs["foreign"], os.path.join(other, "ca.crt"), os.path.join(other, "ca.key"),
                    "DNS:localhost")
        cls.good = TlsBroker(dirs["good"], "good")
        cls.foreign = TlsBroker(dirs["foreign"], "foreign")

    @classmethod
    def tearDownClass(cls):
        cls.good.remove()
        cls.foreign.remove()
        shutil.rmtree(cls.tmp, ignore_errors=True)

    def start_firmware(self, host, port):
        log = tempfile.NamedTemporaryFile("w+", suffix=".log", delete=False)
        proc = subprocess.Popen([self.EXE, f"--broker-host={host}", f"--broker-port={port}",
                                 f"--node-id={NODE_ID}", f"--bunker-name={BUNKER}"],
                                stdin=subprocess.PIPE, stdout=log, stderr=subprocess.STDOUT)
        self.addCleanup(self.stop, proc, log)
        return proc, log

    @staticmethod
    def stop(proc, log):
        if proc.poll() is None:
            proc.kill()
            proc.wait()
        proc.stdin.close()
        log.close()
        os.unlink(log.name)

    @staticmethod
    def read(log):
        log.flush()
        with open(log.name, encoding="utf-8", errors="replace") as f:
            return f.read()

    def assert_refused(self, broker, host):
        sub = broker.subscribe("eop/#")
        self.addCleanup(sub.close)
        proc, log = self.start_firmware(host, broker.port)
        self.assertTrue(wait_for(lambda: "next attempt in 4 s" in self.read(log), 20), self.read(log))
        self.assertIsNone(proc.poll(), "firmware exited on a refused broker")
        self.assertNotIn("connected to", self.read(log))
        self.assertEqual(sub.snapshot(), [], "published to a broker it should not trust")

    def test_publishes_over_tls(self):
        sub = self.good.subscribe("eop/bunker/+/telemetry")
        self.addCleanup(sub.close)
        proc, log = self.start_firmware("localhost", self.good.port)
        self.assertTrue(wait_for(lambda: len(sub.snapshot()) >= 3, 20), self.read(log))
        self.assertIsNone(proc.poll())
        self.assertIn(f"connected to localhost:{self.good.port}", self.read(log))

    def test_refuses_a_certificate_for_another_name(self):
        self.assert_refused(self.good, "127.0.0.1")

    def test_refuses_a_certificate_from_another_ca(self):
        self.assert_refused(self.foreign, "localhost")


if __name__ == "__main__":
    if len(sys.argv) < 3:
        sys.exit(__doc__)
    FirmwareTlsTest.EXE, FirmwareTlsTest.CA_DIR = sys.argv.pop(1), os.path.abspath(sys.argv.pop(1))
    unittest.main(verbosity=2)
