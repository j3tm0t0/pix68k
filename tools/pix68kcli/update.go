package main

import (
	"archive/zip"
	"bytes"
	"context"
	"crypto/sha256"
	"encoding/hex"
	"encoding/json"
	"errors"
	"flag"
	"fmt"
	"io"
	"net/http"
	"net/url"
	"regexp"
	"strings"
	"time"
)

// 配布しているカーネルの名前 (tools/make_release.sh)。キーは GET /version の raspi=
var releaseKernels = map[string]string{"2": "kernel7.img", "3": "kernel8.img", "4": "kernel8-rpi4.img", "5": "kernel_2712.img"}

const (
	defaultReleaseAPI  = "https://api.github.com/repos/j3tm0t0/pix68k/releases"
	defaultReleaseBase = "https://github.com/j3tm0t0/pix68k/releases/download"
	maxZip             = 64 << 20
	maxKernel          = 16 << 20
	maxText            = 1 << 20
)

var reTag = regexp.MustCompile(`^v[0-9][0-9A-Za-z.+-]*$`)
var reKernelName = regexp.MustCompile(`^[A-Za-z0-9_.-]+$`)
var reHex64 = regexp.MustCompile(`^[0-9a-f]{64}$`)

// 「名前=値」の行を読む
func parseKV(b []byte) map[string]string {
	m := map[string]string{}
	for _, line := range strings.Split(string(b), "\n") {
		if k, v, ok := strings.Cut(strings.TrimRight(line, "\r"), "="); ok {
			m[k] = v
		}
	}
	return m
}

// ---- config.txt ----

// 機種を選ぶ [ ] の見出し (Raspberry Pi の config.txt の文書にあるもの)
var modelFilters = map[string]bool{
	"pi0": true, "pi0w": true, "pi02": true, "pi1": true, "pi2": true, "pi3": true, "pi3+": true, "pi4": true,
	"pi400": true, "pi5": true, "pi500": true, "cm0": true, "cm1": true, "cm3": true, "cm3+": true, "cm4": true, "cm4s": true, "cm5": true,
}

// この機種に当てはまる見出し。知らない機種なら nil
func modelSections(raspi, model string) map[string]bool {
	switch {
	case raspi == "2" && strings.HasPrefix(model, "Raspberry Pi 2 "):
		return map[string]bool{"pi2": true}
	case raspi == "3" && strings.Contains(model, "Pi 3 Model B+"):
		return map[string]bool{"pi3": true, "pi3+": true}
	case raspi == "3" && strings.Contains(model, "Pi 3 Model B"):
		return map[string]bool{"pi3": true}
	case raspi == "4" && strings.Contains(model, "Pi 400"):
		return map[string]bool{"pi4": true, "pi400": true}
	case raspi == "4" && strings.Contains(model, "Pi 4 Model B"):
		return map[string]bool{"pi4": true}
	case raspi == "5" && strings.Contains(model, "Pi 5") && !strings.Contains(model, "Pi 500"):
		return map[string]bool{"pi5": true}
	}
	return nil
}

// config.txt で、この機種が使う kernel= の値と、その値の範囲 (バイト)。判断できない書き方なら誤りにする
func findKernel(config string, sections map[string]bool) (name string, begin, end int, err error) {
	active, known, none := true, true, false
	found := false
	for pos := 0; pos < len(config); {
		next := len(config)
		if i := strings.IndexByte(config[pos:], '\n'); i >= 0 {
			next = pos + i + 1
		}
		raw := strings.TrimRight(config[pos:next], "\r\n")
		line := strings.TrimSpace(raw)
		start := pos
		pos = next
		if strings.HasPrefix(line, "[") {
			f := strings.TrimSuffix(strings.TrimPrefix(line, "["), "]")
			switch {
			case f == "all" && strings.HasSuffix(line, "]"):
				active, known, none = true, true, false
			case f == "none" && strings.HasSuffix(line, "]"):
				active, none = false, true
			case modelFilters[f] && strings.HasSuffix(line, "]"):
				active = sections[f] // [none] のあとなら、使われるか分からない
				known = known && !none
			default:
				known = false // ほかの条件 ([gpio4=1]、[tryboot] など)。[all] まで当てはまるか分からない
			}
			continue
		}
		key, value, hasEq := strings.Cut(line, "=")
		word := strings.TrimSpace(key)
		if f := strings.Fields(word); len(f) > 1 && (f[0] == "kernel" || f[0] == "include" || f[0] == "os_prefix") {
			word = f[0]
		}
		if word != "kernel" && word != "include" && word != "os_prefix" {
			continue
		}
		if !active {
			continue
		}
		if !known {
			return "", 0, 0, errors.New("config.txt: 条件付きの見出しの中に " + word + " があり、使われるか判断できない")
		}
		if word != "kernel" {
			return "", 0, 0, errors.New("config.txt: " + word + " があるので、使うカーネルを判断できない")
		}
		if !hasEq || line != raw || key != "kernel" || !reKernelName.MatchString(value) {
			return "", 0, 0, fmt.Errorf("config.txt: kernel= の行を読めない: %q", line)
		}
		found = true
		name, begin, end = value, start+len("kernel="), start+len("kernel=")+len(value)
	}
	if !found {
		return "", 0, 0, errors.New("config.txt にこの機種の kernel= がない")
	}
	return name, begin, end, nil
}

// kernel= の値が 2 つのセクタ (512 バイト) にまたがると、Pi は書き換えを断る。カーネルを書く前に調べる
func sectorCheck(config string, sections map[string]bool) error {
	_, b, e, err := findKernel(config, sections)
	if err == nil && b/512 != (e-1)/512 {
		err = fmt.Errorf("config.txt の kernel= の値が、ファイルの先頭から %d バイト目のセクタの境目にまたがるので、書き換えられない。\n"+
			"その行より前の注釈 (#) を %d バイト (半角の文字) 増やすと、書き換えられる。何も書いていない", (e-1)/512*512, (e-1)/512*512-b)
	}
	return err
}

// config.txt の、この機種が使う kernel= の値だけを替える。ほかのバイトは変えない
func setKernel(config string, sections map[string]bool, name string) (string, error) {
	if !reKernelName.MatchString(name) {
		return "", fmt.Errorf("カーネルの名前が正しくない: %q", name)
	}
	_, b, e, err := findKernel(config, sections)
	if err != nil {
		return "", err
	}
	return config[:b] + name + config[e:], nil
}

// ---- 配布しているもの ----

type releaseSource struct {
	api, base string
	hc        *http.Client
}

func (s *releaseSource) get(ctx context.Context, u string, max int64) ([]byte, error) {
	req, err := http.NewRequestWithContext(ctx, "GET", u, nil)
	if err != nil {
		return nil, err
	}
	req.Header.Set("User-Agent", "pix68kcli")
	resp, err := s.hc.Do(req)
	if err != nil {
		return nil, &cliError{exitConnect, "配布元につながらない: " + err.Error()}
	}
	defer resp.Body.Close()
	// 403 と 429 は GitHub の回数の制限のことが多い。Pi の token の誤り (終了コード 4) とは分ける
	if resp.StatusCode == 403 || resp.StatusCode == 429 {
		msg := fmt.Sprintf("配布元: GET %s: %s (GitHub の回数の制限の可能性がある。時間をおいて試す", u, resp.Status)
		if strings.HasPrefix(u, s.api+"/") {
			msg += "か、--tag v0.3.0 のように版を指定する (API を使わない)"
		}
		return nil, &cliError{exitError, msg + ")"}
	}
	if resp.StatusCode != 200 {
		code := exitError // Pi の状態を表す終了コード (4、6) は使わない
		if resp.StatusCode == 404 {
			code = exitNotFound
		}
		return nil, &cliError{code, fmt.Sprintf("配布元: GET %s: %s", u, resp.Status)}
	}
	b, err := io.ReadAll(io.LimitReader(resp.Body, max+1))
	if err != nil {
		return nil, &cliError{exitConnect, "配布元: GET " + u + ": " + err.Error()}
	}
	if int64(len(b)) > max {
		return nil, fmt.Errorf("配布元: GET %s: 大きすぎる", u)
	}
	return b, nil
}

// 配布元の転送先は https だけ (GitHub は別のホストに転送する)
func httpsOnly(req *http.Request, via []*http.Request) error {
	if req.URL.Scheme != "https" {
		return errors.New("https でない転送先: " + req.URL.Scheme + "://" + req.URL.Host)
	}
	if len(via) >= 10 {
		return errors.New("転送が多すぎる")
	}
	return nil
}

func isNotFound(err error) bool {
	var ce *cliError
	return errors.As(err, &ce) && ce.code == exitNotFound
}

// tag が空なら最新の正式な版
func (s *releaseSource) resolve(ctx context.Context, tag string) (string, error) {
	if tag == "" {
		b, err := s.get(ctx, s.api+"/latest", maxText)
		if err != nil {
			return "", err
		}
		var r struct {
			Tag string `json:"tag_name"`
		}
		if err := json.Unmarshal(b, &r); err != nil {
			return "", errors.New("配布元の応答を読めない: " + err.Error())
		}
		tag = r.Tag
	}
	if !reTag.MatchString(tag) {
		return "", fmt.Errorf("版の名前が正しくない: %q", tag)
	}
	return tag, nil
}

// SHA256SUMS から name の値を取り出す
func sumFor(sums []byte, name string) (string, error) {
	got := ""
	for _, line := range strings.Split(string(sums), "\n") {
		f := strings.Fields(line)
		if len(f) == 2 && strings.TrimPrefix(f[1], "*") == name {
			if got != "" || !reHex64.MatchString(f[0]) {
				return "", errors.New("SHA256SUMS の " + name + " の行が正しくない")
			}
			got = f[0]
		}
	}
	if got == "" {
		return "", errors.New("SHA256SUMS に " + name + " がない")
	}
	return got, nil
}

func sha256Hex(b []byte) string {
	h := sha256.Sum256(b)
	return hex.EncodeToString(h[:])
}

// 版 tag の zip を取り、SHA256SUMS と照合して、カーネル kernel を返す
func (s *releaseSource) kernel(ctx context.Context, tag, kernel string) ([]byte, error) {
	dir := s.base + "/" + url.PathEscape(tag) + "/"
	sums, err := s.get(ctx, dir+"SHA256SUMS", maxText)
	if err != nil {
		return nil, err
	}
	name := "pix68k-" + tag + ".zip"
	want, err := sumFor(sums, name)
	if err != nil {
		return nil, err
	}
	b, err := s.get(ctx, dir+name, maxZip)
	if err != nil {
		return nil, err
	}
	if got := sha256Hex(b); got != want {
		return nil, fmt.Errorf("%s の SHA-256 が SHA256SUMS と違う (%s、SHA256SUMS は %s)", name, got, want)
	}
	z, err := zip.NewReader(bytes.NewReader(b), int64(len(b)))
	if err != nil {
		return nil, fmt.Errorf("%s: %w", name, err)
	}
	var out []byte
	for _, f := range z.File {
		if f.Name != kernel {
			continue
		}
		if out != nil {
			return nil, fmt.Errorf("%s に %s が 2 つある", name, kernel)
		}
		r, err := f.Open()
		if err != nil {
			return nil, fmt.Errorf("%s: %s: %w", name, kernel, err)
		}
		out, err = io.ReadAll(io.LimitReader(r, maxKernel+1))
		r.Close()
		if err != nil {
			return nil, fmt.Errorf("%s: %s: %w", name, kernel, err)
		}
		if len(out) > maxKernel || len(out) == 0 {
			return nil, fmt.Errorf("%s: %s の大きさが正しくない", name, kernel)
		}
	}
	if out == nil {
		return nil, fmt.Errorf("%s に %s がない", name, kernel)
	}
	return out, nil
}

// ---- update ----

func (a *app) cmdUpdate(ctx context.Context, c *Client, args []string) error {
	fs := flag.NewFlagSet("update", flag.ContinueOnError)
	var o updateOpts
	check := fs.Bool("check", false, "")
	tag := fs.String("tag", "", "")
	rollback := fs.Bool("rollback", false, "")
	fs.BoolVar(&o.noTryboot, "no-tryboot", false, "")
	fs.BoolVar(&o.allowDowngrade, "allow-downgrade", false, "")
	fs.BoolVar(&o.force, "force", false, "")
	if err := parseFlags(fs, args); err != nil {
		return err
	}
	if err := noArgs(fs); err != nil {
		return err
	}
	if *rollback {
		if *check || *tag != "" || o.allowDowngrade || o.force || o.noTryboot {
			return usageErr("update: --rollback はほかのオプションと一緒に使えない")
		}
		return a.rollback(ctx, c)
	}
	if !*check && c.target.Token == "" {
		return usageErr("update: token が要る")
	}
	if *tag != "" && !reTag.MatchString(*tag) {
		return usageErr(fmt.Sprintf("update: 版の名前が正しくない: %q", *tag))
	}
	w := a.stdout
	b, err := c.fetch(ctx, reqSpec{method: "GET", escapedPath: "/version"})
	if err != nil {
		return unsupportedOn404(err, "GET /version")
	}
	v := parseKV(b)
	kernel := releaseKernels[v["raspi"]]
	sections := modelSections(v["raspi"], v["model"])
	if kernel == "" || sections == nil {
		return fmt.Errorf("知らない機種: raspi=%s model=%s", v["raspi"], v["model"])
	}
	fmt.Fprintf(w, "今の版: %s (コミット %s)\n", v["version"], v["commit"])
	fmt.Fprintf(w, "機種: %s\n", v["model"])
	var reasons []string
	switch {
	case v["drive"] == "none" || v["drive"] == "":
		return &cliError{exitBusy, "Pi がファイルを読めるドライブがない"}
	case strings.HasPrefix(v["drive"], "USB"):
		reasons = append(reasons, "USB メモリのドライブには /pix68k/ の外を書けない")
	case v["drive"] != "SD:":
		reasons = append(reasons, "SD カードのほかのドライブ ("+v["drive"]+") は更新できない")
	}
	// Pi のドライブの config.txt と、そこに書いてあるカーネル
	cfg, err := c.fetch(ctx, reqSpec{method: "GET", escapedPath: "/files/config.txt", auth: true})
	name := ""
	switch {
	case isNotFound(err):
		reasons = append(reasons, "ドライブに config.txt がない (LAN から起動しているときなど)")
	case err != nil:
		return err
	default:
		if name, _, _, err = findKernel(string(cfg), sections); err != nil {
			reasons = append(reasons, err.Error())
		}
	}
	have := ""
	if name != "" {
		// 先に /sha256 を見る (大きすぎるファイルはここで断られる)
		s, err := c.fetch(ctx, reqSpec{method: "GET", escapedPath: "/sha256/" + url.PathEscape(name), auth: true, timeout: time.Minute})
		if isNotFound(err) {
			// /version のあるカーネルには /sha256 もあるので、404 はファイルがないこと
			reasons = append(reasons, "ドライブに config.txt の kernel= の "+name+" がない (LAN から起動しているときなど)")
		} else if err != nil {
			return err
		} else {
			have = parseKV(s)["sha256"]
			if !reHex64.MatchString(have) {
				return fmt.Errorf("/sha256 の応答を読めない: %q", have)
			}
		}
	}
	if have != "" {
		sd, err := c.fetch(ctx, reqSpec{method: "GET", escapedPath: filesPath(name), auth: true, timeout: 3 * time.Minute})
		if err != nil {
			return err
		}
		if have != sha256Hex(sd) {
			return fmt.Errorf("%s: /sha256 の値 (%s) と、読んだファイルの SHA-256 (%s) が違う", name, have, sha256Hex(sd))
		}
		fmt.Fprintf(w, "ドライブのカーネル: %s%s (config.txt の kernel=)\n", v["drive"], "/"+name)
		// 起動時のログの文字列 (baremetal/kernel.cpp)
		if !bytes.Contains(sd, []byte("version "+v["version"]+", commit "+v["commit"])) {
			reasons = append(reasons, "動いているカーネルとドライブのカーネルの版が違う (LAN から起動しているか、入れ替えてから再起動していない)")
		}
	}
	src := a.releases
	if src == nil {
		src = &releaseSource{defaultReleaseAPI, defaultReleaseBase, &http.Client{Timeout: 10 * time.Minute, CheckRedirect: httpsOnly}}
	}
	t, err := src.resolve(ctx, *tag)
	if err != nil {
		return err
	}
	k, err := src.kernel(ctx, t, kernel)
	if err != nil {
		return err
	}
	fmt.Fprintf(w, "配布している版: %s (zip の SHA-256 は SHA256SUMS と同じ。%s は %d バイト)\n", t, kernel, len(k))
	for _, r := range reasons {
		fmt.Fprintf(w, "更新できない: %s\n", r)
	}
	switch {
	case have == "":
		fmt.Fprintln(w, "結果: ドライブのカーネルがなく、比べられない")
	case have == sha256Hex(k):
		fmt.Fprintln(w, "結果: 配布している版と同じカーネル")
	default:
		fmt.Fprintf(w, "結果: %s のカーネルと違う (空き容量は調べていない)\n", t)
	}
	order := compareVersions(v["version"], t)
	if have != sha256Hex(k) {
		switch {
		case order == orderNewer:
			fmt.Fprintf(w, "版: %s は今の %s より新しい\n", t, v["version"])
		case order == orderOlder:
			fmt.Fprintf(w, "版: %s は今の %s より古い\n", t, v["version"])
		default:
			fmt.Fprintf(w, "版: 今の %s と %s のどちらが新しいか分からない\n", v["version"], t)
		}
	}
	if *check || have == sha256Hex(k) && len(reasons) == 0 {
		return nil
	}
	if len(reasons) > 0 {
		return &cliError{exitError, "更新できない (上の理由)。何も書いていない"}
	}
	return a.applyUpdate(ctx, c, o, v, sections, string(cfg), name, have, t, k, order)
}
