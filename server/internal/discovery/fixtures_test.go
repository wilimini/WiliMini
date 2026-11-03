package discovery

import (
	"encoding/hex"
	"flag"
	"net"
	"os"
	"path/filepath"
	"strings"
	"testing"

	"github.com/miekg/dns"
)

var update = flag.Bool("update", false, "rewrite tests/data/wili_mdns_response.hex")

// The PSP client (client/stream_server.c) is tested against these files from C.
// This test keeps the two languages honest: the C query must be answerable by
// the Go responder, and the Go response must be byte-identical to what the C
// parser was tested with.
func fixtureDir() string { return filepath.Join("..", "..", "..", "tests", "data") }

func readHex(t *testing.T, name string) []byte {
	t.Helper()
	text, err := os.ReadFile(filepath.Join(fixtureDir(), name))
	if err != nil {
		t.Fatalf("%v (run `go test ./internal/discovery -update` from server/ to create it)", err)
	}
	b, err := hex.DecodeString(strings.Join(strings.Fields(string(text)), ""))
	if err != nil {
		t.Fatal(err)
	}
	return b
}

func TestPSPQueryIsAnsweredAndFixtureIsCurrent(t *testing.T) {
	query := readHex(t, "wili_mdns_query.hex")
	var q dns.Msg
	if err := q.Unpack(query); err != nil {
		t.Fatalf("the PSP's query does not parse: %v", err)
	}
	if len(q.Question) != 1 || q.Question[0].Name != ServiceType || q.Question[0].Qtype != dns.TypePTR {
		t.Fatalf("question %+v", q.Question)
	}
	s := &Service{Instance: "Desk PC", Host: "desk-pc", Port: 8787, Proto: 1, Version: "0.1.0"}
	resp := s.Answer(&q, []net.IP{net.IPv4(192, 168, 1, 20)}, true)
	if resp == nil {
		t.Fatal("the responder ignored the PSP's query")
	}
	got, err := resp.Pack()
	if err != nil {
		t.Fatal(err)
	}
	path := filepath.Join(fixtureDir(), "wili_mdns_response.hex")
	if *update {
		if err := os.WriteFile(path, []byte(hex.EncodeToString(got)+"\n"), 0o644); err != nil {
			t.Fatal(err)
		}
		return
	}
	if want := readHex(t, "wili_mdns_response.hex"); string(got) != string(want) {
		t.Fatalf("responder output changed; rerun with -update and re-check the C test\n got %x\nwant %x", got, want)
	}
}
