package qr

import (
	"strings"
	"testing"
)

func TestRenderShape(t *testing.T) {
	out, err := Render("https://passport.bilibili.com/h5-app/passport/login/scan?qrcode_key=abc")
	if err != nil {
		t.Fatal(err)
	}
	lines := strings.Split(strings.TrimRight(out, "\n"), "\n")
	if len(lines) < 15 {
		t.Fatalf("only %d lines", len(lines))
	}
	width := -1
	for _, l := range lines {
		plain := strings.TrimSuffix(strings.TrimPrefix(l, "\x1b[30;47m"), "\x1b[0m")
		if n := len([]rune(plain)); width < 0 {
			width = n
		} else if n != width {
			t.Fatalf("ragged rows: %d vs %d", n, width)
		}
	}
	if !strings.ContainsAny(out, "█▀▄") {
		t.Fatal("no dark modules drawn")
	}
}
