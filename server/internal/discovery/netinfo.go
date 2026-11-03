// Package discovery lets the PSP find the server without typing an address:
// an mDNS/DNS-SD responder (_wilimini._tcp) and a tiny UDP beacon for networks
// where multicast is filtered.
package discovery

import (
	"net"
	"os"
	"sort"
	"strings"
)

// LANAddrs returns the machine's usable IPv4 addresses, private ranges first.
// Link-local (169.254/16) addresses are only returned when nothing else exists.
func LANAddrs() []net.IP {
	ifaces, err := net.Interfaces()
	if err != nil {
		return nil
	}
	var good, linkLocal []net.IP
	for _, iface := range ifaces {
		if iface.Flags&net.FlagUp == 0 || iface.Flags&net.FlagLoopback != 0 {
			continue
		}
		addrs, _ := iface.Addrs()
		for _, a := range addrs {
			ipnet, ok := a.(*net.IPNet)
			if !ok {
				continue
			}
			ip := ipnet.IP.To4()
			if ip == nil || ip.IsLoopback() {
				continue
			}
			if ip.IsLinkLocalUnicast() {
				linkLocal = append(linkLocal, ip)
			} else {
				good = append(good, ip)
			}
		}
	}
	if len(good) == 0 {
		good = linkLocal
	}
	sort.SliceStable(good, func(i, j int) bool { return rank(good[i]) < rank(good[j]) })
	return good
}

// rank orders typical home-LAN addresses first.
func rank(ip net.IP) int {
	switch {
	case ip[0] == 192 && ip[1] == 168:
		return 0
	case ip[0] == 10:
		return 1
	case ip[0] == 172 && ip[1] >= 16 && ip[1] <= 31:
		return 2
	default:
		return 3
	}
}

// HostLabel is the machine name as a DNS label ("my-pc"), never empty.
func HostLabel() string {
	name, _ := os.Hostname()
	return Label(name, "wilimini")
}

// Label turns an arbitrary name into a lowercase letters/digits/hyphen label.
func Label(name, fallback string) string {
	name, _, _ = strings.Cut(name, ".")
	var b strings.Builder
	for _, r := range strings.ToLower(name) {
		switch {
		case r >= 'a' && r <= 'z', r >= '0' && r <= '9':
			b.WriteRune(r)
		case r == '-' || r == '_' || r == ' ':
			if b.Len() > 0 && !strings.HasSuffix(b.String(), "-") {
				b.WriteByte('-')
			}
		}
	}
	out := strings.Trim(b.String(), "-")
	if len(out) > 40 {
		out = strings.Trim(out[:40], "-")
	}
	if out == "" {
		return fallback
	}
	return out
}
