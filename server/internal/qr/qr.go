// Package qr draws a QR code with terminal half-block characters.
package qr

import (
	"strings"

	qrcode "github.com/skip2/go-qrcode"
)

// Render returns the code as lines of text. Dark modules are black on a white
// background, so it scans on both dark and light terminals; the quiet zone is
// part of the bitmap.
func Render(content string) (string, error) {
	code, err := qrcode.New(content, qrcode.Medium)
	if err != nil {
		return "", err
	}
	bits := code.Bitmap() // true = dark
	var b strings.Builder
	for y := 0; y < len(bits); y += 2 {
		b.WriteString("\x1b[30;47m")
		for x := 0; x < len(bits[y]); x++ {
			top := bits[y][x]
			bottom := y+1 < len(bits) && bits[y+1][x]
			switch {
			case top && bottom:
				b.WriteString("█")
			case top:
				b.WriteString("▀")
			case bottom:
				b.WriteString("▄")
			default:
				b.WriteString(" ")
			}
		}
		b.WriteString("\x1b[0m\n")
	}
	return b.String(), nil
}
