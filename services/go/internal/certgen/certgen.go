// Package certgen prepares the TLS material of the edge broker's device listener (ADR-013,
// US-406 AC1): in production it runs as an init container of Mosquitto, so a deployment needs no
// manual certificate step. Certificates the operator provides win; otherwise a CA and a server
// certificate are generated once and kept on the broker's volume, so devices can trust the same
// CA across restarts.
package certgen

import (
	"crypto/ecdsa"
	"crypto/elliptic"
	"crypto/rand"
	"crypto/x509"
	"crypto/x509/pkix"
	"encoding/pem"
	"errors"
	"fmt"
	"io"
	"math/big"
	"net"
	"os"
	"path/filepath"
	"time"
)

// File names, the ones mosquitto.conf and scripts/edge-certs.sh use.
const (
	CAFile   = "ca.crt"
	CertFile = "tls.crt"
	KeyFile  = "tls.key"
	caKey    = "ca.key"
)

const (
	serialBits = 128
	certMode   = 0o644
	keyMode    = 0o600
	dirMode    = 0o700
)

// Outcome says what Prepare did.
type Outcome string

const (
	// Provided: the operator's certificates were copied.
	Provided Outcome = "provided"
	// Kept: certificates from an earlier run were already there.
	Kept Outcome = "kept"
	// Generated: a new CA and server certificate were written.
	Generated Outcome = "generated"
)

// Options configure Prepare.
type Options struct {
	Dir      string   // where the broker reads ca.crt, tls.crt, tls.key
	Provided string   // optional directory with operator certificates (a mounted Secret)
	Hosts    []string // DNS names and IPs of the server certificate
	Validity time.Duration
	Now      func() time.Time
}

// Prepare leaves ca.crt, tls.crt and tls.key in opts.Dir.
func Prepare(opts Options) (Outcome, error) {
	if opts.Dir == "" {
		return "", errors.New("certgen: no directory")
	}
	if err := os.MkdirAll(opts.Dir, dirMode); err != nil {
		return "", err
	}
	if opts.Provided != "" && complete(opts.Provided) {
		for _, name := range []string{CAFile, CertFile, KeyFile} {
			mode := os.FileMode(certMode)
			if name == KeyFile {
				mode = keyMode
			}
			if err := copyFile(filepath.Join(opts.Provided, name), filepath.Join(opts.Dir, name), mode); err != nil {
				return "", err
			}
		}
		return Provided, nil
	}
	if complete(opts.Dir) {
		return Kept, nil
	}
	if len(opts.Hosts) == 0 {
		return "", errors.New("certgen: no hosts for the server certificate")
	}
	if err := generate(opts); err != nil {
		return "", err
	}
	return Generated, nil
}

func complete(dir string) bool {
	for _, name := range []string{CAFile, CertFile, KeyFile} {
		if info, err := os.Stat(filepath.Join(dir, name)); err != nil || info.Size() == 0 {
			return false
		}
	}
	return true
}

func generate(opts Options) error {
	now := time.Now
	if opts.Now != nil {
		now = opts.Now
	}
	start := now().Add(-time.Hour) // tolerate small clock differences with the devices
	end := start.Add(opts.Validity)

	caPriv, err := ecdsa.GenerateKey(elliptic.P256(), rand.Reader)
	if err != nil {
		return err
	}
	caTemplate := &x509.Certificate{
		SerialNumber:          serial(),
		Subject:               pkix.Name{CommonName: "EOP edge CA"},
		NotBefore:             start,
		NotAfter:              end,
		IsCA:                  true,
		BasicConstraintsValid: true,
		KeyUsage:              x509.KeyUsageCertSign | x509.KeyUsageCRLSign,
	}
	caDER, err := x509.CreateCertificate(rand.Reader, caTemplate, caTemplate, &caPriv.PublicKey, caPriv)
	if err != nil {
		return err
	}
	caCert, err := x509.ParseCertificate(caDER)
	if err != nil {
		return err
	}

	priv, err := ecdsa.GenerateKey(elliptic.P256(), rand.Reader)
	if err != nil {
		return err
	}
	template := &x509.Certificate{
		SerialNumber: serial(),
		Subject:      pkix.Name{CommonName: opts.Hosts[0]},
		NotBefore:    start,
		NotAfter:     end,
		KeyUsage:     x509.KeyUsageDigitalSignature,
		ExtKeyUsage:  []x509.ExtKeyUsage{x509.ExtKeyUsageServerAuth},
	}
	for _, host := range opts.Hosts {
		if ip := net.ParseIP(host); ip != nil {
			template.IPAddresses = append(template.IPAddresses, ip)
		} else {
			template.DNSNames = append(template.DNSNames, host)
		}
	}
	der, err := x509.CreateCertificate(rand.Reader, template, caCert, &priv.PublicKey, caPriv)
	if err != nil {
		return err
	}

	caKeyDER, err := x509.MarshalECPrivateKey(caPriv)
	if err != nil {
		return err
	}
	keyDER, err := x509.MarshalECPrivateKey(priv)
	if err != nil {
		return err
	}
	// The server files last, so an interrupted run is redone instead of half kept.
	files := []struct {
		name, kind string
		der        []byte
		mode       os.FileMode
	}{
		{caKey, "EC PRIVATE KEY", caKeyDER, keyMode},
		{CAFile, "CERTIFICATE", caDER, certMode},
		{KeyFile, "EC PRIVATE KEY", keyDER, keyMode},
		{CertFile, "CERTIFICATE", der, certMode},
	}
	for _, f := range files {
		data := pem.EncodeToMemory(&pem.Block{Type: f.kind, Bytes: f.der})
		if err := os.WriteFile(filepath.Join(opts.Dir, f.name), data, f.mode); err != nil {
			return err
		}
	}
	return nil
}

func serial() *big.Int {
	n, err := rand.Int(rand.Reader, new(big.Int).Lsh(big.NewInt(1), serialBits))
	if err != nil {
		return big.NewInt(time.Now().UnixNano())
	}
	return n
}

func copyFile(from, to string, mode os.FileMode) error {
	in, err := os.Open(from)
	if err != nil {
		return err
	}
	defer func() { _ = in.Close() }()
	out, err := os.OpenFile(to, os.O_CREATE|os.O_TRUNC|os.O_WRONLY, mode)
	if err != nil {
		return err
	}
	if _, err := io.Copy(out, in); err != nil {
		_ = out.Close()
		return fmt.Errorf("copy %s: %w", from, err)
	}
	return out.Close()
}
