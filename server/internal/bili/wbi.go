package bili

import (
	"crypto/md5"
	"encoding/hex"
	"net/url"
	"sort"
	"strconv"
	"strings"
	"time"
)

// mixinTable is Bilibili's fixed permutation for the WBI key. It matches
// mixinKey() in client/api_client.cpp.
var mixinTable = [64]int{
	46, 47, 18, 2, 53, 8, 23, 32, 15, 50, 10, 31, 58, 3, 45, 35,
	27, 43, 5, 49, 33, 9, 42, 19, 29, 28, 14, 39, 12, 38, 41, 13,
	37, 48, 7, 16, 24, 55, 40, 61, 26, 17, 0, 1, 60, 51, 30, 4,
	22, 25, 54, 21, 56, 59, 6, 63, 57, 62, 11, 36, 20, 34, 44, 52,
}

// KeyFromURL extracts "abc" from ".../abc.png".
func KeyFromURL(u string) string {
	slash := strings.LastIndexByte(u, '/')
	dot := strings.LastIndexByte(u, '.')
	if slash < 0 || dot < 0 || slash >= dot {
		return ""
	}
	return u[slash+1 : dot]
}

// MixinKey derives the 32-character signing key from the two nav keys.
func MixinKey(imgKey, subKey string) string {
	raw := imgKey + subKey
	if len(raw) < 64 {
		return ""
	}
	out := make([]byte, 32)
	for i := range out {
		out[i] = raw[mixinTable[i]]
	}
	return string(out)
}

// percentEncode keeps the RFC 3986 unreserved set, like the PSP client.
func percentEncode(s string) string {
	const hexDigits = "0123456789ABCDEF"
	var b strings.Builder
	for i := 0; i < len(s); i++ {
		c := s[i]
		if c >= 'a' && c <= 'z' || c >= 'A' && c <= 'Z' || c >= '0' && c <= '9' || c == '-' || c == '_' || c == '.' || c == '~' {
			b.WriteByte(c)
			continue
		}
		b.WriteByte('%')
		b.WriteByte(hexDigits[c>>4])
		b.WriteByte(hexDigits[c&15])
	}
	return b.String()
}

// SignWBI returns the encoded query string, with wts and w_rid appended, for
// params. The caller sends exactly this string so both sides agree on escaping.
func SignWBI(params url.Values, mixin string, now time.Time) string {
	type pair struct{ k, v string }
	pairs := make([]pair, 0, len(params)+1)
	for k, vs := range params {
		if k == "w_rid" || k == "wts" {
			continue
		}
		for _, v := range vs {
			pairs = append(pairs, pair{percentEncode(k), percentEncode(v)})
		}
	}
	pairs = append(pairs, pair{"wts", strconv.FormatInt(now.Unix(), 10)})
	sort.Slice(pairs, func(i, j int) bool {
		if pairs[i].k != pairs[j].k {
			return pairs[i].k < pairs[j].k
		}
		return pairs[i].v < pairs[j].v
	})
	var query strings.Builder
	for i, p := range pairs {
		if i > 0 {
			query.WriteByte('&')
		}
		query.WriteString(p.k + "=" + p.v)
	}
	// The server computes the digest over the query without these characters.
	clean := strings.NewReplacer("!", "", "'", "", "(", "", ")", "", "*", "").Replace(query.String())
	sum := md5.Sum([]byte(clean + mixin))
	return query.String() + "&w_rid=" + hex.EncodeToString(sum[:])
}
