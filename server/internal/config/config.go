// Package config holds the server settings that survive restarts: the pairing
// token, the display name and the listen port. Secrets are stored with 0600.
package config

import (
	"crypto/rand"
	"encoding/json"
	"errors"
	"fmt"
	"math/big"
	"os"
	"path/filepath"
	"strings"
)

const (
	DefaultPort       = 8787
	DefaultMaxStreams = 2
	fileName          = "config.json"
	// No 0/o/1/l/i: the token is typed with a game pad.
	tokenAlphabet = "abcdefghjkmnpqrstuvwxyz23456789"
	tokenLength   = 8
)

type Config struct {
	Name       string `json:"name"`
	Token      string `json:"token"`
	Port       int    `json:"port"`
	MaxStreams int    `json:"max_streams"`
	ReadOnly   bool   `json:"read_only"`
	FFmpeg     string `json:"ffmpeg,omitempty"`
}

// Dir returns the directory for config and session files. WILI_SERVER_DIR
// overrides the per-user default.
func Dir() (string, error) {
	if dir := os.Getenv("WILI_SERVER_DIR"); dir != "" {
		return dir, nil
	}
	base, err := os.UserConfigDir()
	if err != nil {
		return "", fmt.Errorf("no per-user config directory: %w", err)
	}
	return filepath.Join(base, "wili-server"), nil
}

// NewToken makes a random pairing token from an unambiguous alphabet.
func NewToken() string {
	var out strings.Builder
	max := big.NewInt(int64(len(tokenAlphabet)))
	for i := 0; i < tokenLength; i++ {
		n, err := rand.Int(rand.Reader, max)
		if err != nil {
			panic("wili-server: no system randomness: " + err.Error())
		}
		out.WriteByte(tokenAlphabet[n.Int64()])
	}
	return out.String()
}

func defaults() Config {
	name, _ := os.Hostname()
	if name == "" {
		name = "WiliMini"
	}
	return Config{Name: name, Token: NewToken(), Port: DefaultPort, MaxStreams: DefaultMaxStreams}
}

// Load reads dir/config.json. A missing file yields fresh defaults which are
// written immediately so the token is stable across runs.
func Load(dir string) (*Config, error) {
	path := filepath.Join(dir, fileName)
	data, err := os.ReadFile(path)
	if errors.Is(err, os.ErrNotExist) {
		cfg := defaults()
		return &cfg, cfg.Save(dir)
	}
	if err != nil {
		return nil, err
	}
	cfg := defaults()
	if err := json.Unmarshal(data, &cfg); err != nil {
		return nil, fmt.Errorf("%s: %w", path, err)
	}
	cfg.normalize()
	return &cfg, nil
}

func (c *Config) normalize() {
	if c.Token == "" {
		c.Token = NewToken()
	}
	if c.Port <= 0 || c.Port > 65535 {
		c.Port = DefaultPort
	}
	if c.MaxStreams <= 0 {
		c.MaxStreams = DefaultMaxStreams
	}
	c.Name = sanitizeName(c.Name)
}

// sanitizeName keeps the name safe for the discovery wire formats.
func sanitizeName(name string) string {
	name = strings.Map(func(r rune) rune {
		if r < 0x20 || r == 0x7f {
			return -1
		}
		return r
	}, name)
	name = strings.TrimSpace(name)
	if len(name) > 48 {
		name = strings.TrimSpace(name[:48])
	}
	if name == "" {
		name = "WiliMini"
	}
	return name
}

// Save writes the file atomically with owner-only permissions.
func (c *Config) Save(dir string) error {
	c.normalize()
	if err := os.MkdirAll(dir, 0o700); err != nil {
		return err
	}
	data, err := json.MarshalIndent(c, "", "  ")
	if err != nil {
		return err
	}
	return WriteFileAtomic(filepath.Join(dir, fileName), data)
}

// WriteFileAtomic replaces path with data (mode 0600) via a temporary file.
func WriteFileAtomic(path string, data []byte) error {
	tmp, err := os.CreateTemp(filepath.Dir(path), filepath.Base(path)+".*.tmp")
	if err != nil {
		return err
	}
	name := tmp.Name()
	defer os.Remove(name)
	if err := tmp.Chmod(0o600); err != nil && !errors.Is(err, os.ErrPermission) {
		// Windows ignores modes; other failures are real.
		if !strings.Contains(err.Error(), "not supported") {
			tmp.Close()
			return err
		}
	}
	if _, err := tmp.Write(data); err != nil {
		tmp.Close()
		return err
	}
	if err := tmp.Sync(); err != nil {
		tmp.Close()
		return err
	}
	if err := tmp.Close(); err != nil {
		return err
	}
	return os.Rename(name, path)
}
