package authstore

import (
	"net/http"
	"os"
	"testing"
)

func TestMergePersistsAndDeletes(t *testing.T) {
	dir := t.TempDir()
	s, err := Open(dir)
	if err != nil {
		t.Fatal(err)
	}
	if s.LoggedIn() {
		t.Fatal("fresh store must be logged out")
	}
	if err := s.Merge([]*http.Cookie{{Name: "SESSDATA", Value: "abc"}, {Name: "bili_jct", Value: "csrf1"}, {Name: "b_3", Value: "x"}}); err != nil {
		t.Fatal(err)
	}
	if got := s.Header(); got != "SESSDATA=abc; b_3=x; bili_jct=csrf1" {
		t.Fatalf("header %q", got)
	}
	again, err := Open(dir)
	if err != nil {
		t.Fatal(err)
	}
	if !again.LoggedIn() || again.CSRF() != "csrf1" {
		t.Fatal("cookies did not persist")
	}
	if err := again.Merge([]*http.Cookie{{Name: "b_3", MaxAge: -1}}); err != nil {
		t.Fatal(err)
	}
	if again.Get("b_3") != "" {
		t.Fatal("expired cookie must be removed")
	}
}

func TestClearRemovesFile(t *testing.T) {
	dir := t.TempDir()
	s, _ := Open(dir)
	s.Merge([]*http.Cookie{{Name: "SESSDATA", Value: "abc"}})
	if err := s.Clear(); err != nil {
		t.Fatal(err)
	}
	if s.LoggedIn() {
		t.Fatal("still logged in")
	}
	if _, err := os.Stat(s.path); !os.IsNotExist(err) {
		t.Fatalf("session file left behind: %v", err)
	}
}

func TestSecretsFileIsOwnerOnly(t *testing.T) {
	if os.PathSeparator != '/' {
		t.Skip("modes are not meaningful on this platform")
	}
	s, _ := Open(t.TempDir())
	s.Merge([]*http.Cookie{{Name: "SESSDATA", Value: "abc"}})
	info, err := os.Stat(s.path)
	if err != nil {
		t.Fatal(err)
	}
	if info.Mode().Perm()&0o077 != 0 {
		t.Fatalf("session file mode %v", info.Mode())
	}
}
