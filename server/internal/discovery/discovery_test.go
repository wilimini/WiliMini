package discovery

import (
	"context"
	"net"
	"strings"
	"testing"
	"time"

	"github.com/miekg/dns"
)

var svc = &Service{Instance: "Desk PC", Host: "desk-pc", Port: 8787, Proto: 1, Version: "0.1.0"}
var lan = []net.IP{net.IPv4(192, 168, 1, 20)}

func query(name string, qtype uint16) *dns.Msg {
	q := new(dns.Msg)
	q.SetQuestion(name, qtype)
	q.Id = 0x1234
	return q
}

// roundTrip packs and unpacks like the wire does, so size and compression bugs show.
func roundTrip(t *testing.T, m *dns.Msg) *dns.Msg {
	t.Helper()
	b, err := m.Pack()
	if err != nil {
		t.Fatal(err)
	}
	var out dns.Msg
	if err := out.Unpack(b); err != nil {
		t.Fatal(err)
	}
	return &out
}

func TestBrowseAnswersWithPTRSRVTXTAndA(t *testing.T) {
	resp := svc.Answer(query(ServiceType, dns.TypePTR), lan, false)
	if resp == nil {
		t.Fatal("no answer")
	}
	resp = roundTrip(t, resp)
	if !resp.Response || !resp.Authoritative || resp.Id != 0 {
		t.Fatalf("flags/id: %+v", resp.MsgHdr)
	}
	ptr := resp.Answer[0].(*dns.PTR)
	// miekg/dns prints a space in a label escaped; on the wire it is a plain space.
	if got := strings.ReplaceAll(ptr.Ptr, `\ `, " "); got != "Desk PC._wilimini._tcp.local." {
		t.Fatal(ptr.Ptr)
	}
	var gotSRV, gotTXT, gotA bool
	for _, rr := range resp.Extra {
		switch v := rr.(type) {
		case *dns.SRV:
			gotSRV = v.Port == 8787 && v.Target == "desk-pc.local."
		case *dns.TXT:
			gotTXT = len(v.Txt) == 3 && v.Txt[0] == "proto=1" && v.Txt[1] == "name=Desk PC"
		case *dns.A:
			gotA = v.A.Equal(net.IPv4(192, 168, 1, 20))
		}
	}
	if !gotSRV || !gotTXT || !gotA {
		t.Fatalf("additional records incomplete: srv=%v txt=%v a=%v", gotSRV, gotTXT, gotA)
	}
}

func TestLegacyUnicastEchoesIDAndQuestionWithShortTTL(t *testing.T) {
	resp := roundTrip(t, svc.Answer(query(ServiceType, dns.TypePTR), lan, true))
	if resp.Id != 0x1234 || len(resp.Question) != 1 || resp.Question[0].Name != ServiceType {
		t.Fatalf("legacy reply must echo id and question: %+v", resp.MsgHdr)
	}
	for _, rr := range append(append([]dns.RR{}, resp.Answer...), resp.Extra...) {
		if rr.Header().Ttl > legacyTTL {
			t.Errorf("ttl %d too long for legacy unicast", rr.Header().Ttl)
		}
		if rr.Header().Class != dns.ClassINET {
			t.Errorf("legacy replies must not set the cache-flush bit: class %#x", rr.Header().Class)
		}
	}
}

func TestMulticastRepliesUseCacheFlushOnUniqueRecords(t *testing.T) {
	resp := svc.Answer(query(ServiceType, dns.TypePTR), lan, false)
	for _, rr := range resp.Extra {
		if rr.Header().Class&cacheFlush == 0 {
			t.Errorf("%T lacks the cache-flush bit", rr)
		}
	}
	if resp.Answer[0].Header().Class&cacheFlush != 0 {
		t.Error("shared PTR records must not set cache-flush")
	}
}

func TestDirectQueriesAndMetaQuery(t *testing.T) {
	if r := svc.Answer(query("Desk PC._wilimini._tcp.local.", dns.TypeSRV), lan, false); r == nil || r.Answer[0].(*dns.SRV).Port != 8787 {
		t.Fatalf("srv: %v", r)
	}
	if r := svc.Answer(query("desk PC._WILIMINI._tcp.local.", dns.TypeTXT), lan, false); r == nil {
		t.Fatal("matching must be case-insensitive")
	}
	if r := svc.Answer(query("desk-pc.local.", dns.TypeA), lan, false); r == nil || !r.Answer[0].(*dns.A).A.Equal(lan[0]) {
		t.Fatalf("a: %v", r)
	}
	if r := svc.Answer(query(metaQuery, dns.TypePTR), lan, false); r == nil || r.Answer[0].(*dns.PTR).Ptr != ServiceType {
		t.Fatalf("meta: %v", r)
	}
}

func TestIgnoresUnrelatedAndResponses(t *testing.T) {
	if svc.Answer(query("_http._tcp.local.", dns.TypePTR), lan, false) != nil {
		t.Error("answered an unrelated service")
	}
	if svc.Answer(query(ServiceType, dns.TypeAAAA), lan, false) != nil {
		t.Error("answered an unsupported type")
	}
	q := query(ServiceType, dns.TypePTR)
	q.Response = true
	if svc.Answer(q, lan, false) != nil {
		t.Error("answered a response")
	}
}

func TestKnownAnswerSuppression(t *testing.T) {
	q := query(ServiceType, dns.TypePTR)
	q.Answer = []dns.RR{&dns.PTR{Hdr: dns.RR_Header{Name: ServiceType, Rrtype: dns.TypePTR, Class: dns.ClassINET, Ttl: 100}, Ptr: "Desk PC._wilimini._tcp.local."}}
	if svc.Answer(q, lan, false) != nil {
		t.Error("should stay quiet when the querier already knows us")
	}
	q.Answer[0].Header().Ttl = 10 // about to expire, so answer again
	if svc.Answer(q, lan, false) == nil {
		t.Error("should refresh a nearly expired record")
	}
}

func TestGoodbyeHasZeroTTL(t *testing.T) {
	m := svc.Announcement(lan, true)
	for _, rr := range append(append([]dns.RR{}, m.Answer...), m.Extra...) {
		if rr.Header().Ttl != 0 {
			t.Errorf("%T ttl %d", rr, rr.Header().Ttl)
		}
	}
	if svc.Announcement(lan, false).Answer[0].Header().Ttl != recordTTL {
		t.Error("announcement ttl")
	}
}

func TestInstanceDotsAreNotLabelSeparators(t *testing.T) {
	s := &Service{Instance: "my.pc", Host: "h", Port: 1, Proto: 1}
	if got := s.instanceFQDN(); got != "my-pc._wilimini._tcp.local." {
		t.Fatal(got)
	}
	roundTrip(t, s.Answer(query(ServiceType, dns.TypePTR), lan, false))
}

func TestLabel(t *testing.T) {
	for in, want := range map[string]string{
		"My-PC.local": "my-pc", "Liu's Mac mini": "lius-mac-mini", "  ": "x", "---": "x", "桌面": "x",
		"a_b c": "a-b-c", "UPPER": "upper",
	} {
		if got := Label(in, "x"); got != want {
			t.Errorf("Label(%q)=%q want %q", in, got, want)
		}
	}
}

func TestBeaconRoundTrip(t *testing.T) {
	reply, ok := BeaconReply([]byte("WILIMINI-DISCOVER 1\n"), 8787, "Desk\nPC")
	if !ok {
		t.Fatal("probe refused")
	}
	port, name, ok := ParseBeaconReply(reply)
	if !ok || port != 8787 || name != "DeskPC" {
		t.Fatalf("%v %q %q", ok, name, reply)
	}
	for _, bad := range []string{"", "HELLO", "WILIMINI-DISCOVER", "WILIMINI-DISCOVER x", "WILIMINI-DISCOVER 0", "WILIMINI-DISCOVER 1 extra"} {
		if _, ok := BeaconReply([]byte(bad), 1, "n"); ok {
			t.Errorf("%q must be ignored", bad)
		}
	}
	if _, _, ok := ParseBeaconReply([]byte("WILIMINI-SERVER 1 99999 n")); ok {
		t.Error("port out of range")
	}
}

func TestBeaconServerOverUDP(t *testing.T) {
	ctx, cancel := context.WithCancel(context.Background())
	defer cancel()
	addr, err := ServeBeacon(ctx, "127.0.0.1:0", 8787, func() string { return "Desk" })
	if err != nil {
		t.Skipf("udp not available: %v", err)
	}
	conn, err := net.Dial("udp4", addr.String())
	if err != nil {
		t.Fatal(err)
	}
	defer conn.Close()
	conn.SetDeadline(time.Now().Add(2 * time.Second))
	conn.Write([]byte("junk"))
	conn.Write([]byte("WILIMINI-DISCOVER 1\n"))
	buf := make([]byte, 128)
	n, err := conn.Read(buf)
	if err != nil {
		t.Fatal(err)
	}
	if port, name, ok := ParseBeaconReply(buf[:n]); !ok || port != 8787 || name != "Desk" {
		t.Fatalf("%q", buf[:n])
	}
}

// Real multicast needs a configured interface, which sandboxes often lack.
func TestMDNSResponderOverTheNetwork(t *testing.T) {
	ctx, cancel := context.WithCancel(context.Background())
	defer cancel()
	r := &Responder{Service: svc, Logf: t.Logf}
	n, err := r.Run(ctx)
	if err != nil {
		t.Skipf("no multicast interface: %v", err)
	}
	t.Logf("answering on %d interface(s)", n)

	pc, err := net.ListenPacket("udp4", "0.0.0.0:0") // ephemeral port => legacy unicast
	if err != nil {
		t.Fatal(err)
	}
	defer pc.Close()
	q := query(ServiceType, dns.TypePTR)
	b, _ := q.Pack()
	for i := 0; i < 3; i++ {
		pc.WriteTo(b, mdnsGroup)
	}
	pc.SetReadDeadline(time.Now().Add(3 * time.Second))
	buf := make([]byte, 9000)
	size, _, err := pc.ReadFrom(buf)
	if err != nil {
		t.Skipf("multicast is not routable here (%v)", err)
	}
	var resp dns.Msg
	if err := resp.Unpack(buf[:size]); err != nil {
		t.Fatal(err)
	}
	if resp.Id != q.Id || len(resp.Answer) == 0 {
		t.Fatalf("unexpected reply %+v", resp)
	}
}
