// Command eop-certgen leaves the TLS material of the edge broker in a directory (internal/certgen).
//
//	eop-certgen -dir /mosquitto/data/certs -provided /provided -hosts mosquitto,mosquitto.eop.svc -days 825
package main

import (
	"flag"
	"fmt"
	"os"
	"strings"
	"time"

	"github.com/AldanaPavetGarcia/sd-2026-af-developers-v0.3-v1.0/services/go/internal/certgen"
)

const hoursPerDay = 24

// nonEmpty drops the empty names an unset optional host list leaves ("a,b," or ",").
func nonEmpty(names []string) []string {
	out := names[:0]
	for _, name := range names {
		if name = strings.TrimSpace(name); name != "" {
			out = append(out, name)
		}
	}
	return out
}

func main() {
	dir := flag.String("dir", "", "directory the broker reads")
	provided := flag.String("provided", "", "directory with operator certificates (optional)")
	hosts := flag.String("hosts", "mosquitto", "comma-separated DNS names and IPs of the server certificate")
	days := flag.Int("days", 825, "validity of generated certificates")
	flag.Parse()

	outcome, err := certgen.Prepare(certgen.Options{
		Dir:      *dir,
		Provided: *provided,
		Hosts:    nonEmpty(strings.Split(*hosts, ",")),
		Validity: time.Duration(*days) * hoursPerDay * time.Hour,
	})
	if err != nil {
		fmt.Fprintln(os.Stderr, "eop-certgen:", err)
		os.Exit(1)
	}
	fmt.Printf("eop-certgen: %s certificates in %s\n", outcome, *dir)
}
