// Package proxy forwards the PSP client's Bilibili API calls. The PSP sends
// plain HTTP to /p/<host>/<path>; the server adds the account cookies, signs
// WBI requests and fills in the CSRF token, so the PSP never holds secrets.
package proxy

import (
	"bytes"
	"context"
	"encoding/json"
	"errors"
	"fmt"
	"io"
	"net/http"
	"net/url"
	"path"
	"strings"

	"github.com/wilimini/wilimini/server/internal/authstore"
	"github.com/wilimini/wilimini/server/internal/bili"
)

const (
	Prefix          = "/p/"
	maxRequestBody  = 1 << 20
	maxBufferedBody = 4 << 20
	maxRedirects    = 5
)

var apiHosts = map[string]bool{
	"api.bilibili.com":      true,
	"app.bilibili.com":      true,
	"api.vc.bilibili.com":   true,
	"api.live.bilibili.com": true,
	"passport.bilibili.com": true,
	"bangumi.bilibili.com":  true,
	"s.search.bilibili.com": true,
	"message.bilibili.com":  true,
	"www.bilibili.com":      true,
}

// Allowed reports whether host may be proxied. Hosts are matched exactly;
// "api.bilibili.com.evil.test" and IP literals never pass.
func Allowed(host string) bool {
	if apiHosts[host] {
		return true
	}
	return strings.HasSuffix(host, ".hdslb.com") && len(host) > len(".hdslb.com") && validLabels(host)
}

// credentialHost reports whether account cookies may be sent to host.
func credentialHost(host string) bool { return apiHosts[host] }

func validLabels(host string) bool {
	for _, r := range host {
		if !(r >= 'a' && r <= 'z' || r >= '0' && r <= '9' || r == '.' || r == '-') {
			return false
		}
	}
	return !strings.Contains(host, "..")
}

// Handler is the /p/ proxy.
type Handler struct {
	Bili     *bili.Client
	Store    *authstore.Store
	ReadOnly bool
	// Transport carries upstream requests (nil = http.DefaultTransport clone).
	Transport http.RoundTripper
	// Upstream maps an allowed host to its base URL. Nil means https://host.
	Upstream func(host string) string
	// OnRequest, when set, is told about each proxied request for the log.
	OnRequest func(method, host, path string, status int)

	client *http.Client
}

func (h *Handler) httpClient() *http.Client {
	if h.client == nil {
		rt := h.Transport
		if rt == nil {
			t := http.DefaultTransport.(*http.Transport).Clone()
			t.DisableCompression = true // forward the PSP's own Accept-Encoding
			rt = t
		}
		h.client = &http.Client{
			Transport: rt,
			CheckRedirect: func(req *http.Request, via []*http.Request) error {
				if len(via) >= maxRedirects {
					return errors.New("too many redirects")
				}
				if h.Upstream == nil && !Allowed(req.URL.Hostname()) {
					return fmt.Errorf("redirect to %s is not allowed", req.URL.Hostname())
				}
				return nil
			},
		}
	}
	return h.client
}

func writeError(w http.ResponseWriter, status int, code, message string) {
	w.Header().Set("Content-Type", "application/json; charset=utf-8")
	w.WriteHeader(status)
	_ = json.NewEncoder(w).Encode(map[string]string{"error": code, "message": message})
}

func (h *Handler) ServeHTTP(w http.ResponseWriter, r *http.Request) {
	rest := strings.TrimPrefix(r.URL.EscapedPath(), Prefix)
	host, upstreamPath, _ := strings.Cut(rest, "/")
	host = strings.ToLower(host)
	if !Allowed(host) {
		writeError(w, http.StatusForbidden, "host_not_allowed", "this host is not proxied")
		return
	}
	switch r.Method {
	case http.MethodGet, http.MethodHead:
	case http.MethodPost:
		if h.ReadOnly {
			writeError(w, http.StatusForbidden, "read_only", "the server is in read-only mode")
			return
		}
	default:
		writeError(w, http.StatusMethodNotAllowed, "method", "only GET, HEAD and POST are proxied")
		return
	}
	clean := path.Clean("/" + upstreamPath)
	if strings.HasSuffix(r.URL.EscapedPath(), "/") && clean != "/" {
		clean += "/"
	}
	rawQuery := stripToken(r.URL.RawQuery)

	var body []byte
	if r.Method == http.MethodPost {
		var err error
		body, err = io.ReadAll(io.LimitReader(r.Body, maxRequestBody+1))
		if err != nil || len(body) > maxRequestBody {
			writeError(w, http.StatusRequestEntityTooLarge, "body", "request body too large or unreadable")
			return
		}
		if credentialHost(host) {
			body = h.fillCSRF(r.Header.Get("Content-Type"), body)
		}
	}

	signed := strings.Contains(clean, "/wbi/") && !hasParam(rawQuery, "w_rid")
	resp, buffered, err := h.forward(r, host, clean, rawQuery, body, signed)
	if err != nil {
		status, code := http.StatusBadGateway, "upstream_unreachable"
		if errors.Is(err, context.Canceled) {
			return
		}
		writeError(w, status, code, err.Error())
		if h.OnRequest != nil {
			h.OnRequest(r.Method, host, clean, status)
		}
		return
	}
	defer resp.Body.Close()
	copyHeaders(w.Header(), resp.Header)
	w.WriteHeader(resp.StatusCode)
	if r.Method != http.MethodHead {
		if buffered != nil {
			_, _ = w.Write(buffered)
		} else {
			_, _ = io.Copy(w, resp.Body)
		}
	}
	if h.OnRequest != nil {
		h.OnRequest(r.Method, host, clean, resp.StatusCode)
	}
}

// forward sends the upstream request. For WBI endpoints it buffers the reply so
// a rejected signature (-403) can be retried once with a fresh key.
func (h *Handler) forward(r *http.Request, host, clean, rawQuery string, body []byte, signed bool) (*http.Response, []byte, error) {
	for attempt := 0; ; attempt++ {
		query := rawQuery
		if signed {
			params, err := url.ParseQuery(rawQuery)
			if err != nil {
				return nil, nil, err
			}
			_ = h.Bili.EnsureBuvid(r.Context())
			query, err = h.Bili.Sign(r.Context(), params, attempt > 0)
			if err != nil {
				return nil, nil, err
			}
		}
		req, err := h.newUpstream(r, host, clean, query, body, signed)
		if err != nil {
			return nil, nil, err
		}
		resp, err := h.httpClient().Do(req)
		if err != nil {
			return nil, nil, err
		}
		if cookies := resp.Cookies(); len(cookies) > 0 && credentialHost(host) {
			_ = h.Store.Merge(cookies)
		}
		if !signed {
			return resp, nil, nil
		}
		data, rerr := io.ReadAll(io.LimitReader(resp.Body, maxBufferedBody))
		resp.Body.Close()
		if rerr != nil {
			return nil, nil, rerr
		}
		if attempt == 0 && rejectedSignature(data) {
			continue
		}
		resp.Body = io.NopCloser(bytes.NewReader(nil))
		resp.Header.Del("Content-Length")
		resp.Header.Del("Content-Encoding")
		return resp, data, nil
	}
}

func rejectedSignature(body []byte) bool {
	var env struct {
		Code int `json:"code"`
	}
	return json.Unmarshal(body, &env) == nil && env.Code == -403
}

func (h *Handler) newUpstream(r *http.Request, host, clean, query string, body []byte, buffered bool) (*http.Request, error) {
	base := "https://" + host
	if h.Upstream != nil {
		base = strings.TrimRight(h.Upstream(host), "/")
	}
	target := base + clean
	if query != "" {
		target += "?" + query
	}
	var reader io.Reader
	if body != nil {
		reader = bytes.NewReader(body)
	}
	req, err := http.NewRequestWithContext(r.Context(), r.Method, target, reader)
	if err != nil {
		return nil, err
	}
	for _, name := range []string{"Accept", "Accept-Language", "Content-Type", "Range", "If-None-Match", "If-Modified-Since"} {
		if v := r.Header.Get(name); v != "" {
			req.Header.Set(name, v)
		}
	}
	if buffered {
		req.Header.Set("Accept-Encoding", "identity")
	} else if v := r.Header.Get("Accept-Encoding"); v != "" {
		req.Header.Set("Accept-Encoding", v)
	}
	req.Header.Set("User-Agent", h.Bili.UA)
	req.Header.Set("Referer", bili.DefaultReferer)
	if r.Method == http.MethodPost {
		req.Header.Set("Origin", "https://www.bilibili.com")
	}
	if credentialHost(host) {
		if c := h.Store.Header(); c != "" {
			req.Header.Set("Cookie", c)
		}
	}
	return req, nil
}

// fillCSRF sets csrf (and csrf_token when the client sent one) from the stored
// account, so the PSP does not need the bili_jct cookie.
func (h *Handler) fillCSRF(contentType string, body []byte) []byte {
	if !strings.HasPrefix(strings.ToLower(contentType), "application/x-www-form-urlencoded") {
		return body
	}
	jct := h.Store.CSRF()
	if jct == "" {
		return body
	}
	form, err := url.ParseQuery(string(body))
	if err != nil {
		return body
	}
	form.Set("csrf", jct)
	if form.Has("csrf_token") {
		form.Set("csrf_token", jct)
	}
	return []byte(form.Encode())
}

// stripToken removes the pairing token from the forwarded query.
func stripToken(rawQuery string) string {
	if rawQuery == "" {
		return ""
	}
	kept := make([]string, 0, 4)
	for _, part := range strings.Split(rawQuery, "&") {
		if part == "k" || strings.HasPrefix(part, "k=") || part == "" {
			continue
		}
		kept = append(kept, part)
	}
	return strings.Join(kept, "&")
}

func hasParam(rawQuery, name string) bool {
	for _, part := range strings.Split(rawQuery, "&") {
		if part == name || strings.HasPrefix(part, name+"=") {
			return true
		}
	}
	return false
}

var hopByHop = map[string]bool{
	"Connection": true, "Keep-Alive": true, "Proxy-Authenticate": true, "Proxy-Authorization": true,
	"Te": true, "Trailer": true, "Transfer-Encoding": true, "Upgrade": true,
	// Account state stays on the server.
	"Set-Cookie": true, "Set-Cookie2": true,
	// Do not let the page redirect the PSP into a different origin.
	"Strict-Transport-Security": true,
}

func copyHeaders(dst, src http.Header) {
	for name, values := range src {
		if hopByHop[http.CanonicalHeaderKey(name)] {
			continue
		}
		for _, v := range values {
			dst.Add(name, v)
		}
	}
}
