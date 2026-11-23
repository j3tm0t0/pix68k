package main

import (
	"bytes"
	"context"
	"errors"
	"fmt"
	"net/url"
	"os"
	"path/filepath"
	"regexp"
	"strconv"
	"strings"
	"time"
)

type updateOpts struct {
	noTryboot, allowDowngrade, force bool
}

// tryboot (1 回だけ新しいカーネルで起動する) を実機で確かめた機種 (GET /version の model=)。
// 確かめたものはまだなく、tryboot を使う処理も入っていない。足すときは、処理と一緒に足す
var trybootVerified = map[string]bool{}

const updateTxt = "/pix68k/update.txt"

// 機種ごとの 2 つの名前。長さが同じなので、config.txt の大きさが変わらない (src/platform/config_kernel.h と同じ)
var kernelPairs = map[string][2]string{
	"2": {"kernel7.img", "pix68k7.img"}, "3": {"kernel8.img", "pix68k8.img"},
	"4": {"kernel8-rpi4.img", "pix68k8-rpi4.img"}, "5": {"kernel_2712.img", "pix68k_2712.img"},
}

func otherKernel(raspi, name string) string {
	p := kernelPairs[raspi]
	switch name {
	case "":
		return ""
	case p[0]:
		return p[1]
	case p[1]:
		return p[0]
	}
	return ""
}

const (
	orderUnknown = iota
	orderSame
	orderNewer
	orderOlder
)

var reVersion = regexp.MustCompile(`^v([0-9]{1,6})\.([0-9]{1,6})\.([0-9]{1,6})(?:-beta\.([0-9]{1,6}))?$`)

// to が from より新しいか。vX.Y.Z か vX.Y.Z-beta.N でなければ分からない
func compareVersions(from, to string) int {
	a, b := reVersion.FindStringSubmatch(from), reVersion.FindStringSubmatch(to)
	if a == nil || b == nil {
		return orderUnknown
	}
	for i := 1; i <= 4; i++ {
		// beta のない版は、同じ番号のどの beta よりも新しい
		x, y := 1<<30, 1<<30
		if a[i] != "" {
			x, _ = strconv.Atoi(a[i])
		}
		if b[i] != "" {
			y, _ = strconv.Atoi(b[i])
		}
		if y > x {
			return orderNewer
		}
		if y < x {
			return orderOlder
		}
	}
	return orderSame
}

var reUnsafeName = regexp.MustCompile(`[^A-Za-z0-9_.-]+`)

var reKernelVersion = regexp.MustCompile(`version ([A-Za-z0-9_.+-]{1,64}), commit ([0-9a-z]{3,40})`)

func (a *app) backupConfig(c *Client, cfg string) (string, error) {
	dir := a.backupDir
	name := fmt.Sprintf("pix68k-config-%s-%s.txt", reUnsafeName.ReplaceAllString(c.target.Host, "_"), time.Now().Format("20060102-150405.000"))
	p, err := filepath.Abs(filepath.Join(dir, name))
	if err != nil {
		return "", err
	}
	f, err := os.OpenFile(p, os.O_WRONLY|os.O_CREATE|os.O_EXCL, 0o600)
	if err != nil {
		return "", fmt.Errorf("config.txt の控えを書けない: %w", err)
	}
	_, err = f.WriteString(cfg)
	if err2 := f.Sync(); err == nil {
		err = err2
	}
	if err2 := f.Close(); err == nil {
		err = err2
	}
	if err != nil {
		return "", fmt.Errorf("config.txt の控えを書けない: %w", err)
	}
	return p, nil
}

func (c *Client) putFile(ctx context.Context, path string, data []byte, query url.Values) error {
	_, err := c.fetch(ctx, reqSpec{method: "PUT", escapedPath: filesPath(path), query: query, auth: true, body: bytes.NewReader(data),
		length: int64(len(data)), expect: true, noTimeout: true})
	return err
}

func (c *Client) fileSHA256(ctx context.Context, name string) (string, error) {
	b, err := c.fetch(ctx, reqSpec{method: "GET", escapedPath: "/sha256/" + url.PathEscape(name), auth: true, timeout: time.Minute})
	if err != nil {
		return "", err
	}
	s := parseKV(b)["sha256"]
	if !reHex64.MatchString(s) {
		return "", errors.New("/sha256 の応答を読めない")
	}
	return s, nil
}

func (c *Client) switchKernel(ctx context.Context, from, to, sum string) error {
	_, err := c.fetch(ctx, reqSpec{method: "POST", escapedPath: "/config/kernel",
		query: url.Values{"from": {from}, "to": {to}, "sha256": {sum}}, auth: true, body: bytes.NewReader(nil), timeout: 2 * time.Minute})
	return err
}

// config.txt を読み直し、want と 1 バイトも違わないことを確かめる
func (c *Client) checkConfig(ctx context.Context, want string) error {
	b, err := c.fetch(ctx, reqSpec{method: "GET", escapedPath: "/files/config.txt", auth: true})
	if err != nil {
		return err
	}
	if string(b) != want {
		return errors.New("config.txt が、思った中身になっていない")
	}
	return nil
}

func (a *app) applyUpdate(ctx context.Context, c *Client, o updateOpts, v map[string]string, sections map[string]bool,
	cfg, name, have, tag string, kernel []byte, order int) error {
	w := a.stdout
	to := otherKernel(v["raspi"], name)
	if to == "" {
		return fmt.Errorf("更新できない: config.txt の kernel=%s は、この機種で入れ替える名前 (%s、%s) ではない。何も書いていない",
			name, kernelPairs[v["raspi"]][0], kernelPairs[v["raspi"]][1])
	}
	switch order {
	case orderOlder:
		if !o.allowDowngrade {
			return errors.New("古い版になるので書かない。それでもよければ --allow-downgrade を付ける")
		}
	case orderNewer:
	default:
		if !o.force {
			return errors.New("新しくなるか分からないので書かない。それでもよければ --force を付ける")
		}
	}
	if trybootVerified[v["model"]] && !o.noTryboot {
		return errors.New("tryboot を使う更新はまだない。--no-tryboot を付ける") // 表に足すときに、ここを処理に替える
	}
	if !o.noTryboot {
		return errors.New("この機種では、新しい版を 1 回だけ試して起動する方法 (tryboot) を確かめていないので、書かない。\n" +
			"新しい版が起動しないと、SD カードを PC に入れて config.txt の kernel= を戻すことになる。それでもよければ --no-tryboot を付ける")
	}
	ok, err := c.hasEndpoint(ctx, "/config/kernel")
	if err != nil {
		return err
	}
	if !ok {
		return &cliError{exitUnsupported, c.target.Host + ": 動いているカーネルに POST /config/kernel がない。新しいカーネルに手で入れ替える"}
	}
	// 起動時のログの文字列 (baremetal/kernel.cpp)。v0.2.3 以前のカーネルにはない
	commit := ""
	if m := reKernelVersion.FindSubmatch(kernel); m != nil {
		if string(m[1]) != tag {
			return fmt.Errorf("配布しているカーネルの版が %s で、%s ではない。何も書いていない", m[1], tag)
		}
		commit = string(m[2])
	} else if order == orderNewer {
		return fmt.Errorf("配布しているカーネルの中に %s の版の文字列がない。何も書いていない", tag)
	}
	if tag == v["version"] && commit == v["commit"] {
		return errors.New("中身は違うが、版とコミットが今と同じなので、入れ替わったかを確かめられない。何も書いていない")
	}
	if !bytes.Contains(kernel, []byte("/config/kernel")) {
		fmt.Fprintf(w, "注意: %s には POST /config/kernel がない。入れたあとは、この命令で戻せない (SD カードを PC で書き換える)\n", tag)
	}
	if err := sectorCheck(cfg, sections); err != nil {
		return err
	}
	want, err := setKernel(cfg, sections, to)
	if err != nil {
		return err
	}
	backup, err := a.backupConfig(c, cfg)
	if err != nil {
		return err
	}
	fmt.Fprintf(w, "config.txt の控え: %s\n", backup)
	sum := sha256Hex(kernel)
	// unused=1: ほかの更新と重なり、to が起動に使うカーネルになっていれば、Pi が断る
	fmt.Fprintf(w, "%s を書く (%d バイト)\n", to, len(kernel))
	if err := c.putFile(ctx, "/"+to, kernel, url.Values{"unused": {"1"}}); err != nil {
		return notSwitched(err, name)
	}
	got, err := c.fileSHA256(ctx, to)
	if err == nil && got != sum {
		err = fmt.Errorf("%s: 書いたものの SHA-256 が違う (%s、正しくは %s)", to, got, sum)
	}
	if err != nil {
		return notSwitched(err, name)
	}
	info := fmt.Sprintf("previous=%s\nprevious_version=%s\nprevious_commit=%s\nprevious_sha256=%s\ncurrent=%s\ncurrent_version=%s\ncurrent_sha256=%s\n",
		name, v["version"], v["commit"], have, to, tag, sum)
	if err := c.putFile(ctx, updateTxt, []byte(info), nil); err != nil {
		return notSwitched(err, name)
	}
	// 書いている間に config.txt を変えられていないか
	if err := c.checkConfig(ctx, cfg); err != nil {
		return notSwitched(err, name)
	}
	if err := c.switchKernel(ctx, name, to, sum); err != nil {
		return switchFailed(err, name, backup)
	}
	if err := c.checkConfig(ctx, want); err != nil {
		return maybeSwitched(err, backup)
	}
	fmt.Fprintf(w, "config.txt: kernel=%s を kernel=%s に替えた (%s は残してある)\n", name, to, name)
	if err := a.rebootAndWait(ctx, c, tag, commit); err != nil {
		return fmt.Errorf("%w\n起動しないときは、SD カードを PC に入れ、config.txt を控え (%s) に戻す", err, backup)
	}
	return nil
}

func notSwitched(err error, name string) error {
	return &cliError{exitCode(err), err.Error() + "\nこの更新では config.txt を替えていない (ほかの更新が重なっていなければ、次の起動も " + name + ")"}
}

// POST /config/kernel の失敗。書く前に断られたと分かる応答 (4xx、503、「not written」の 500) のほかは、書いたか分からない
func switchFailed(err error, name, backup string) error {
	var r *replyError
	if errors.As(err, &r) && (r.status >= 400 && r.status < 500 || r.status == 503 ||
		r.status == 500 && strings.Contains(r.body, "; not written")) {
		return notSwitched(err, name)
	}
	return maybeSwitched(err, backup)
}

func maybeSwitched(err error, backup string) error {
	return &cliError{exitCode(err), err.Error() + "\nconfig.txt が替わったか、壊れたかもしれない。再起動も電源を切ることもせず、" +
		"pix68kcli files get /config.txt - で中身を確かめる。控えは " + backup}
}

// 再起動し、GET /version の版とコミットが want と commit になるのを待つ。commit が空なら、/version のないカーネルなので待たない
func (a *app) rebootAndWait(ctx context.Context, c *Client, want, commit string) error {
	if _, err := c.fetch(ctx, reqSpec{method: "POST", escapedPath: "/reboot", auth: true, body: bytes.NewReader(nil)}); err != nil {
		return fmt.Errorf("再起動を頼めない: %w", err)
	}
	if commit == "" {
		return &cliError{exitConnect, fmt.Sprintf("再起動を頼んだ。%s には GET /version がないので、動いたかは確かめられない (画面で確かめる)", want)}
	}
	fmt.Fprintln(a.stdout, "再起動を頼んだ。動くのを待つ")
	wait, interval := a.updateWait, a.updateInterval
	if wait == 0 {
		wait, interval = 3*time.Minute, 2*time.Second
	}
	end := time.Now().Add(wait)
	last := "つながらない"
	for time.Now().Before(end) {
		select {
		case <-ctx.Done():
			return &cliError{exitConnect, "取り消した (Pi の再起動は続く)"}
		case <-time.After(interval):
		}
		b, err := c.fetch(ctx, reqSpec{method: "GET", escapedPath: "/version", timeout: 5 * time.Second})
		if err != nil {
			last = err.Error()
			continue
		}
		v := parseKV(b)
		if v["version"] == want && v["commit"] == commit {
			fmt.Fprintf(a.stdout, "%s が動いている\n", want)
			return nil
		}
		last = "動いている版は " + v["version"]
	}
	return &cliError{exitConnect, fmt.Sprintf("%s が動いていることを確かめられない (%s)", want, last)}
}

// 前の版に戻す (update.txt に書いた、もう一方の名前のカーネルへ)
func (a *app) rollback(ctx context.Context, c *Client) error {
	if c.target.Token == "" {
		return usageErr("update: token が要る")
	}
	b, err := c.fetch(ctx, reqSpec{method: "GET", escapedPath: "/version"})
	if err != nil {
		return unsupportedOn404(err, "GET /version")
	}
	v := parseKV(b)
	sections := modelSections(v["raspi"], v["model"])
	if sections == nil {
		return fmt.Errorf("知らない機種: raspi=%s model=%s", v["raspi"], v["model"])
	}
	b, err = c.fetch(ctx, reqSpec{method: "GET", escapedPath: "/files/config.txt", auth: true})
	if err != nil {
		return err
	}
	cfg := string(b)
	name, _, _, err := findKernel(cfg, sections)
	if err != nil {
		return err
	}
	if ok, err := c.hasEndpoint(ctx, "/config/kernel"); err != nil {
		return err
	} else if !ok {
		return &cliError{exitUnsupported, c.target.Host + ": 動いているカーネルに POST /config/kernel がない。SD カードを PC に入れて kernel= を戻す"}
	}
	if v["drive"] != "SD:" {
		return errors.New("戻せるのは、SD カードから起動しているときだけ")
	}
	b, err = c.fetch(ctx, reqSpec{method: "GET", escapedPath: filesPath(updateTxt), auth: true})
	if err != nil {
		if isNotFound(err) {
			return &cliError{exitNotFound, "戻せる版がない (" + updateTxt + " がない)"}
		}
		return err
	}
	u := parseKV(b)
	to := otherKernel(v["raspi"], name)
	if to == "" || u["previous"] != to || u["current"] != name {
		return fmt.Errorf("戻せる版がない (config.txt の kernel=%s、%s の previous=%s)", name, updateTxt, u["previous"])
	}
	// 更新した版か、更新のあと再起動していない前の版が動いているときだけ (LAN から起動したカーネルなどでは断る)
	if v["version"] != u["current_version"] && !(v["version"] == u["previous_version"] && v["commit"] == u["previous_commit"]) {
		return fmt.Errorf("動いている版 (%s) が、更新した版 (%s) ではない (LAN から起動しているなど)。書いていない", v["version"], u["current_version"])
	}
	got, err := c.fileSHA256(ctx, to)
	if err != nil {
		return err
	}
	if !reHex64.MatchString(u["previous_sha256"]) || got != u["previous_sha256"] {
		return fmt.Errorf("%s が、更新したときの前の版と違う。書いていない", to)
	}
	if err := sectorCheck(cfg, sections); err != nil {
		return err
	}
	want, err := setKernel(cfg, sections, to)
	if err != nil {
		return err
	}
	backup, err := a.backupConfig(c, cfg)
	if err != nil {
		return err
	}
	fmt.Fprintf(a.stdout, "config.txt の控え: %s\n", backup)
	if err := c.switchKernel(ctx, name, to, got); err != nil {
		return switchFailed(err, name, backup)
	}
	if err := c.checkConfig(ctx, want); err != nil {
		return maybeSwitched(err, backup)
	}
	fmt.Fprintf(a.stdout, "config.txt: kernel=%s を kernel=%s に替えた\n", name, to)
	// 更新のあとに再起動していなければ、動いているのは戻す先の版なので、再起動は要らない
	if v["version"] == u["previous_version"] && v["commit"] == u["previous_commit"] {
		fmt.Fprintf(a.stdout, "%s が動いているので、再起動していない\n", v["version"])
		return nil
	}
	if err := a.rebootAndWait(ctx, c, u["previous_version"], u["previous_commit"]); err != nil {
		return fmt.Errorf("%w\n起動しないときは、SD カードを PC に入れ、config.txt の kernel= を直す (控えは %s)", err, backup)
	}
	return nil
}
