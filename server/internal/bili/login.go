package bili

import (
	"context"
	"encoding/json"
	"errors"
	"net/url"
)

type QRState int

const (
	QRWaitingScan QRState = iota
	QRWaitingConfirm
	QRExpired
	QRDone
)

func (s QRState) String() string {
	return [...]string{"waiting for scan", "waiting for confirmation", "expired", "signed in"}[s]
}

// QRGenerate starts a QR login and returns the poll key and the text to encode.
func (c *Client) QRGenerate(ctx context.Context) (key, content string, err error) {
	data, err := c.GetJSON(ctx, c.PassportBase+"/x/passport-login/web/qrcode/generate")
	if err != nil {
		return "", "", err
	}
	var out struct {
		URL string `json:"url"`
		Key string `json:"qrcode_key"`
	}
	if err := json.Unmarshal(data, &out); err != nil {
		return "", "", err
	}
	if out.Key == "" || out.URL == "" {
		return "", "", errors.New("bilibili returned an empty QR payload")
	}
	return out.Key, out.URL, nil
}

// QRPoll checks the scan state. On success the cookies are already stored.
func (c *Client) QRPoll(ctx context.Context, key string) (QRState, error) {
	// The poll answers with HTTP 200 and the real status inside data.code.
	data, err := c.GetJSON(ctx, c.PassportBase+"/x/passport-login/web/qrcode/poll?qrcode_key="+url.QueryEscape(key))
	if err != nil {
		return QRWaitingScan, err
	}
	var out struct {
		Code    int    `json:"code"`
		Message string `json:"message"`
	}
	if err := json.Unmarshal(data, &out); err != nil {
		return QRWaitingScan, err
	}
	switch out.Code {
	case 86101:
		return QRWaitingScan, nil
	case 86090:
		return QRWaitingConfirm, nil
	case 86038:
		return QRExpired, nil
	case 0:
		if !c.Store.LoggedIn() {
			return QRWaitingScan, errors.New("login succeeded but no session cookie arrived")
		}
		_, _ = c.Nav(ctx) // records the account name for display
		return QRDone, nil
	default:
		return QRWaitingScan, &APIError{Code: out.Code, Message: out.Message}
	}
}
