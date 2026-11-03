package bili

import (
	"net/url"
	"strings"
	"testing"
	"time"
)

// Regression vectors. The mixin key is the value I recall from public WBI
// write-ups for these two keys; it agrees with applying client/api_client.cpp's
// table. The digest was recomputed with `md5sum` over
// "bar=514&foo=114&wts=1684746385&zab=1919810" + mixin. Neither was captured
// from Bilibili, so they guard against drift, not against a wrong algorithm.
func TestMixinKeyKnownVector(t *testing.T) {
	got := MixinKey("7cd084941338484aae1ad9425b84077c", "4932caff0ff746eab6f01bf08b70ac45")
	if got != "ea1db124af3c7062474693fa704f4ff8" {
		t.Fatalf("mixin key %q", got)
	}
}

func TestSignWBIKnownVector(t *testing.T) {
	params := url.Values{"foo": {"114"}, "bar": {"514"}, "zab": {"1919810"}}
	signed := SignWBI(params, "ea1db124af3c7062474693fa704f4ff8", time.Unix(1684746385, 0))
	want := "bar=514&foo=114&wts=1684746385&zab=1919810&w_rid=72c16126b5c96f58937ead1e1ee9dad0"
	if signed != want {
		t.Fatalf("\n got %s\nwant %s", signed, want)
	}
}

func TestSignWBIStripsSpecialCharactersFromDigestOnly(t *testing.T) {
	signed := SignWBI(url.Values{"k": {"a(b)*!'c d"}}, "ea1db124af3c7062474693fa704f4ff8", time.Unix(1, 0))
	if !strings.HasPrefix(signed, "k=a%28b%29%2A%21%27c%20d&wts=1&w_rid=") {
		t.Fatalf("value must stay encoded in the sent query: %s", signed)
	}
}

func TestKeyFromURL(t *testing.T) {
	if k := KeyFromURL("https://i0.hdslb.com/bfs/wbi/7cd084941338484aae1ad9425b84077c.png"); k != "7cd084941338484aae1ad9425b84077c" {
		t.Fatal(k)
	}
	if KeyFromURL("nonsense") != "" {
		t.Fatal("expected empty key")
	}
}
