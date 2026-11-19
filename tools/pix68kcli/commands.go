package main

import (
	"bufio"
	"bytes"
	"context"
	"errors"
	"flag"
	"fmt"
	"image/png"
	"io"
	"io/fs"
	"net/url"
	"os"
	"path"
	"path/filepath"
	"regexp"
	"runtime"
	"strconv"
	"strings"
	"time"
)

// GET / の使い方の一覧に、その口の行があるか。古いカーネルは知らない口のクエリを読まずに受けることがある
// (POST /reboot?profile= は #231 より前のカーネルだと、profile= を無視してそのまま再起動する) ので、先に確かめる
func (c *Client) hasEndpoint(ctx context.Context, marker string) (bool, error) {
	b, err := c.fetch(ctx, reqSpec{method: "GET", escapedPath: "/"})
	if err != nil {
		return false, err
	}
	return bytes.Contains(b, []byte(marker)), nil
}

// 404 を「このカーネルにはその口がない」にする (/files 以外の口で使う)
func unsupportedOn404(err error, what string) error {
	var ce *cliError
	if errors.As(err, &ce) && ce.code == exitNotFound {
		return &cliError{exitUnsupported, ce.msg + "\n(動いているカーネルに " + what + " がない。新しいカーネルに入れ替える)"}
	}
	return err
}

func noArgs(fs *flag.FlagSet) error {
	if fs.NArg() > 0 {
		return usageErr(fmt.Sprintf("%s: 余計な引数 %q", fs.Name(), fs.Arg(0)))
	}
	return nil
}

func (a *app) cmdSimpleGet(ctx context.Context, c *Client, args []string, p string) error {
	fs := flag.NewFlagSet(strings.TrimPrefix(p, "/"), flag.ContinueOnError)
	if err := parseFlags(fs, args); err != nil {
		return err
	}
	if err := noArgs(fs); err != nil {
		return err
	}
	b, err := c.fetch(ctx, reqSpec{method: "GET", escapedPath: p})
	if err != nil {
		return unsupportedOn404(err, "GET "+p)
	}
	_, err = a.stdout.Write(b)
	return err
}

// ---- status ----

var reSpeed = regexp.MustCompile(`(?m)^.*\bx68k: speed: .*$`)
var reCPU = regexp.MustCompile(`(?m)^.*\bx68k: cpu: .*$`)

// 起動したときの 1 行 (baremetal/kernel.cpp)。ログが 64KB を超えると (X68000 を動かして 10 分ほどで) 消える
var reBoot = regexp.MustCompile(`pix68k bare metal on (.*?), compiled (.*)`)

func (a *app) cmdStatus(ctx context.Context, c *Client, args []string) error {
	fs := flag.NewFlagSet("status", flag.ContinueOnError)
	if err := parseFlags(fs, args); err != nil {
		return err
	}
	if err := noArgs(fs); err != nil {
		return err
	}
	w := a.stdout
	t0 := time.Now()
	help, err := c.fetch(ctx, reqSpec{method: "GET", escapedPath: "/", timeout: 10 * time.Second})
	if err != nil {
		return err
	}
	name := c.target.Host
	if c.target.Name != "" {
		name = c.target.Name + " (" + c.target.Host + ")"
	}
	fmt.Fprintf(w, "host: %s: つながる (%d ms)\n", name, time.Since(t0).Milliseconds())
	var feats []string
	for _, f := range []struct{ marker, name string }{
		{"/files/", "files"}, {"/log", "log"}, {"/thermal", "thermal"}, {"/screen.ppm", "screen"},
		{"/reboot", "reboot"}, {"/reboot?profile=", "reboot-profile"}, {"/type", "type"}, {"/key", "key"},
		{"/message", "message"},
	} {
		if bytes.Contains(help, []byte(f.marker)) {
			feats = append(feats, f.name)
		}
	}
	fmt.Fprintf(w, "api: %s\n", strings.Join(feats, " "))
	for _, line := range strings.Split(string(help), "\n") {
		if strings.HasPrefix(line, "syslog:") {
			fmt.Fprintln(w, line)
		}
	}
	// token が通るか (net_token= のある口では GET /files/ にもトークンが要る)
	var statusErr error
	switch {
	case c.target.Token == "":
		fmt.Fprintln(w, "token: なし")
	default:
		_, err := c.fetch(ctx, reqSpec{method: "GET", escapedPath: "/files/", auth: true, timeout: 10 * time.Second})
		var ce *cliError
		switch {
		case err == nil:
			// token なしでも読めるなら、口に net_token= がない (token を確かめていない)
			// token なしで 403 を返したときだけ、token が要る口で token が通ったと言える
			_, err2 := c.fetch(ctx, reqSpec{method: "GET", escapedPath: "/files/", timeout: 10 * time.Second})
			var ce2 *cliError
			switch {
			case err2 == nil:
				fmt.Fprintf(w, "token: 確かめていない (口に net_token= がなく、token なしでも /files を読める。%s)\n", c.target.TokenSrc)
			case errors.As(err2, &ce2) && ce2.code == exitForbidden:
				fmt.Fprintf(w, "token: 通る (%s)\n", c.target.TokenSrc)
			default:
				fmt.Fprintf(w, "token: 確かめられない (token なしで読んだときの結果: %v)\n", err2)
			}
		case errors.As(err, &ce) && ce.code == exitForbidden:
			fmt.Fprintf(w, "token: 断られた (%s): %s\n", c.target.TokenSrc, ce.msg)
			statusErr = &cliError{exitForbidden, "token が断られた"}
		default:
			fmt.Fprintf(w, "token: 確かめられない: %v\n", err)
		}
	}
	if th, err := c.fetch(ctx, reqSpec{method: "GET", escapedPath: "/thermal", timeout: 10 * time.Second}); err == nil {
		fmt.Fprintln(w, "thermal:")
		for _, line := range strings.Split(strings.TrimRight(string(th), "\n"), "\n") {
			fmt.Fprintln(w, "  "+line)
		}
	} else {
		fmt.Fprintf(w, "thermal: 読めない: %v\n", err)
	}
	if lg, err := c.fetch(ctx, reqSpec{method: "GET", escapedPath: "/log", timeout: 10 * time.Second}); err == nil {
		lg = trimPartialHead(lg)
		if m := reBoot.FindSubmatch(lg); m != nil {
			fmt.Fprintf(w, "model: %s (カーネルのビルド %s)\n", strings.TrimSpace(string(m[1])), strings.TrimSpace(string(m[2])))
		} else {
			fmt.Fprintln(w, "model: 分からない (起動したときのログの行が、64KB のログから押し出された)")
		}
		for _, re := range []*regexp.Regexp{reSpeed, reCPU} {
			if m := re.FindAll(lg, -1); len(m) > 0 {
				fmt.Fprintf(w, "log: %s\n", strings.TrimSpace(string(m[len(m)-1])))
			}
		}
	} else {
		fmt.Fprintf(w, "log: 読めない: %v\n", err)
	}
	return statusErr
}

// ---- log ----

func (a *app) cmdLog(ctx context.Context, c *Client, args []string) error {
	fs := flag.NewFlagSet("log", flag.ContinueOnError)
	follow := fs.Bool("f", false, "")
	n := fs.Int("n", 0, "")
	interval := fs.Float64("interval", 1, "")
	if err := parseFlags(fs, args); err != nil {
		return err
	}
	if err := noArgs(fs); err != nil {
		return err
	}
	// NaN は比べるとどちらも偽になるので、範囲の中にあることを確かめる
	if !(*interval >= 0.2 && *interval <= 3600) {
		return usageErr("log: --interval は 0.2 秒から 3600 秒まで")
	}
	get := func() ([]byte, error) {
		return c.fetch(ctx, reqSpec{method: "GET", escapedPath: "/log", timeout: 15 * time.Second})
	}
	b, err := get()
	if err != nil && (!*follow || !retryable(err)) {
		return err
	}
	var prev []byte // 最後に読めた本文 (まだ読めていなければ nil)
	down := false
	if err == nil {
		if _, err := a.stdout.Write(lastLines(trimPartialHead(b), *n)); err != nil {
			return err
		}
		if !*follow {
			return nil
		}
		prev = b
	} else {
		// 再起動の途中で始めたときなど。つながるまで待つ
		fmt.Fprintf(a.stderr, "pix68kcli: %v (待ち続ける)\n", err)
		down = true
	}
	tick := time.NewTicker(time.Duration(*interval * float64(time.Second)))
	defer tick.Stop()
	for {
		select {
		case <-ctx.Done():
			return nil
		case <-tick.C:
		}
		cur, err := get()
		if err != nil {
			if ctx.Err() != nil {
				return nil
			}
			// 再起動の間はつながらないので、待ち続ける。403 や 404 は待っても変わらないので終える
			if !retryable(err) {
				return err
			}
			if !down {
				fmt.Fprintf(a.stderr, "pix68kcli: %v (待ち続ける)\n", err)
				down = true
			}
			continue
		}
		if down {
			fmt.Fprintln(a.stderr, "pix68kcli: つながった")
			down = false
		}
		var d []byte
		if prev == nil {
			d = lastLines(trimPartialHead(cur), *n)
		} else {
			var restarted bool
			d, restarted = logDelta(prev, cur)
			if restarted {
				fmt.Fprintln(a.stderr, "pix68kcli: --- ログが続いていない (再起動したか、読む間に 64KB より多く増えた) ---")
				d = trimPartialHead(cur)
			}
		}
		if _, err := a.stdout.Write(dropLogPolls(d)); err != nil {
			return err
		}
		prev = cur
	}
}

// log -f で待って読み直す誤り: つながらない (再起動の途中) と 503
func retryable(err error) bool {
	var ce *cliError
	if !errors.As(err, &ce) {
		return false
	}
	return ce.code == exitConnect || (ce.code == exitBusy && strings.Contains(ce.msg, ": 503 "))
}

// ---- screen ----

func (a *app) cmdScreen(ctx context.Context, c *Client, args []string) error {
	fs := flag.NewFlagSet("screen", flag.ContinueOnError)
	out := fs.String("o", "screen.png", "")
	if err := parseFlags(fs, args); err != nil {
		return err
	}
	if err := noArgs(fs); err != nil {
		return err
	}
	b, err := c.fetch(ctx, reqSpec{method: "GET", escapedPath: "/screen.ppm"})
	if err != nil {
		return unsupportedOn404(err, "GET /screen.ppm")
	}
	if strings.HasSuffix(strings.ToLower(*out), ".ppm") {
		return writeFileAtomic(*out, bytes.NewReader(b))
	}
	img, err := decodePPM(b)
	if err != nil {
		return err
	}
	var buf bytes.Buffer
	if err := png.Encode(&buf, img); err != nil {
		return err
	}
	if *out == "-" {
		_, err := a.stdout.Write(buf.Bytes())
		return err
	}
	if err := writeFileAtomic(*out, &buf); err != nil {
		return err
	}
	fmt.Fprintf(a.stderr, "%s (%dx%d)\n", *out, img.Rect.Dx(), img.Rect.Dy())
	return nil
}

// ---- reboot / profiles ----

var reProfileName = regexp.MustCompile(`^[A-Za-z0-9_-]*$`)

func (a *app) cmdReboot(ctx context.Context, c *Client, args []string) error {
	fs := flag.NewFlagSet("reboot", flag.ContinueOnError)
	profile := fs.String("profile", "", "")
	if err := parseFlags(fs, args); err != nil {
		return err
	}
	if err := noArgs(fs); err != nil {
		return err
	}
	withProfile := false
	fs.Visit(func(f *flag.Flag) {
		if f.Name == "profile" {
			withProfile = true
		}
	})
	r := reqSpec{method: "POST", escapedPath: "/reboot", auth: true, body: bytes.NewReader(nil), length: 0}
	if withProfile {
		if !reProfileName.MatchString(*profile) {
			return usageErr("reboot: --profile の名前は英数字と - と _ だけ (pix68k-<名前>.cfg。空なら pix68k.cfg だけ)")
		}
		ok, err := c.hasEndpoint(ctx, "/reboot?profile=")
		if err != nil {
			return err
		}
		if !ok {
			return &cliError{exitUnsupported, c.target.Host + ": 動いているカーネルは POST /reboot?profile= に対応していない " +
				"(古いカーネルは profile= を無視して再起動するので、送らなかった)"}
		}
		r.query = url.Values{"profile": {*profile}}
	}
	b, err := c.fetch(ctx, r)
	if err != nil {
		return err
	}
	_, err = a.stdout.Write(b)
	return err
}

func (a *app) cmdProfiles(ctx context.Context, c *Client, args []string) error {
	fs := flag.NewFlagSet("profiles", flag.ContinueOnError)
	if err := parseFlags(fs, args); err != nil {
		return err
	}
	if err := noArgs(fs); err != nil {
		return err
	}
	b, err := c.fetch(ctx, reqSpec{method: "GET", escapedPath: "/files/pix68k/", auth: true})
	if err != nil {
		return err
	}
	for _, e := range parseListing(b) {
		if e.name == "..." {
			fmt.Fprintln(a.stderr, "pix68kcli: /pix68k の一覧が長いので口が途中で切った。この後ろの組み合わせは出ていない")
			continue
		}
		low := strings.ToLower(e.name)
		if !e.dir && strings.HasPrefix(low, "pix68k-") && strings.HasSuffix(low, ".cfg") {
			name := e.name[len("pix68k-") : len(e.name)-len(".cfg")]
			if name != "" && reProfileName.MatchString(name) {
				fmt.Fprintln(a.stdout, name)
			}
		}
	}
	return nil
}

// ---- type / key / message ----

const typeMax = 4096

func (a *app) cmdType(ctx context.Context, c *Client, args []string) error {
	fs := flag.NewFlagSet("type", flag.ContinueOnError)
	if err := parseFlags(fs, args); err != nil {
		return err
	}
	if fs.NArg() == 0 {
		return usageErr("type: 打つ文字列がない (- なら標準入力)")
	}
	s, err := joinArgs(a, fs.Args())
	if err != nil {
		return err
	}
	if len(s) > typeMax {
		return usageErr(fmt.Sprintf("type: %d バイトある (口は %d バイトまで)", len(s), typeMax))
	}
	// 口は打ち終えてから返す (1 字 80ms)
	r := reqSpec{method: "POST", escapedPath: "/type", auth: true, body: strings.NewReader(s), length: int64(len(s)),
		timeout: time.Duration(len(s))*100*time.Millisecond + 30*time.Second}
	b, err := c.fetch(ctx, r)
	if err != nil {
		return unsupportedOn404(err, "POST /type")
	}
	_, err = a.stdout.Write(b)
	return err
}

var reHexByte = regexp.MustCompile(`^(0[xX])?[0-9A-Fa-f]{1,2}$`)

func (a *app) cmdKey(ctx context.Context, c *Client, args []string) error {
	if len(args) == 0 {
		return usageErr("key: down か up を書く")
	}
	sub := args[0]
	fs := flag.NewFlagSet("key "+sub, flag.ContinueOnError)
	mod := fs.String("mod", "", "")
	if err := parseFlags(fs, args[1:]); err != nil {
		return err
	}
	// key down 4c --mod 09 のように、引数の後ろに書いた --mod も読む
	var pos []string
	for rest := fs.Args(); len(rest) > 0; rest = fs.Args() {
		pos = append(pos, rest[0])
		if err := parseFlags(fs, rest[1:]); err != nil {
			return err
		}
	}
	if err := fs.Parse(pos); err != nil {
		return usageErr(err.Error())
	}
	// RawQuery を手で組む (/key?up は値なしで「すべて離す」)
	var raw string
	switch sub {
	case "down":
		if fs.NArg() != 1 || !reHexByte.MatchString(fs.Arg(0)) {
			return usageErr("key down: USB の usage ID を 16 進で 1 つ (例 4c は Delete)")
		}
		raw = "down=" + fs.Arg(0)
		if *mod != "" {
			if !reHexByte.MatchString(*mod) {
				return usageErr("key down: --mod は 16 進の 1 バイト (01 左 Ctrl、02 左 Shift、04 左 Alt、08 左 GUI)")
			}
			raw += "&mod=" + *mod
		}
	case "up":
		if *mod != "" {
			return usageErr("key up: --mod は使えない")
		}
		switch fs.NArg() {
		case 0:
			raw = "up"
		case 1:
			if !reHexByte.MatchString(fs.Arg(0)) {
				return usageErr("key up: USB の usage ID を 16 進で (なければすべて離す)")
			}
			raw = "up=" + fs.Arg(0)
		default:
			return usageErr("key up: 引数は 1 つまで")
		}
	default:
		return usageErr(fmt.Sprintf("key: 知らない命令 %q (down か up)", sub))
	}
	b, err := c.fetch(ctx, reqSpec{method: "POST", escapedPath: "/key?" + raw, auth: true, body: bytes.NewReader(nil)})
	if err != nil {
		return unsupportedOn404(err, "POST /key")
	}
	_, err = a.stdout.Write(b)
	return err
}

const messageMax = 1024

func (a *app) cmdMessage(ctx context.Context, c *Client, args []string) error {
	fs := flag.NewFlagSet("message", flag.ContinueOnError)
	sec := fs.Int("sec", -1, "")
	clear := fs.Bool("clear", false, "")
	if err := parseFlags(fs, args); err != nil {
		return err
	}
	var s string
	switch {
	case *clear:
		if fs.NArg() > 0 || *sec >= 0 {
			return usageErr("message --clear: 文字列と --sec は書かない")
		}
	case fs.NArg() == 0:
		return usageErr("message: 出す文字列がない (消すなら --clear)")
	default:
		var err error
		if s, err = joinArgs(a, fs.Args()); err != nil {
			return err
		}
		if strings.TrimSpace(s) == "" {
			return usageErr("message: 文字列が空 (消すなら --clear)")
		}
	}
	if len(s) > messageMax {
		return usageErr(fmt.Sprintf("message: %d バイトある (口は %d バイトまで)", len(s), messageMax))
	}
	if *sec > 86400 {
		return usageErr("message: --sec は 86400 まで")
	}
	r := reqSpec{method: "POST", escapedPath: "/message", auth: true, body: strings.NewReader(s), length: int64(len(s))}
	if *sec >= 0 {
		r.query = url.Values{"sec": {strconv.Itoa(*sec)}}
	}
	b, err := c.fetch(ctx, r)
	if err != nil {
		return unsupportedOn404(err, "POST /message")
	}
	_, err = a.stdout.Write(b)
	return err
}

// ---- files ----

type listEntry struct {
	name string
	dir  bool
	size int64
}

// 一覧の本文 (1 行に「名前<TAB>バイト数」、ディレクトリは名前の後ろに /。入りきらなければ最後に ...)
func parseListing(b []byte) []listEntry {
	var out []listEntry
	sc := bufio.NewScanner(bytes.NewReader(b))
	for sc.Scan() {
		line := sc.Text()
		name, sz, ok := strings.Cut(line, "\t")
		if !ok {
			if line == "..." {
				out = append(out, listEntry{name: "..."})
			}
			continue
		}
		e := listEntry{name: name}
		if strings.HasSuffix(name, "/") {
			e.dir = true
			e.name = strings.TrimSuffix(name, "/")
		}
		e.size, _ = strconv.ParseInt(sz, 10, 64)
		out = append(out, e)
	}
	return out
}

func (a *app) cmdFiles(ctx context.Context, c *Client, args []string) error {
	if len(args) == 0 {
		return usageErr("files: ls、get、put、rm のどれかを書く")
	}
	sub := args[0]
	fs := flag.NewFlagSet("files "+sub, flag.ContinueOnError)
	if err := parseFlags(fs, args[1:]); err != nil {
		return err
	}
	p := fs.Args()
	switch sub {
	case "ls":
		if len(p) > 1 {
			return usageErr("files ls: パスは 1 つまで")
		}
		dir := "/"
		if len(p) == 1 {
			dir = p[0]
		}
		if !strings.HasSuffix(dir, "/") {
			dir += "/"
		}
		r := reqSpec{method: "GET", escapedPath: filesPath(dir), auth: true}
		resp, cancel, err := c.do(ctx, r)
		if err != nil {
			return err
		}
		defer cancel()
		defer resp.Body.Close()
		// 口は一覧を text/plain、ファイルを application/octet-stream で返す。ファイルなら読まずに終える
		if isFileResponse(resp.Header.Get("Content-Type")) {
			return &cliError{exitError, fmt.Sprintf("%s: %s はディレクトリではなくファイル (読むなら files get)", c.target.Host, strings.TrimSuffix(dir, "/"))}
		}
		b, err := io.ReadAll(resp.Body)
		if err != nil {
			return c.connErr(r, err)
		}
		ents := parseListing(b)
		width := 1
		for _, e := range ents {
			if n := len(strconv.FormatInt(e.size, 10)); !e.dir && n > width {
				width = n
			}
		}
		for _, e := range ents {
			switch {
			case e.name == "...":
				fmt.Fprintln(a.stdout, "(一覧が長いので口が切った)")
			case e.dir:
				fmt.Fprintf(a.stdout, "%*s  %s/\n", width, "-", e.name)
			default:
				fmt.Fprintf(a.stdout, "%*d  %s\n", width, e.size, e.name)
			}
		}
		return nil
	case "get":
		if len(p) < 1 || len(p) > 2 {
			return usageErr("files get: パス [手元 | -]")
		}
		local := path.Base(strings.ReplaceAll(p[0], `\`, "/"))
		if len(p) == 2 {
			local = p[1]
		}
		if local == "/" || local == "." || local == "" {
			return usageErr("files get: 手元のファイル名を書く")
		}
		resp, cancel, err := c.do(ctx, reqSpec{method: "GET", escapedPath: filesPath(p[0]), auth: true, noTimeout: true})
		if err != nil {
			return err
		}
		defer cancel()
		defer resp.Body.Close()
		if !isFileResponse(resp.Header.Get("Content-Type")) {
			return &cliError{exitError, fmt.Sprintf("%s: %s はディレクトリ (一覧は files ls)", c.target.Host, p[0])}
		}
		// 受け取る側の失敗 (途中で切れた) は「つながらない」(3)、手元に書く側の失敗はそのほか (1) にする
		body := &readErrRecorder{r: resp.Body}
		r := reqSpec{method: "GET", escapedPath: filesPath(p[0])}
		if local == "-" {
			if _, err := io.Copy(a.stdout, body); err != nil {
				if body.err != nil {
					return c.connErr(r, body.err)
				}
				return err
			}
			return nil
		}
		if st, err := os.Stat(local); err == nil && st.IsDir() {
			local = filepath.Join(local, path.Base(strings.ReplaceAll(p[0], `\`, "/")))
		}
		if err := writeFileAtomic(local, body); err != nil {
			if body.err != nil {
				return c.connErr(r, body.err)
			}
			return err
		}
		fmt.Fprintln(a.stderr, local)
		return nil
	case "put":
		if len(p) != 2 {
			return usageErr("files put: 手元 パス")
		}
		local, remote := p[0], p[1]
		var body io.Reader
		var size int64
		if local == "-" {
			b, err := io.ReadAll(a.stdin)
			if err != nil {
				return err
			}
			body, size = bytes.NewReader(b), int64(len(b))
			if strings.HasSuffix(remote, "/") {
				return usageErr("files put: 標準入力から書くときは、パスにファイル名まで書く")
			}
		} else {
			f, err := os.Open(local)
			if err != nil {
				return err
			}
			defer f.Close()
			st, err := f.Stat()
			if err != nil {
				return err
			}
			if !st.Mode().IsRegular() {
				return usageErr("files put: " + local + " はふつうのファイルではない")
			}
			body, size = f, st.Size()
			if strings.HasSuffix(remote, "/") {
				remote += filepath.Base(local)
			}
		}
		// Content-Length を必ず付ける (口は chunked を断る)。大きなファイルは時間がかかるので時間切れにしない
		b, err := c.fetch(ctx, reqSpec{method: "PUT", escapedPath: filesPath(remote), auth: true, body: body, length: size, noTimeout: true, expect: true})
		if err != nil {
			return err
		}
		_, err = a.stdout.Write(b)
		return err
	case "rm":
		if len(p) != 1 {
			return usageErr("files rm: パスを 1 つ")
		}
		b, err := c.fetch(ctx, reqSpec{method: "DELETE", escapedPath: filesPath(p[0]), auth: true})
		if err != nil {
			return err
		}
		_, err = a.stdout.Write(b)
		return err
	}
	return usageErr(fmt.Sprintf("files: 知らない命令 %q (ls、get、put、rm)", sub))
}

// 口がファイルの中身を返したか (一覧は text/plain)
func isFileResponse(ct string) bool {
	return !strings.HasPrefix(strings.ToLower(strings.TrimSpace(ct)), "text/plain")
}

// 一時ファイルに書いてから名前を変える (途中で切れたら元のファイルを残す)。
// 権限は、元のファイルがあればその権限 (広げない。pix68k.cfg の net_token= を 0600 で持っていることがある)、
// なければ 0666 から umask を除いたもの (os.OpenFile が umask を当てる)
func writeFileAtomic(name string, r io.Reader) error {
	dir := filepath.Dir(name)
	// 元の権限は作る前に取り、一時ファイルを初めからその権限 (と umask) で作る。0666 で作ってから狭めると、
	// その間に他のユーザーが開けるため
	perm := os.FileMode(0o666)
	keep := false
	if st, serr := os.Stat(name); serr == nil && st.Mode().IsRegular() {
		perm, keep = st.Mode().Perm(), true
	}
	var tmp *os.File
	var err error
	for i := 0; i < 100; i++ {
		tn := filepath.Join(dir, fmt.Sprintf(".%s.%d.%d.part", filepath.Base(name), os.Getpid(), time.Now().UnixNano()+int64(i)))
		tmp, err = os.OpenFile(tn, os.O_WRONLY|os.O_CREATE|os.O_EXCL, perm)
		if !errors.Is(err, fs.ErrExist) {
			break
		}
	}
	if err != nil {
		return err
	}
	if afterTempCreate != nil {
		afterTempCreate(tmp.Name())
	}
	// umask で元より狭くなった分を戻す (元の権限より広げることはない)
	if keep {
		if err := tmp.Chmod(perm); err != nil && runtime.GOOS != "windows" {
			tmp.Close()
			os.Remove(tmp.Name())
			return err
		}
	}
	_, err = io.Copy(tmp, r)
	if cerr := tmp.Close(); err == nil {
		err = cerr
	}
	if err != nil {
		os.Remove(tmp.Name())
		return err
	}
	if err := os.Rename(tmp.Name(), name); err != nil {
		os.Remove(tmp.Name())
		return err
	}
	return nil
}

// 試験用: 一時ファイルを作った直後に呼ぶ
var afterTempCreate func(name string)

// 読んだときの誤りを覚えておく (io.Copy の誤りが、読む側と書く側のどちらのものか分けるため)
type readErrRecorder struct {
	r   io.Reader
	err error
}

func (e *readErrRecorder) Read(p []byte) (int, error) {
	n, err := e.r.Read(p)
	if err != nil && err != io.EOF {
		e.err = err
	}
	return n, err
}
