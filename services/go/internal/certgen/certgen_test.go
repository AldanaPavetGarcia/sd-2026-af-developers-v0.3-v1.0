package certgen

import (
	"crypto/x509"
	"encoding/pem"
	"os"
	"path/filepath"
	"testing"
	"time"
)

func readCert(t *testing.T, path string) *x509.Certificate {
	t.Helper()
	data, err := os.ReadFile(path)
	if err != nil {
		t.Fatal(err)
	}
	block, _ := pem.Decode(data)
	if block == nil {
		t.Fatalf("%s: no PEM", path)
	}
	cert, err := x509.ParseCertificate(block.Bytes)
	if err != nil {
		t.Fatal(err)
	}
	return cert
}

func TestGeneratesAChainDevicesCanVerify(t *testing.T) {
	dir := filepath.Join(t.TempDir(), "certs")
	outcome, err := Prepare(Options{Dir: dir, Hosts: []string{"mosquitto", "mosquitto.eop.svc", "10.0.0.5"}, Validity: 24 * time.Hour})
	if err != nil || outcome != Generated {
		t.Fatalf("%s %v", outcome, err)
	}
	ca, server := readCert(t, filepath.Join(dir, CAFile)), readCert(t, filepath.Join(dir, CertFile))
	pool := x509.NewCertPool()
	pool.AddCert(ca)
	for _, name := range []string{"mosquitto", "mosquitto.eop.svc", "10.0.0.5"} {
		if _, err := server.Verify(x509.VerifyOptions{Roots: pool, DNSName: name}); err != nil {
			t.Fatalf("%s: %v", name, err)
		}
	}
	if _, err := server.Verify(x509.VerifyOptions{Roots: pool, DNSName: "other"}); err == nil {
		t.Fatal("a name that is not in the certificate verified")
	}
	info, err := os.Stat(filepath.Join(dir, KeyFile))
	if err != nil || info.Mode().Perm() != keyMode {
		t.Fatalf("key mode %v %v", info.Mode(), err)
	}
}

// A restart keeps the CA the devices already trust.
func TestKeepsEarlierCertificates(t *testing.T) {
	dir := t.TempDir()
	opts := Options{Dir: dir, Hosts: []string{"mosquitto"}, Validity: time.Hour}
	if _, err := Prepare(opts); err != nil {
		t.Fatal(err)
	}
	before, _ := os.ReadFile(filepath.Join(dir, CAFile))
	outcome, err := Prepare(opts)
	after, _ := os.ReadFile(filepath.Join(dir, CAFile))
	if err != nil || outcome != Kept || string(before) != string(after) {
		t.Fatalf("%s %v, CA changed: %v", outcome, err, string(before) != string(after))
	}
}

// Certificates the operator provides (a mounted Secret) replace generated ones.
func TestProvidedCertificatesWin(t *testing.T) {
	provided, dir := t.TempDir(), t.TempDir()
	if _, err := Prepare(Options{Dir: provided, Hosts: []string{"broker.vault-tec.example"}, Validity: time.Hour}); err != nil {
		t.Fatal(err)
	}
	if _, err := Prepare(Options{Dir: dir, Hosts: []string{"mosquitto"}, Validity: time.Hour}); err != nil {
		t.Fatal(err)
	}
	outcome, err := Prepare(Options{Dir: dir, Provided: provided, Hosts: []string{"mosquitto"}, Validity: time.Hour})
	if err != nil || outcome != Provided {
		t.Fatalf("%s %v", outcome, err)
	}
	if got := readCert(t, filepath.Join(dir, CertFile)).DNSNames; len(got) != 1 || got[0] != "broker.vault-tec.example" {
		t.Fatalf("server certificate not replaced: %v", got)
	}
	// An incomplete provided directory (Secret absent or partial) is ignored.
	partial := t.TempDir()
	if err := os.WriteFile(filepath.Join(partial, CAFile), []byte("x"), certMode); err != nil {
		t.Fatal(err)
	}
	if outcome, err := Prepare(Options{Dir: dir, Provided: partial, Hosts: []string{"mosquitto"}, Validity: time.Hour}); err != nil || outcome != Kept {
		t.Fatalf("%s %v", outcome, err)
	}
}

func TestValidityStartsBeforeNow(t *testing.T) {
	now := time.Date(2026, 11, 20, 12, 0, 0, 0, time.UTC)
	dir := t.TempDir()
	if _, err := Prepare(Options{Dir: dir, Hosts: []string{"mosquitto"}, Validity: 48 * time.Hour, Now: func() time.Time { return now }}); err != nil {
		t.Fatal(err)
	}
	cert := readCert(t, filepath.Join(dir, CertFile))
	if !cert.NotBefore.Before(now) || !cert.NotAfter.After(now.Add(46*time.Hour)) {
		t.Fatalf("validity %v .. %v", cert.NotBefore, cert.NotAfter)
	}
}

func TestErrors(t *testing.T) {
	if _, err := Prepare(Options{}); err == nil {
		t.Fatal("no directory accepted")
	}
	if _, err := Prepare(Options{Dir: t.TempDir()}); err == nil {
		t.Fatal("no hosts accepted")
	}
	file := filepath.Join(t.TempDir(), "file")
	if err := os.WriteFile(file, nil, certMode); err != nil {
		t.Fatal(err)
	}
	if _, err := Prepare(Options{Dir: filepath.Join(file, "sub"), Hosts: []string{"x"}}); err == nil {
		t.Fatal("unusable directory accepted")
	}
	readOnly := t.TempDir()
	if err := os.Chmod(readOnly, 0o500); err != nil {
		t.Fatal(err)
	}
	defer func() { _ = os.Chmod(readOnly, dirMode) }()
	if os.Geteuid() != 0 {
		if _, err := Prepare(Options{Dir: readOnly, Hosts: []string{"x"}, Validity: time.Hour}); err == nil {
			t.Fatal("unwritable directory accepted")
		}
	}
	// A provided directory whose files cannot be read.
	provided, dir := t.TempDir(), t.TempDir()
	for _, name := range []string{CAFile, CertFile, KeyFile} {
		if err := os.Mkdir(filepath.Join(provided, name), dirMode); err != nil {
			t.Fatal(err)
		}
	}
	if err := copyFile(filepath.Join(provided, "missing"), filepath.Join(dir, "x"), certMode); err == nil {
		t.Fatal("missing source accepted")
	}
	if err := copyFile(filepath.Join(provided, CAFile), filepath.Join(dir, "x"), certMode); err == nil {
		t.Fatal("directory as source accepted")
	}
	if err := os.WriteFile(filepath.Join(dir, "x"), []byte("x"), certMode); err != nil {
		t.Fatal(err)
	}
	if err := copyFile(filepath.Join(dir, "x"), filepath.Join(provided, CAFile), certMode); err == nil {
		t.Fatal("directory as destination accepted")
	}
}

func TestSerialsDiffer(t *testing.T) {
	if serial().Cmp(serial()) == 0 {
		t.Fatal("equal serials")
	}
}
