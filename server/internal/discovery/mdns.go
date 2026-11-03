package discovery

import (
	"context"
	"fmt"
	"net"
	"strings"
	"sync"
	"time"

	"github.com/miekg/dns"
	"golang.org/x/net/ipv4"
)

const (
	// ServiceType is the DNS-SD type the PSP browses for.
	ServiceType = "_wilimini._tcp.local."
	metaQuery   = "_services._dns-sd._udp.local."
	mdnsPort    = 5353
	recordTTL   = 120
	legacyTTL   = 10 // RFC 6762 6.7: answers to non-mDNS queriers are short-lived
	cacheFlush  = 1 << 15
)

var mdnsGroup = &net.UDPAddr{IP: net.IPv4(224, 0, 0, 251), Port: mdnsPort}

// Service is what we advertise.
type Service struct {
	Instance string // display name, e.g. "Desk PC"
	Host     string // DNS label for the A record, e.g. "desk-pc"
	Port     int    // HTTP port
	Proto    int
	Version  string
}

func (s *Service) instanceFQDN() string {
	// Dots would split the label; DNS-SD would escape them, so avoid the issue.
	return strings.ReplaceAll(s.Instance, ".", "-") + "." + ServiceType
}
func (s *Service) hostFQDN() string { return s.Host + ".local." }

func (s *Service) txt() []string {
	return []string{fmt.Sprintf("proto=%d", s.Proto), "name=" + s.Instance, "ver=" + s.Version}
}

// Answer builds the response to query, or nil when it asks about nothing we
// own. ips are the addresses of the interface the query arrived on. legacy is
// true for queriers that are not full mDNS stacks (source port != 5353), which
// get the query ID echoed, the question repeated and short TTLs.
func (s *Service) Answer(query *dns.Msg, ips []net.IP, legacy bool) *dns.Msg {
	if query.Response || len(query.Question) == 0 {
		return nil
	}
	ttl := uint32(recordTTL)
	class := uint16(dns.ClassINET)
	flushClass := uint16(dns.ClassINET | cacheFlush)
	if legacy {
		ttl, flushClass = legacyTTL, dns.ClassINET
	}
	resp := new(dns.Msg)
	resp.Response, resp.Authoritative = true, true
	if legacy {
		resp.Id = query.Id
		resp.Question = query.Question
	}
	ptr := func(name, target string) dns.RR {
		return &dns.PTR{Hdr: dns.RR_Header{Name: name, Rrtype: dns.TypePTR, Class: class, Ttl: ttl}, Ptr: target}
	}
	srv := func() dns.RR {
		return &dns.SRV{Hdr: dns.RR_Header{Name: s.instanceFQDN(), Rrtype: dns.TypeSRV, Class: flushClass, Ttl: ttl},
			Port: uint16(s.Port), Target: s.hostFQDN()}
	}
	txt := func() dns.RR {
		return &dns.TXT{Hdr: dns.RR_Header{Name: s.instanceFQDN(), Rrtype: dns.TypeTXT, Class: flushClass, Ttl: ttl}, Txt: s.txt()}
	}
	addrs := func() []dns.RR {
		var out []dns.RR
		for _, ip := range ips {
			if v4 := ip.To4(); v4 != nil {
				out = append(out, &dns.A{Hdr: dns.RR_Header{Name: s.hostFQDN(), Rrtype: dns.TypeA, Class: flushClass, Ttl: ttl}, A: v4})
			}
		}
		return out
	}
	known := func(target string) bool { // known-answer suppression (RFC 6762 7.1)
		for _, rr := range query.Answer {
			if p, ok := rr.(*dns.PTR); ok && strings.EqualFold(p.Ptr, target) && p.Hdr.Ttl >= recordTTL/2 {
				return true
			}
		}
		return false
	}
	want := func(t uint16, have uint16) bool { return t == have || t == dns.TypeANY }
	seen := map[string]bool{}
	add := func(dst *[]dns.RR, rrs ...dns.RR) {
		for _, rr := range rrs {
			key := rr.String()
			if !seen[key] {
				seen[key] = true
				*dst = append(*dst, rr)
			}
		}
	}
	for _, q := range query.Question {
		name := strings.ToLower(q.Name)
		switch {
		case name == strings.ToLower(ServiceType) && want(q.Qtype, dns.TypePTR):
			if !known(s.instanceFQDN()) {
				add(&resp.Answer, ptr(ServiceType, s.instanceFQDN()))
				add(&resp.Extra, srv(), txt())
				add(&resp.Extra, addrs()...)
			}
		case name == metaQuery && want(q.Qtype, dns.TypePTR):
			add(&resp.Answer, ptr(metaQuery, ServiceType))
		case name == strings.ToLower(s.instanceFQDN()):
			if want(q.Qtype, dns.TypeSRV) {
				add(&resp.Answer, srv())
				add(&resp.Extra, addrs()...)
			}
			if want(q.Qtype, dns.TypeTXT) {
				add(&resp.Answer, txt())
			}
		case name == strings.ToLower(s.hostFQDN()) && want(q.Qtype, dns.TypeA):
			add(&resp.Answer, addrs()...)
		}
	}
	if len(resp.Answer) == 0 {
		return nil
	}
	return resp
}

// Announcement is the unsolicited response sent at start-up; ttl 0 says goodbye.
func (s *Service) Announcement(ips []net.IP, goodbye bool) *dns.Msg {
	q := new(dns.Msg)
	q.SetQuestion(ServiceType, dns.TypePTR)
	msg := s.Answer(q, ips, false)
	if msg == nil {
		return nil
	}
	if goodbye {
		for _, section := range [][]dns.RR{msg.Answer, msg.Extra} {
			for _, rr := range section {
				rr.Header().Ttl = 0
			}
		}
	}
	return msg
}

// Responder runs the service on every multicast-capable interface.
type Responder struct {
	Service *Service
	Logf    func(format string, args ...any)

	mu    sync.Mutex
	conns []*ifConn
}

type ifConn struct {
	iface net.Interface
	ips   []net.IP
	raw   *net.UDPConn
	pc    *ipv4.PacketConn
}

func (r *Responder) logf(format string, args ...any) {
	if r.Logf != nil {
		r.Logf(format, args...)
	}
}

// Run binds the sockets and serves until ctx ends. It returns how many
// interfaces it is answering on, or an error when none could be opened.
func (r *Responder) Run(ctx context.Context) (int, error) {
	ifaces, err := net.Interfaces()
	if err != nil {
		return 0, err
	}
	for _, iface := range ifaces {
		if iface.Flags&net.FlagUp == 0 || iface.Flags&net.FlagMulticast == 0 || iface.Flags&net.FlagLoopback != 0 {
			continue
		}
		var ips []net.IP
		addrs, _ := iface.Addrs()
		for _, a := range addrs {
			if ipnet, ok := a.(*net.IPNet); ok && ipnet.IP.To4() != nil {
				ips = append(ips, ipnet.IP.To4())
			}
		}
		if len(ips) == 0 {
			continue
		}
		// Go sets SO_REUSEADDR (and SO_REUSEPORT on BSD/macOS) here, so this
		// coexists with Avahi / Bonjour on the same machine.
		raw, err := net.ListenMulticastUDP("udp4", &iface, mdnsGroup)
		if err != nil {
			r.logf("mdns: %s: %v", iface.Name, err)
			continue
		}
		pc := ipv4.NewPacketConn(raw)
		_ = pc.SetMulticastInterface(&iface)
		_ = pc.SetMulticastTTL(255)
		_ = pc.SetMulticastLoopback(false)
		c := &ifConn{iface: iface, ips: ips, raw: raw, pc: pc}
		r.mu.Lock()
		r.conns = append(r.conns, c)
		r.mu.Unlock()
		go r.serve(ctx, c)
	}
	r.mu.Lock()
	n := len(r.conns)
	r.mu.Unlock()
	if n == 0 {
		return 0, fmt.Errorf("no multicast-capable network interface could be opened")
	}
	go func() {
		<-ctx.Done()
		r.goodbye()
		r.mu.Lock()
		for _, c := range r.conns {
			c.raw.Close()
		}
		r.mu.Unlock()
	}()
	go r.announce(ctx)
	return n, nil
}

func (r *Responder) announce(ctx context.Context) {
	for i := 0; i < 3; i++ {
		r.mu.Lock()
		for _, c := range r.conns {
			r.send(c, r.Service.Announcement(c.ips, false), mdnsGroup)
		}
		r.mu.Unlock()
		select {
		case <-ctx.Done():
			return
		case <-time.After(time.Second << i):
		}
	}
}

func (r *Responder) goodbye() {
	r.mu.Lock()
	defer r.mu.Unlock()
	for _, c := range r.conns {
		r.send(c, r.Service.Announcement(c.ips, true), mdnsGroup)
	}
}

func (r *Responder) send(c *ifConn, msg *dns.Msg, to *net.UDPAddr) {
	if msg == nil {
		return
	}
	b, err := msg.Pack()
	if err != nil {
		r.logf("mdns: pack: %v", err)
		return
	}
	if _, err := c.raw.WriteToUDP(b, to); err != nil {
		r.logf("mdns: send on %s: %v", c.iface.Name, err)
	}
}

func (r *Responder) serve(ctx context.Context, c *ifConn) {
	buf := make([]byte, 9000)
	for {
		n, from, err := c.raw.ReadFromUDP(buf)
		if err != nil {
			return
		}
		var q dns.Msg
		if q.Unpack(buf[:n]) != nil {
			continue
		}
		legacy := from.Port != mdnsPort
		resp := r.Service.Answer(&q, c.ips, legacy)
		if resp == nil {
			continue
		}
		dest := mdnsGroup
		if legacy {
			dest = from // unicast straight back to the PSP
		}
		r.send(c, resp, dest)
	}
}
