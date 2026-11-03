package config

import (
	"os"
	"path/filepath"
	"strings"
	"testing"
)

func TestLoadCreatesStableToken(t *testing.T) {
	dir := t.TempDir()
	first, err := Load(dir)
	if err != nil {
		t.Fatal(err)
	}
	if len(first.Token) != tokenLength {
		t.Fatalf("token %q", first.Token)
	}
	for _, r := range first.Token {
		if !strings.ContainsRune(tokenAlphabet, r) {
			t.Fatalf("token has %q outside the alphabet", r)
		}
	}
	second, err := Load(dir)
	if err != nil {
		t.Fatal(err)
	}
	if first.Token != second.Token {
		t.Fatal("token changed between loads")
	}
	if info, err := os.Stat(filepath.Join(dir, fileName)); err != nil || info.Mode().Perm()&0o077 != 0 && os.PathSeparator == '/' {
		t.Fatalf("config must be owner-only: %v %v", info, err)
	}
}

func TestNormalizeRepairsBadValues(t *testing.T) {
	dir := t.TempDir()
	os.WriteFile(filepath.Join(dir, fileName), []byte(`{"name":"a\nb","port":99999,"max_streams":-3}`), 0o600)
	cfg, err := Load(dir)
	if err != nil {
		t.Fatal(err)
	}
	if cfg.Port != DefaultPort || cfg.MaxStreams != DefaultMaxStreams || strings.ContainsAny(cfg.Name, "\n") || cfg.Token == "" {
		t.Fatalf("not repaired: %+v", cfg)
	}
}

func TestMalformedFileIsReported(t *testing.T) {
	dir := t.TempDir()
	os.WriteFile(filepath.Join(dir, fileName), []byte(`{nope`), 0o600)
	if _, err := Load(dir); err == nil {
		t.Fatal("expected an error")
	}
}
