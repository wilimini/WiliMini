// Package bili talks to Bilibili on behalf of the PSP: it owns the account
// cookies, the WBI signing key, QR login and play-URL selection.
package bili

import (
	"context"
	"encoding/json"
	"errors"
	"fmt"
	"io"
	"net/http"
	"net/url"
	"strings"
	"sync"
	"time"

	"github.com/wilimini/wilimini/server/internal/authstore"
)

const (
	DefaultUA      = "Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36 (KHTML, like Gecko) Chrome/124.0.0.0 Safari/537.36"
	DefaultReferer = "https://www.bilibili.com/"
	mixinTTL       = time.Hour
	maxJSONBytes   = 4 << 20
)

// APIError is a non-zero "code" in a Bilibili JSON envelope.
type APIError struct {
	Code    int
	Message string
}

func (e *APIError) Error() string { return fmt.Sprintf("bilibili code %d: %s", e.Code, e.Message) }

// IsLoginRequired reports code -101 (not logged in).
func IsLoginRequired(err error) bool {
	var api *APIError
	return errors.As(err, &api) && api.Code == -101
}

type Client struct {
	Store        *authstore.Store
	HTTP         *http.Client
	UA           string
	APIBase      string
	PassportBase string

	mu      sync.Mutex
	mixin   string
	mixinAt time.Time
}

func New(store *authstore.Store) *Client {
	return &Client{
		Store:        store,
		HTTP:         &http.Client{Timeout: 20 * time.Second},
		UA:           DefaultUA,
		APIBase:      "https://api.bilibili.com",
		PassportBase: "https://passport.bilibili.com",
	}
}

// Decorate adds the headers every upstream request needs. Cookies are only
// attached when the request has none, so callers can override them.
func (c *Client) Decorate(req *http.Request) {
	if req.Header.Get("User-Agent") == "" {
		req.Header.Set("User-Agent", c.UA)
	}
	if req.Header.Get("Referer") == "" {
		req.Header.Set("Referer", DefaultReferer)
	}
	if req.Header.Get("Cookie") == "" {
		if h := c.Store.Header(); h != "" {
			req.Header.Set("Cookie", h)
		}
	}
}

// Do sends req with the stored account and records any cookies in the reply.
func (c *Client) Do(req *http.Request) (*http.Response, error) {
	c.Decorate(req)
	resp, err := c.HTTP.Do(req)
	if err != nil {
		return nil, err
	}
	if cookies := resp.Cookies(); len(cookies) > 0 {
		if err := c.Store.Merge(cookies); err != nil {
			resp.Body.Close()
			return nil, err
		}
	}
	return resp, nil
}

type envelope struct {
	Code    int             `json:"code"`
	Message string          `json:"message"`
	Data    json.RawMessage `json:"data"`
	Result  json.RawMessage `json:"result"`
}

// GetJSON fetches a Bilibili JSON endpoint and returns its data (or result).
// A non-zero code is returned as *APIError together with the raw data.
func (c *Client) GetJSON(ctx context.Context, rawURL string) (json.RawMessage, error) {
	req, err := http.NewRequestWithContext(ctx, http.MethodGet, rawURL, nil)
	if err != nil {
		return nil, err
	}
	resp, err := c.Do(req)
	if err != nil {
		return nil, err
	}
	defer resp.Body.Close()
	body, err := io.ReadAll(io.LimitReader(resp.Body, maxJSONBytes))
	if err != nil {
		return nil, err
	}
	var env envelope
	if err := json.Unmarshal(body, &env); err != nil {
		return nil, fmt.Errorf("bilibili answered HTTP %d with a non-JSON body", resp.StatusCode)
	}
	data := env.Data
	if len(data) == 0 || string(data) == "null" {
		data = env.Result
	}
	if env.Code != 0 {
		return data, &APIError{Code: env.Code, Message: env.Message}
	}
	return data, nil
}

// NavInfo is the part of /x/web-interface/nav the server cares about.
type NavInfo struct {
	IsLogin bool   `json:"isLogin"`
	Mid     int64  `json:"mid"`
	Uname   string `json:"uname"`
	WbiImg  struct {
		ImgURL string `json:"img_url"`
		SubURL string `json:"sub_url"`
	} `json:"wbi_img"`
}

// Nav reads the account state and refreshes the WBI key as a side effect.
// Logged-out calls return code -101 but still carry the key.
func (c *Client) Nav(ctx context.Context) (*NavInfo, error) {
	data, err := c.GetJSON(ctx, c.APIBase+"/x/web-interface/nav")
	if err != nil && !IsLoginRequired(err) {
		return nil, err
	}
	var info NavInfo
	if len(data) > 0 {
		if jerr := json.Unmarshal(data, &info); jerr != nil {
			return nil, jerr
		}
	}
	if m := MixinKey(KeyFromURL(info.WbiImg.ImgURL), KeyFromURL(info.WbiImg.SubURL)); m != "" {
		c.mu.Lock()
		c.mixin, c.mixinAt = m, time.Now()
		c.mu.Unlock()
	}
	if info.IsLogin {
		_ = c.Store.SetIdentity(info.Mid, info.Uname)
	}
	return &info, nil
}

// Mixin returns the cached WBI key, refreshing it hourly or when force is set.
func (c *Client) Mixin(ctx context.Context, force bool) (string, error) {
	c.mu.Lock()
	m, at := c.mixin, c.mixinAt
	c.mu.Unlock()
	if !force && m != "" && time.Since(at) < mixinTTL {
		return m, nil
	}
	if _, err := c.Nav(ctx); err != nil {
		return "", err
	}
	c.mu.Lock()
	defer c.mu.Unlock()
	if c.mixin == "" {
		return "", errors.New("bilibili nav carried no WBI key")
	}
	return c.mixin, nil
}

// Sign returns the signed, encoded query for params.
func (c *Client) Sign(ctx context.Context, params url.Values, force bool) (string, error) {
	m, err := c.Mixin(ctx, force)
	if err != nil {
		return "", err
	}
	return SignWBI(params, m, time.Now()), nil
}

// EnsureBuvid fetches the device cookies that avoid risk-control rejections.
func (c *Client) EnsureBuvid(ctx context.Context) error {
	if c.Store.Get("buvid3") != "" {
		return nil
	}
	data, err := c.GetJSON(ctx, c.APIBase+"/x/frontend/finger/spi")
	if err != nil {
		return err
	}
	var spi struct {
		B3 string `json:"b_3"`
		B4 string `json:"b_4"`
	}
	if err := json.Unmarshal(data, &spi); err != nil {
		return err
	}
	if spi.B3 == "" {
		return errors.New("bilibili returned no buvid")
	}
	return c.Store.Merge([]*http.Cookie{{Name: "buvid3", Value: spi.B3}, {Name: "buvid4", Value: spi.B4}})
}

// SignedGet performs a WBI-signed GET and retries once with a fresh key when
// Bilibili rejects the signature (-403).
func (c *Client) SignedGet(ctx context.Context, endpoint string, params url.Values) (json.RawMessage, error) {
	_ = c.EnsureBuvid(ctx) // best effort; unsigned calls still work without it
	for attempt := 0; ; attempt++ {
		query, err := c.Sign(ctx, params, attempt > 0)
		if err != nil {
			return nil, err
		}
		data, err := c.GetJSON(ctx, endpoint+"?"+query)
		var api *APIError
		if errors.As(err, &api) && api.Code == -403 && attempt == 0 {
			continue
		}
		return data, err
	}
}

// IsBilibiliHost reports whether cookies may be stored from this host.
func IsBilibiliHost(host string) bool {
	host = strings.ToLower(host)
	return host == "bilibili.com" || strings.HasSuffix(host, ".bilibili.com")
}
