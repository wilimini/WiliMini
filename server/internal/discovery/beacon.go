package discovery

import (
	"bytes"
	"context"
	"fmt"
	"net"
	"strconv"
	"strings"
)

// BeaconPort is where the server listens for discovery probes.
const BeaconPort = 47612

// A probe is "WILIMINI-DISCOVER 1\n"; the reply is
// "WILIMINI-SERVER 1 <http port> <name>\n" sent to the prober. The PSP takes
// the server address from the datagram's source.
const (
	beaconRequest = "WILIMINI-DISCOVER"
	beaconReply   = "WILIMINI-SERVER"
	beaconVersion = 1
)

// BeaconReply answers a probe, or returns ok=false for anything else.
func BeaconReply(req []byte, httpPort int, name string) ([]byte, bool) {
	fields := strings.Fields(string(bytes.TrimSpace(req)))
	if len(fields) != 2 || fields[0] != beaconRequest {
		return nil, false
	}
	if v, err := strconv.Atoi(fields[1]); err != nil || v < 1 {
		return nil, false
	}
	name = strings.Map(func(r rune) rune {
		if r < 0x20 || r == 0x7f {
			return -1
		}
		return r
	}, name)
	return []byte(fmt.Sprintf("%s %d %d %s\n", beaconReply, beaconVersion, httpPort, name)), true
}

// ParseBeaconReply is the inverse of BeaconReply, used by tests and tools.
func ParseBeaconReply(b []byte) (port int, name string, ok bool) {
	line := strings.TrimSpace(string(b))
	parts := strings.SplitN(line, " ", 4)
	if len(parts) < 4 || parts[0] != beaconReply {
		return 0, "", false
	}
	port, err := strconv.Atoi(parts[2])
	if err != nil || port <= 0 || port > 65535 {
		return 0, "", false
	}
	return port, parts[3], true
}

// ServeBeacon answers probes on addr (":47612" by default) until ctx ends.
// It returns the bound address for tests.
func ServeBeacon(ctx context.Context, addr string, httpPort int, name func() string) (net.Addr, error) {
	pc, err := net.ListenPacket("udp4", addr)
	if err != nil {
		return nil, err
	}
	go func() {
		<-ctx.Done()
		pc.Close()
	}()
	go func() {
		buf := make([]byte, 256)
		for {
			n, from, err := pc.ReadFrom(buf)
			if err != nil {
				return
			}
			if reply, ok := BeaconReply(buf[:n], httpPort, name()); ok {
				_, _ = pc.WriteTo(reply, from)
			}
		}
	}()
	return pc.LocalAddr(), nil
}
