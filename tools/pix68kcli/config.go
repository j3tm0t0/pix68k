package main

import (
	"encoding/json"
	"errors"
	"fmt"
	"io/fs"
	"os"
	"path/filepath"
	"runtime"
	"sort"
	"strings"
)

// 設定ファイル (JSON)。宛先に名前を付けて並べる
//
//	{
//	  "default": "pi3",
//	  "hosts": {
//	    "pi3": {"host": "192.168.1.x", "token_file": "~/.config/pix68kcli/pi3.token"}
//	  }
//	}
type Config struct {
	Default string           `json:"default"`
	Hosts   map[string]Entry `json:"hosts"`
}

type Entry struct {
	Host      string `json:"host"`
	TokenFile string `json:"token_file"`
	Token     string `json:"token"` // 使えるが勧めない (token_file を使う)
}

// 設定ファイルの既定の場所。PIX68KCLI_CONFIG > $XDG_CONFIG_HOME/pix68kcli/config.json > ~/.config/pix68kcli/config.json
// (Windows は %AppData%\pix68kcli\config.json)
func defaultConfigPath(getenv func(string) string) string {
	if p := getenv("PIX68KCLI_CONFIG"); p != "" {
		return p
	}
	if x := getenv("XDG_CONFIG_HOME"); x != "" {
		return filepath.Join(x, "pix68kcli", "config.json")
	}
	if runtime.GOOS == "windows" {
		if a := getenv("AppData"); a != "" {
			return filepath.Join(a, "pix68kcli", "config.json")
		}
	}
	home, err := os.UserHomeDir()
	if err != nil {
		return ""
	}
	return filepath.Join(home, ".config", "pix68kcli", "config.json")
}

// 設定ファイルを読む。explicit が false で、ファイルがなければ空の設定を返す
func loadConfig(path string, explicit bool) (*Config, error) {
	c := &Config{}
	if path == "" {
		return c, nil
	}
	b, err := os.ReadFile(path)
	if err != nil {
		if !explicit && errors.Is(err, fs.ErrNotExist) {
			return c, nil
		}
		return nil, fmt.Errorf("設定ファイルを読めない: %w", err)
	}
	dec := json.NewDecoder(strings.NewReader(string(b)))
	dec.DisallowUnknownFields()
	if err := dec.Decode(c); err != nil {
		return nil, fmt.Errorf("設定ファイル %s を読めない: %v", path, err)
	}
	return c, nil
}

func (c *Config) names() []string {
	var n []string
	for k := range c.Hosts {
		n = append(n, k)
	}
	sort.Strings(n)
	return n
}

// ~/ で始まるパスを広げる
func expandHome(p string) string {
	if p == "~" || strings.HasPrefix(p, "~/") || strings.HasPrefix(p, `~\`) {
		if home, err := os.UserHomeDir(); err == nil {
			return filepath.Join(home, p[1:])
		}
	}
	return p
}

// 宛先と token を決めたもの
type Target struct {
	Name     string // 設定ファイルの名前 (なければ空)
	Host     string // host[:port]
	Token    string
	TokenSrc string // token をどこから取ったか (表示用。値は出さない)
}

type resolveOpts struct {
	host      string // --host
	tokenFile string // --token-file
	getenv    func(string) string
}

// 宛先と token を決める。
//
//	宛先: --host > PIX68K_HOST > 設定ファイルの default。値が設定ファイルの名前なら、その host と token を使う
//	token: --token-file > PIX68K_TOKEN > PIX68K_TOKEN_FILE > 設定ファイルの token_file > token
func resolveTarget(c *Config, o resolveOpts) (*Target, error) {
	h := o.host
	if h == "" {
		h = o.getenv("PIX68K_HOST")
	}
	if h == "" {
		h = c.Default
	}
	if h == "" {
		return nil, usageErr("宛先がない: --host か PIX68K_HOST を指定するか、設定ファイルに default を書く")
	}
	t := &Target{}
	var e *Entry
	if ent, ok := c.Hosts[h]; ok {
		t.Name = h
		e = &ent
		h = ent.Host
		if h == "" {
			return nil, usageErr(fmt.Sprintf("設定ファイルの %q に host がない", t.Name))
		}
	} else {
		// アドレスで指定したとき、同じ host の項目があれば、その token を使う
		for _, n := range c.names() {
			if ent := c.Hosts[n]; normHost(ent.Host) == normHost(h) {
				t.Name = n
				e = &ent
				break
			}
		}
	}
	host, err := normHostChecked(h)
	if err != nil {
		return nil, err
	}
	t.Host = host

	switch {
	case o.tokenFile != "":
		if err := t.readTokenFile(o.tokenFile, "--token-file"); err != nil {
			return nil, err
		}
	case strings.TrimSpace(o.getenv("PIX68K_TOKEN")) != "":
		t.Token, t.TokenSrc = strings.TrimSpace(o.getenv("PIX68K_TOKEN")), "PIX68K_TOKEN"
	case strings.TrimSpace(o.getenv("PIX68K_TOKEN_FILE")) != "":
		if err := t.readTokenFile(o.getenv("PIX68K_TOKEN_FILE"), "PIX68K_TOKEN_FILE"); err != nil {
			return nil, err
		}
	case e != nil && e.TokenFile != "":
		if err := t.readTokenFile(e.TokenFile, "設定ファイルの token_file"); err != nil {
			return nil, err
		}
	case e != nil && strings.TrimSpace(e.Token) != "":
		t.Token, t.TokenSrc = strings.TrimSpace(e.Token), "設定ファイルの token"
	}
	if strings.ContainsAny(t.Token, "\r\n\x00") {
		return nil, usageErr("token に改行か NUL が入っている (" + t.TokenSrc + ")")
	}
	return t, nil
}

func (t *Target) readTokenFile(path, src string) error {
	b, err := os.ReadFile(expandHome(path))
	if err != nil {
		return usageErr(fmt.Sprintf("token のファイルを読めない (%s): %v", src, err))
	}
	t.Token = strings.TrimSpace(string(b))
	t.TokenSrc = src
	if t.Token == "" {
		return usageErr(fmt.Sprintf("token のファイルが空 (%s)", src))
	}
	return nil
}

func normHost(h string) string {
	h = strings.TrimSpace(h)
	h = strings.TrimPrefix(h, "http://")
	h = strings.TrimSuffix(h, "/")
	return strings.TrimSuffix(h, ":80")
}

func normHostChecked(h string) (string, error) {
	if strings.HasPrefix(h, "https://") {
		return "", usageErr("https には対応していない (口は HTTP の 80 番だけ)")
	}
	n := normHost(h)
	if n == "" || strings.ContainsAny(n, "/?# \t") {
		return "", usageErr(fmt.Sprintf("宛先 %q が読めない (アドレスか名前。例: 192.168.1.50、pix68k)", h))
	}
	return n, nil
}
