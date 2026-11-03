// Package authstore keeps the Bilibili account cookies on the PC. The PSP never
// receives them: the proxy attaches them to upstream requests.
package authstore

import (
	"encoding/json"
	"errors"
	"net/http"
	"os"
	"path/filepath"
	"sort"
	"strings"
	"sync"
	"time"

	"github.com/wilimini/wilimini/server/internal/config"
)

const fileName = "session.json"

type state struct {
	Cookies map[string]string `json:"cookies"`
	Mid     int64             `json:"mid,omitempty"`
	Uname   string            `json:"uname,omitempty"`
	Saved   time.Time         `json:"saved"`
}

type Store struct {
	mu   sync.RWMutex
	path string
	st   state
}

// Open loads dir/session.json when present.
func Open(dir string) (*Store, error) {
	s := &Store{path: filepath.Join(dir, fileName)}
	s.st.Cookies = map[string]string{}
	data, err := os.ReadFile(s.path)
	if errors.Is(err, os.ErrNotExist) {
		return s, nil
	}
	if err != nil {
		return nil, err
	}
	if err := json.Unmarshal(data, &s.st); err != nil {
		return nil, err
	}
	if s.st.Cookies == nil {
		s.st.Cookies = map[string]string{}
	}
	return s, nil
}

// Header returns the Cookie request header value, sorted for stable output.
func (s *Store) Header() string {
	s.mu.RLock()
	defer s.mu.RUnlock()
	names := make([]string, 0, len(s.st.Cookies))
	for name := range s.st.Cookies {
		names = append(names, name)
	}
	sort.Strings(names)
	parts := make([]string, 0, len(names))
	for _, name := range names {
		parts = append(parts, name+"="+s.st.Cookies[name])
	}
	return strings.Join(parts, "; ")
}

func (s *Store) Get(name string) string {
	s.mu.RLock()
	defer s.mu.RUnlock()
	return s.st.Cookies[name]
}

// LoggedIn reports whether a session cookie is present. The account may still
// have expired upstream; Nav confirms.
func (s *Store) LoggedIn() bool { return s.Get("SESSDATA") != "" }

// CSRF returns the bili_jct token used for write requests.
func (s *Store) CSRF() string { return s.Get("bili_jct") }

func (s *Store) Identity() (int64, string) {
	s.mu.RLock()
	defer s.mu.RUnlock()
	return s.st.Mid, s.st.Uname
}

// SetIdentity records who the cookies belong to, for display only.
func (s *Store) SetIdentity(mid int64, name string) error {
	s.mu.Lock()
	defer s.mu.Unlock()
	if s.st.Mid == mid && s.st.Uname == name {
		return nil
	}
	s.st.Mid, s.st.Uname = mid, name
	return s.saveLocked()
}

// Merge stores cookies from Set-Cookie headers. Expired or empty ones delete.
func (s *Store) Merge(cookies []*http.Cookie) error {
	s.mu.Lock()
	defer s.mu.Unlock()
	changed := false
	for _, c := range cookies {
		if c.Name == "" {
			continue
		}
		if c.Value == "" || c.MaxAge < 0 {
			if _, ok := s.st.Cookies[c.Name]; ok {
				delete(s.st.Cookies, c.Name)
				changed = true
			}
			continue
		}
		if s.st.Cookies[c.Name] != c.Value {
			s.st.Cookies[c.Name] = c.Value
			changed = true
		}
	}
	if !changed {
		return nil
	}
	return s.saveLocked()
}

// Clear forgets the account (logout).
func (s *Store) Clear() error {
	s.mu.Lock()
	defer s.mu.Unlock()
	s.st = state{Cookies: map[string]string{}}
	if err := os.Remove(s.path); err != nil && !errors.Is(err, os.ErrNotExist) {
		return err
	}
	return nil
}

func (s *Store) saveLocked() error {
	if err := os.MkdirAll(filepath.Dir(s.path), 0o700); err != nil {
		return err
	}
	s.st.Saved = time.Now().UTC()
	data, err := json.MarshalIndent(&s.st, "", "  ")
	if err != nil {
		return err
	}
	return config.WriteFileAtomic(s.path, data)
}
