package main

import (
	"bytes"
	"context"
	"fmt"
	"image/png"
	"io"
	"net/http"
	"net/http/httptest"
	"os"
	"path/filepath"
	"runtime"
	"strings"
	"sync"
	"testing"
	"time"
)

const testToken = "test-token-123"

const helpOld = "pix68k\nGET  /files/<path>    read a file\nGET  /log             log\nPOST /reboot          reboot\nPOST /type            type\nPOST /key?down=XX&mod=YY\nsyslog: off\n"
const helpNew = helpOld + "POST /reboot?profile=NAME   reboot with /pix68k/pix68k-NAME.cfg\nPOST /message?sec=N   show\n"

// 口の偽物。受けた要求を記録し、handler で応える
type recorded struct {
	method, uri, token, body string
	contentLength            int64
	transferEncoding         []string
	origin                   string
	host                     string
}

type fakeServer struct {
	t       *testing.T
	srv     *httptest.Server
	mu      sync.Mutex
	reqs    []recorded
	handler func(w http.ResponseWriter, r *http.Request, body []byte)
}

func newFake(t *testing.T, h func(w http.ResponseWriter, r *http.Request, body []byte)) *fakeServer {
	f := &fakeServer{t: t, handler: h}
	f.srv = httptest.NewServer(http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		b, _ := io.ReadAll(r.Body)
		f.mu.Lock()
		f.reqs = append(f.reqs, recorded{r.Method, r.RequestURI, r.Header.Get("X-Pix68k-Token"), string(b),
			r.ContentLength, r.TransferEncoding, r.Header.Get("Origin"), r.Host})
		f.mu.Unlock()
		f.handler(w, r, b)
	}))
	t.Cleanup(f.srv.Close)
	return f
}

func (f *fakeServer) host() string { return strings.TrimPrefix(f.srv.URL, "http://") }

// 命令を動かし、終了コード、標準出力、標準エラーを返す
func (f *fakeServer) run(stdin string, env map[string]string, args ...string) (int, string, string) {
	return f.runCtx(context.Background(), stdin, env, args...)
}

func (f *fakeServer) runCtx(ctx context.Context, stdin string, env map[string]string, args ...string) (int, string, string) {
	var out, errb bytes.Buffer
	if env == nil {
		env = map[string]string{}
	}
	if _, ok := env["PIX68K_TOKEN"]; !ok {
		env["PIX68K_TOKEN"] = testToken
	}
	if _, ok := env["XDG_CONFIG_HOME"]; !ok {
		env["XDG_CONFIG_HOME"] = f.t.TempDir() // 設定ファイルはない
	}
	a := &app{stdout: &out, stderr: &errb, stdin: strings.NewReader(stdin), getenv: func(k string) string { return env[k] }}
	full := append([]string{"--host", f.host()}, args...)
	code := a.run(ctx, full)
	return code, out.String(), errb.String()
}

func ok(text string) func(http.ResponseWriter, *http.Request, []byte) {
	return func(w http.ResponseWriter, r *http.Request, _ []byte) {
		if r.URL.Path == "/" {
			io.WriteString(w, helpNew)
			return
		}
		// 口と同じく、/files/ のファイルは application/octet-stream、一覧とほかは text/plain で返す
		if strings.HasPrefix(r.URL.Path, "/files/") && !strings.HasSuffix(r.URL.Path, "/") {
			w.Header().Set("Content-Type", "application/octet-stream")
		} else {
			w.Header().Set("Content-Type", "text/plain; charset=utf-8")
		}
		io.WriteString(w, text)
	}
}

func (f *fakeServer) only(t *testing.T) recorded {
	t.Helper()
	if len(f.reqs) != 1 {
		t.Fatalf("要求の数 %d、1 のはず: %+v", len(f.reqs), f.reqs)
	}
	return f.reqs[0]
}

func expect(t *testing.T, got recorded, method, uri string, token bool, body string) {
	t.Helper()
	if got.method != method || got.uri != uri {
		t.Errorf("要求 %s %s、%s %s のはず", got.method, got.uri, method, uri)
	}
	wantTok := ""
	if token {
		wantTok = testToken
	}
	if got.token != wantTok {
		t.Errorf("%s %s: token %q、%q のはず", method, uri, got.token, wantTok)
	}
	if got.body != body {
		t.Errorf("%s %s: 本文 %q、%q のはず", method, uri, got.body, body)
	}
	if len(got.transferEncoding) != 0 {
		t.Errorf("%s %s: Transfer-Encoding %v (口は断る)", method, uri, got.transferEncoding)
	}
	if got.origin != "" {
		t.Errorf("Origin を付けている")
	}
}

func TestType(t *testing.T) {
	f := newFake(t, ok("typed 4 keys (0 characters skipped)\n"))
	code, out, _ := f.run("", nil, "type", `dir\n`)
	if code != 0 || !strings.Contains(out, "typed 4") {
		t.Fatalf("code %d out %q", code, out)
	}
	r := f.only(t)
	expect(t, r, "POST", "/type", true, `dir\n`) // \n はそのまま送る (口が読む)
	if r.contentLength != 5 {
		t.Errorf("Content-Length %d", r.contentLength)
	}
}

func TestTypeStdinAndJoin(t *testing.T) {
	f := newFake(t, ok("typed\n"))
	f.run("echo a\n", nil, "type", "-")
	f.run("", nil, "type", "a", "b")
	expect(t, f.reqs[0], "POST", "/type", true, "echo a\n")
	expect(t, f.reqs[1], "POST", "/type", true, "a b")
}

func TestTypeTooLong(t *testing.T) {
	f := newFake(t, ok(""))
	code, _, _ := f.run("", nil, "type", strings.Repeat("a", 4097))
	if code != exitUsage || len(f.reqs) != 0 {
		t.Fatalf("code %d reqs %d", code, len(f.reqs))
	}
}

func TestMessage(t *testing.T) {
	f := newFake(t, ok("shown for 30 s\n"))
	if code, _, _ := f.run("", nil, "message", "--sec", "30", `案1\nいまの版`); code != 0 {
		t.Fatal(code)
	}
	expect(t, f.only(t), "POST", "/message?sec=30", true, `案1\nいまの版`)
}

func TestMessageDefaultAndClear(t *testing.T) {
	f := newFake(t, ok("cleared\n"))
	f.run("", nil, "message", "hello")
	f.run("", nil, "message", "--clear")
	f.run("", nil, "message", "--sec", "0", "x")
	expect(t, f.reqs[0], "POST", "/message", true, "hello")
	expect(t, f.reqs[1], "POST", "/message", true, "")
	if f.reqs[1].contentLength != 0 {
		t.Errorf("--clear の Content-Length %d", f.reqs[1].contentLength)
	}
	expect(t, f.reqs[2], "POST", "/message?sec=0", true, "x")
}

func TestMessageUnsupported(t *testing.T) {
	f := newFake(t, func(w http.ResponseWriter, r *http.Request, _ []byte) {
		w.WriteHeader(404)
		io.WriteString(w, "not found (GET / for the list)\n")
	})
	code, _, errs := f.run("", nil, "message", "x")
	if code != exitUnsupported || !strings.Contains(errs, "POST /message") {
		t.Fatalf("code %d err %q", code, errs)
	}
}

func TestKey(t *testing.T) {
	f := newFake(t, ok("ok\n"))
	f.run("", nil, "key", "down", "--mod", "09", "4c")
	f.run("", nil, "key", "up", "4c")
	f.run("", nil, "key", "up")
	f.run("", nil, "key", "down", "4c", "--mod", "09") // --mod を後ろに書いてもよい
	expect(t, f.reqs[0], "POST", "/key?down=4c&mod=09", true, "")
	expect(t, f.reqs[1], "POST", "/key?up=4c", true, "")
	expect(t, f.reqs[2], "POST", "/key?up", true, "")
	expect(t, f.reqs[3], "POST", "/key?down=4c&mod=09", true, "")
	if code, _, _ := f.run("", nil, "key", "down", "zz"); code != exitUsage {
		t.Errorf("16 進でない usage を通した: %d", code)
	}
}

func TestReboot(t *testing.T) {
	f := newFake(t, ok("rebooting\n"))
	if code, out, _ := f.run("", nil, "reboot"); code != 0 || out != "rebooting\n" {
		t.Fatal(code, out)
	}
	expect(t, f.only(t), "POST", "/reboot", true, "")
}

func TestRebootProfile(t *testing.T) {
	f := newFake(t, ok("rebooting\n"))
	if code, _, _ := f.run("", nil, "reboot", "--profile", "gr2"); code != 0 {
		t.Fatal(code)
	}
	if len(f.reqs) != 2 {
		t.Fatalf("要求 %+v", f.reqs)
	}
	expect(t, f.reqs[0], "GET", "/", false, "")
	expect(t, f.reqs[1], "POST", "/reboot?profile=gr2", true, "")
	// 空の名前は「組み合わせなし」
	f.reqs = nil
	f.run("", nil, "reboot", "--profile=")
	expect(t, f.reqs[1], "POST", "/reboot?profile=", true, "")
}

// #231 より前のカーネル (GET / に reboot?profile= の行がない) には POST を送らない
func TestRebootProfileOldKernel(t *testing.T) {
	f := newFake(t, func(w http.ResponseWriter, r *http.Request, _ []byte) {
		if r.URL.Path == "/" {
			io.WriteString(w, helpOld)
			return
		}
		io.WriteString(w, "rebooting\n")
	})
	code, _, errs := f.run("", nil, "reboot", "--profile", "gr2")
	if code != exitUnsupported {
		t.Fatalf("code %d err %q", code, errs)
	}
	for _, r := range f.reqs {
		if r.method == "POST" {
			t.Fatalf("古いカーネルに POST を送った: %+v", r)
		}
	}
}

func TestRebootProfileBadName(t *testing.T) {
	f := newFake(t, ok(""))
	if code, _, _ := f.run("", nil, "reboot", "--profile", "../x"); code != exitUsage || len(f.reqs) != 0 {
		t.Fatal(code, f.reqs)
	}
}

func TestRebootProfileNotFound(t *testing.T) {
	f := newFake(t, func(w http.ResponseWriter, r *http.Request, _ []byte) {
		if r.URL.Path == "/" {
			io.WriteString(w, helpNew)
			return
		}
		w.WriteHeader(404)
		io.WriteString(w, "no such profile (pix68k-<name>.cfg in /pix68k)\n")
	})
	code, _, errs := f.run("", nil, "reboot", "--profile", "nope")
	if code != exitNotFound || !strings.Contains(errs, "no such profile") {
		t.Fatalf("code %d err %q", code, errs)
	}
}

func TestProfiles(t *testing.T) {
	f := newFake(t, ok("pix68k.cfg\t100\npix68k-gr2.cfg\t50\nPIX68K-OUTRUN.CFG\t10\npix68k-x.new\t1\nsub/\t0\nprofile.sel\t4\n"))
	code, out, _ := f.run("", nil, "profiles")
	if code != 0 || out != "gr2\nOUTRUN\n" {
		t.Fatalf("code %d out %q", code, out)
	}
	expect(t, f.only(t), "GET", "/files/pix68k/", true, "")
}

func TestLogAndThermalAndAPI(t *testing.T) {
	f := newFake(t, ok("a\nb\nc\n"))
	code, out, _ := f.run("", nil, "log", "-n", "2")
	if code != 0 || out != "b\nc\n" {
		t.Fatalf("log: %d %q", code, out)
	}
	f.run("", nil, "thermal")
	f.run("", nil, "api")
	expect(t, f.reqs[0], "GET", "/log", false, "")
	expect(t, f.reqs[1], "GET", "/thermal", false, "")
	expect(t, f.reqs[2], "GET", "/", false, "")
}

func ppm(w, h int) []byte {
	var b bytes.Buffer
	fmt.Fprintf(&b, "P6\n%d %d\n255\n", w, h)
	for i := 0; i < w*h; i++ {
		b.Write([]byte{byte(i), 0x80, 0xff})
	}
	return b.Bytes()
}

func TestScreen(t *testing.T) {
	img := ppm(4, 3)
	f := newFake(t, func(w http.ResponseWriter, r *http.Request, _ []byte) {
		w.Header().Set("Content-Type", "image/x-portable-pixmap")
		w.Write(img)
	})
	dir := t.TempDir()
	out := filepath.Join(dir, "s.png")
	if code, _, errs := f.run("", nil, "screen", "-o", out); code != 0 {
		t.Fatal(code, errs)
	}
	expect(t, f.only(t), "GET", "/screen.ppm", false, "")
	fh, err := os.Open(out)
	if err != nil {
		t.Fatal(err)
	}
	defer fh.Close()
	p, err := png.Decode(fh)
	if err != nil {
		t.Fatal(err)
	}
	if p.Bounds().Dx() != 4 || p.Bounds().Dy() != 3 {
		t.Fatalf("大きさ %v", p.Bounds())
	}
	r, g, b, _ := p.At(1, 0).RGBA()
	if r>>8 != 1 || g>>8 != 0x80 || b>>8 != 0xff {
		t.Errorf("画素 %x %x %x", r>>8, g>>8, b>>8)
	}
	// .ppm ならそのまま
	raw := filepath.Join(dir, "s.ppm")
	f.run("", nil, "screen", "-o", raw)
	got, _ := os.ReadFile(raw)
	if !bytes.Equal(got, img) {
		t.Errorf("PPM が違う")
	}
}

func TestFilesLs(t *testing.T) {
	f := newFake(t, ok("pix68k/\t0\nkernel8.img\t1234\n"))
	code, out, _ := f.run("", nil, "files", "ls", "/pix68k dir")
	if code != 0 || !strings.Contains(out, "pix68k/") || !strings.Contains(out, "1234  kernel8.img") {
		t.Fatalf("%d %q", code, out)
	}
	expect(t, f.only(t), "GET", "/files/pix68k%20dir/", true, "")
}

func TestFilesGet(t *testing.T) {
	f := newFake(t, ok("cfg body\n"))
	dir := t.TempDir()
	out := filepath.Join(dir, "x.cfg")
	if code, _, errs := f.run("", nil, "files", "get", "/pix68k/pix68k.cfg", out); code != 0 {
		t.Fatal(code, errs)
	}
	expect(t, f.only(t), "GET", "/files/pix68k/pix68k.cfg", true, "")
	if b, _ := os.ReadFile(out); string(b) != "cfg body\n" {
		t.Errorf("中身 %q", b)
	}
	f.reqs = nil
	code, stdout, _ := f.run("", nil, "files", "get", "pix68k/pix68k.cfg", "-")
	if code != 0 || stdout != "cfg body\n" {
		t.Errorf("- : %d %q", code, stdout)
	}
}

// 本物のファイルで送り、Content-Length が付いて chunked にならないことを確かめる
func TestFilesPut(t *testing.T) {
	f := newFake(t, func(w http.ResponseWriter, r *http.Request, b []byte) {
		w.WriteHeader(201)
		fmt.Fprintf(w, "%d bytes written to %s\n", len(b), r.URL.Path)
	})
	dir := t.TempDir()
	local := filepath.Join(dir, "kernel8.img")
	data := bytes.Repeat([]byte{0, 1, 2, 0xff}, 50000)
	os.WriteFile(local, data, 0o644)
	if code, _, errs := f.run("", nil, "files", "put", local, "/"); code != 0 {
		t.Fatal(code, errs)
	}
	r := f.only(t)
	expect(t, r, "PUT", "/files/kernel8.img", true, string(data))
	if r.contentLength != int64(len(data)) {
		t.Errorf("Content-Length %d、%d のはず", r.contentLength, len(data))
	}
	// 空のファイル
	empty := filepath.Join(dir, "empty")
	os.WriteFile(empty, nil, 0o644)
	f.reqs = nil
	f.run("", nil, "files", "put", empty, "/pix68k/empty")
	r = f.only(t)
	expect(t, r, "PUT", "/files/pix68k/empty", true, "")
	if r.contentLength != 0 {
		t.Errorf("空のファイルの Content-Length %d", r.contentLength)
	}
	// 標準入力
	f.reqs = nil
	f.run("net=on\n", nil, "files", "put", "-", "/pix68k/a.cfg")
	r = f.only(t)
	expect(t, r, "PUT", "/files/pix68k/a.cfg", true, "net=on\n")
	if r.contentLength != 7 {
		t.Errorf("標準入力の Content-Length %d", r.contentLength)
	}
}

func TestFilesRm(t *testing.T) {
	f := newFake(t, ok("deleted\n"))
	f.run("", nil, "files", "rm", "/pix68k/old.cfg")
	expect(t, f.only(t), "DELETE", "/files/pix68k/old.cfg", true, "")
}

func TestExitCodes(t *testing.T) {
	for _, c := range []struct {
		status int
		body   string
		code   int
	}{
		{403, "bad token (X-Pix68k-Token, net_token=)\n", exitForbidden},
		{404, "no such file\n", exitNotFound},
		{409, "the X68000 is using this disk image\n", exitBusy},
		{503, "the settings menu is open; try again later\n", exitBusy},
		{400, "bad path\n", exitError},
		{500, "cannot delete\n", exitError},
	} {
		f := newFake(t, func(w http.ResponseWriter, r *http.Request, _ []byte) {
			w.WriteHeader(c.status)
			io.WriteString(w, c.body)
		})
		code, _, errs := f.run("", nil, "files", "rm", "/a")
		if code != c.code {
			t.Errorf("%d: 終了コード %d、%d のはず", c.status, code, c.code)
		}
		if !strings.Contains(errs, strings.TrimSpace(c.body)) || !strings.Contains(errs, fmt.Sprint(c.status)) {
			t.Errorf("%d: 文 %q に口の説明がない", c.status, errs)
		}
		if strings.Contains(errs, testToken) {
			t.Errorf("token を出した: %q", errs)
		}
	}
}

func TestConnectFailure(t *testing.T) {
	f := newFake(t, ok(""))
	addr := f.host()
	f.srv.Close()
	var out, errb bytes.Buffer
	dir := t.TempDir() // 手元の ~/.config を読まない
	a := &app{stdout: &out, stderr: &errb, stdin: strings.NewReader(""), getenv: func(k string) string {
		if k == "XDG_CONFIG_HOME" {
			return dir
		}
		return ""
	}}
	code := a.run(context.Background(), []string{"--host", addr, "status"})
	if code != exitConnect {
		t.Fatalf("code %d err %q", code, errb.String())
	}
}

func TestNoRedirectAndNoToken(t *testing.T) {
	var leaked bool
	other := httptest.NewServer(http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		if r.Header.Get("X-Pix68k-Token") != "" {
			leaked = true
		}
	}))
	defer other.Close()
	f := newFake(t, func(w http.ResponseWriter, r *http.Request, _ []byte) {
		http.Redirect(w, r, other.URL+"/x", http.StatusFound)
	})
	code, _, _ := f.run("", nil, "files", "ls")
	if code == 0 || leaked {
		t.Fatalf("転送について行った: code %d leaked %v", code, leaked)
	}
	// token がなければヘッダを付けない
	g := newFake(t, ok("x\n"))
	g.run("", map[string]string{"PIX68K_TOKEN": ""}, "files", "ls")
	if g.only(t).token != "" {
		t.Errorf("token なしで付けた")
	}
}

func TestVerboseHidesToken(t *testing.T) {
	f := newFake(t, ok("deleted\n"))
	_, _, errs := f.run("", nil, "-v", "files", "rm", "/a")
	if !strings.Contains(errs, "> DELETE /files/a") || strings.Contains(errs, testToken) {
		t.Fatalf("%q", errs)
	}
}

func TestStatus(t *testing.T) {
	f := newFake(t, func(w http.ResponseWriter, r *http.Request, _ []byte) {
		switch r.URL.Path {
		case "/":
			io.WriteString(w, helpNew)
		case "/files/":
			if r.Header.Get("X-Pix68k-Token") != testToken {
				w.WriteHeader(403)
				io.WriteString(w, "bad token (X-Pix68k-Token, net_token=)\n")
				return
			}
			io.WriteString(w, "pix68k/\t0\n")
		case "/thermal":
			io.WriteString(w, "temp_c=40.0\nclock_set_mhz=1400\n")
		case "/log":
			io.WriteString(w, "00:00:00.50 kernel: pix68k bare metal on Raspberry Pi 3 Model B+, compiled Nov 19 2026 12:00:00\n00:00:10.00 x68k: speed: 1 frames, emulated 10 s in 10 s (100% of real time)\n00:00:10.01 x68k: cpu: 1400 MHz set\n")
		}
	})
	code, out, _ := f.run("", nil, "status")
	for _, want := range []string{"つながる", "reboot-profile", "message", "syslog: off", "token: 通る", "temp_c=40.0", "x68k: speed:", "x68k: cpu:", "model: Raspberry Pi 3 Model B+"} {
		if !strings.Contains(out, want) {
			t.Errorf("status に %q がない:\n%s", want, out)
		}
	}
	if code != 0 {
		t.Errorf("code %d", code)
	}
	code, out, errs := f.run("", map[string]string{"PIX68K_TOKEN": "wrong-token-xyz"}, "status")
	if !strings.Contains(out, "token: 断られた") {
		t.Errorf("違う token:\n%s", out)
	}
	if strings.Contains(out+errs, "wrong-token-xyz") {
		t.Errorf("token の値を出した:\n%s%s", out, errs)
	}
	if code != exitForbidden {
		t.Errorf("違う token の終了コード %d、%d のはず", code, exitForbidden)
	}
}

func TestConfigFile(t *testing.T) {
	f := newFake(t, ok("x\n"))
	dir := t.TempDir()
	tok := filepath.Join(dir, "pi3.token")
	os.WriteFile(tok, []byte(testToken+"\n"), 0o600)
	cfg := filepath.Join(dir, "config.json")
	os.WriteFile(cfg, []byte(fmt.Sprintf(`{"default":"pi3","hosts":{"pi3":{"host":%q,"token_file":%q}}}`, f.host(), tok)), 0o600)
	var out, errb bytes.Buffer
	env := map[string]string{}
	a := &app{stdout: &out, stderr: &errb, stdin: strings.NewReader(""), getenv: func(k string) string { return env[k] }}
	// default を使う
	if code := a.run(context.Background(), []string{"--config", cfg, "files", "ls"}); code != 0 {
		t.Fatal(code, errb.String())
	}
	// 名前で選ぶ
	if code := a.run(context.Background(), []string{"--config", cfg, "--host", "pi3", "files", "rm", "/a"}); code != 0 {
		t.Fatal(code, errb.String())
	}
	// アドレスで選んでも、同じ host の項目の token を使う
	if code := a.run(context.Background(), []string{"--config", cfg, "--host", f.host(), "files", "rm", "/b"}); code != 0 {
		t.Fatal(code, errb.String())
	}
	for _, r := range f.reqs {
		if r.token != testToken {
			t.Errorf("%s %s: token %q", r.method, r.uri, r.token)
		}
	}
	// PIX68K_TOKEN が設定ファイルより優先
	env["PIX68K_TOKEN"] = "envtok"
	f.reqs = nil
	a.run(context.Background(), []string{"--config", cfg, "files", "rm", "/c"})
	if f.only(t).token != "envtok" {
		t.Errorf("PIX68K_TOKEN が優先されない")
	}
	// config は token の値を出さない
	out.Reset()
	a.run(context.Background(), []string{"--config", cfg, "config"})
	if strings.Contains(out.String(), "envtok") || !strings.Contains(out.String(), "PIX68K_TOKEN") {
		t.Errorf("config: %q", out.String())
	}
	// 知らない項目は誤り
	bad := filepath.Join(dir, "bad.json")
	os.WriteFile(bad, []byte(`{"hosts":{"a":{"hots":"x"}}}`), 0o600)
	if code := a.run(context.Background(), []string{"--config", bad, "--host", "a", "status"}); code != exitUsage {
		t.Errorf("知らない項目を通した: %d", code)
	}
	// 指定した設定ファイルがなければ誤り
	if code := a.run(context.Background(), []string{"--config", filepath.Join(dir, "nope.json"), "--host", "x", "status"}); code != exitUsage {
		t.Errorf("ない設定ファイルを通した: %d", code)
	}
}

func TestNoHost(t *testing.T) {
	var out, errb bytes.Buffer
	dir := t.TempDir()
	a := &app{stdout: &out, stderr: &errb, stdin: strings.NewReader(""), getenv: func(k string) string {
		if k == "PIX68KCLI_CONFIG" {
			return ""
		}
		if k == "XDG_CONFIG_HOME" {
			return dir
		}
		return ""
	}}
	if code := a.run(context.Background(), []string{"status"}); code != exitUsage {
		t.Fatalf("code %d", code)
	}
}

func TestLogDelta(t *testing.T) {
	prev := []byte("line1\nline2\n")
	d, r := logDelta(prev, []byte("line1\nline2\nline3\n"))
	if r || string(d) != "line3\n" {
		t.Errorf("増えた分 %q %v", d, r)
	}
	d, r = logDelta(prev, prev)
	if r || len(d) != 0 {
		t.Errorf("変わらない %q %v", d, r)
	}
	// リングの頭が捨てられた
	var long bytes.Buffer
	for i := 0; i < 200; i++ {
		fmt.Fprintf(&long, "line %d\n", i)
	}
	lp := long.Bytes()
	d, r = logDelta(lp, append(append([]byte{}, lp[300:]...), "new\n"...))
	if r || string(d) != "new\n" {
		t.Errorf("頭を捨てた %q %v", d, r)
	}
	// 再起動
	d, r = logDelta(prev, []byte("boot\n"))
	if !r || string(d) != "boot\n" {
		t.Errorf("再起動 %q %v", d, r)
	}
	// 同じ行がくり返す: 前の長さより後ろの一致は使わない
	big := []byte(strings.Repeat("x\n", 600))
	d, r = logDelta(big, append(append([]byte{}, big...), big...))
	if r || len(d) != len(big) {
		t.Errorf("くり返し %d %v", len(d), r)
	}
}

func TestDropLogPolls(t *testing.T) {
	in := "00:00:01.00 x68k: a\n00:00:01.10 httpd: 10.0.0.2 GET /log: 200 (100 bytes, 3 ms)\n00:00:01.20 httpd: 10.0.0.2 GET /thermal: 200 (0 bytes, 2 ms)\n"
	want := "00:00:01.00 x68k: a\n00:00:01.20 httpd: 10.0.0.2 GET /thermal: 200 (0 bytes, 2 ms)\n"
	if got := string(dropLogPolls([]byte(in))); got != want {
		t.Errorf("%q", got)
	}
}

func TestTrimPartialHeadAndLastLines(t *testing.T) {
	full := append([]byte("partial\n"), bytes.Repeat([]byte("a"), logRingSize)...)
	if bytes.HasPrefix(trimPartialHead(full), []byte("partial")) {
		t.Error("一杯なのに頭の行を捨てない")
	}
	if string(trimPartialHead([]byte("x\ny\n"))) != "x\ny\n" {
		t.Error("一杯でないのに捨てた")
	}
	if string(lastLines([]byte("a\nb\nc"), 2)) != "b\nc" {
		t.Error("lastLines")
	}
	if string(lastLines([]byte("a\n"), 5)) != "a\n" {
		t.Error("lastLines 足りない")
	}
}

func TestDecodePPMErrors(t *testing.T) {
	for _, b := range [][]byte{[]byte("P3\n1 1\n255\n"), []byte("P6\n2 2\n255\nabc"), []byte("P6\n1 1\n65535\n")} {
		if _, err := decodePPM(b); err == nil {
			t.Errorf("%q を通した", b)
		}
	}
	if _, err := decodePPM([]byte("P6\n# comment\n1 1\n255\n\x01\x02\x03")); err != nil {
		t.Error(err)
	}
}

func TestFilesPath(t *testing.T) {
	for in, want := range map[string]string{
		"/":                 "/files/",
		"pix68k/pix68k.cfg": "/files/pix68k/pix68k.cfg",
		"/a b/c#d?.cfg":     "/files/a%20b/c%23d%3F.cfg",
		`pix68k\x.cfg`:      "/files/pix68k/x.cfg",
	} {
		if got := filesPath(in); got != want {
			t.Errorf("filesPath(%q) = %q、%q のはず", in, got, want)
		}
	}
}

// log -f は、最初につながらなくても待って読み直す
func TestLogFollowStartsWhileDown(t *testing.T) {
	var mu sync.Mutex
	n := 0
	f := newFake(t, func(w http.ResponseWriter, r *http.Request, _ []byte) {
		mu.Lock()
		n++
		k := n
		mu.Unlock()
		if k <= 2 {
			w.WriteHeader(503)
			io.WriteString(w, "booting\n")
			return
		}
		io.WriteString(w, "a\nb\n")
	})
	ctx, cancel := context.WithTimeout(context.Background(), 1500*time.Millisecond)
	defer cancel()
	code, out, errs := f.runCtx(ctx, "", nil, "log", "-f", "--interval", "0.2")
	if code != 0 || out != "a\nb\n" || !strings.Contains(errs, "待ち続ける") || !strings.Contains(errs, "つながった") {
		t.Fatalf("code %d out %q err %q", code, out, errs)
	}
	// -f がなければ、最初の失敗でそのまま終える
	g := newFake(t, func(w http.ResponseWriter, r *http.Request, _ []byte) { w.WriteHeader(503) })
	if code, _, _ := g.run("", nil, "log"); code != exitBusy {
		t.Errorf("-f なしの失敗 %d", code)
	}
}

func TestLogBadInterval(t *testing.T) {
	f := newFake(t, ok("x\n"))
	for _, v := range []string{"NaN", "+Inf", "1e300", "0.1", "-1"} {
		if code, _, _ := f.run("", nil, "log", "-f", "--interval", v); code != exitUsage {
			t.Errorf("--interval %s: %d", v, code)
		}
	}
	if len(f.reqs) != 0 {
		t.Errorf("要求を送った")
	}
}

// 受け取る途中で切れたら「つながらない」(3) にし、手元のファイルは作らない
func TestFilesGetTruncated(t *testing.T) {
	f := newFake(t, func(w http.ResponseWriter, r *http.Request, _ []byte) {
		w.Header().Set("Content-Type", "application/octet-stream")
		w.Header().Set("Content-Length", "1000")
		w.WriteHeader(200)
		io.WriteString(w, "short")
		w.(http.Flusher).Flush()
		conn, _, err := w.(http.Hijacker).Hijack()
		if err == nil {
			conn.Close()
		}
	})
	dir := t.TempDir()
	out := filepath.Join(dir, "x.cfg")
	if code, _, errs := f.run("", nil, "files", "get", "/x.cfg", out); code != exitConnect {
		t.Errorf("ファイルへ: code %d err %q", code, errs)
	}
	if ents, _ := os.ReadDir(dir); len(ents) != 0 {
		t.Errorf("ファイルが残った: %v", ents)
	}
	if code, _, errs := f.run("", nil, "files", "get", "/x.cfg", "-"); code != exitConnect {
		t.Errorf("標準出力へ: code %d err %q", code, errs)
	}
}

// 手元に書けないときは「そのほか」(1)
func TestFilesGetLocalWriteError(t *testing.T) {
	f := newFake(t, ok("body\n"))
	out := filepath.Join(t.TempDir(), "nodir", "x.cfg")
	if code, _, _ := f.run("", nil, "files", "get", "/x.cfg", out); code != exitError {
		t.Errorf("code %d", code)
	}
}

// 上書きしても元の権限を広げない
func TestFilesGetKeepsMode(t *testing.T) {
	if runtime.GOOS == "windows" {
		t.Skip("権限のビットがない")
	}
	f := newFake(t, ok("net_token=x\n"))
	out := filepath.Join(t.TempDir(), "pix68k.cfg")
	os.WriteFile(out, []byte("old\n"), 0o600)
	os.Chmod(out, 0o600)
	if code, _, errs := f.run("", nil, "files", "get", "/pix68k/pix68k.cfg", out); code != 0 {
		t.Fatal(code, errs)
	}
	st, _ := os.Stat(out)
	if st.Mode().Perm() != 0o600 {
		t.Errorf("権限 %o、600 のはず", st.Mode().Perm())
	}
	if b, _ := os.ReadFile(out); string(b) != "net_token=x\n" {
		t.Errorf("中身 %q", b)
	}
}

func TestDecodePPMHuge(t *testing.T) {
	for _, h := range []string{"P6\n65535 65535\n255\n", "P6\n8193 1\n255\n", "P6\n1 1\n255"} {
		if _, err := decodePPM([]byte(h)); err == nil {
			t.Errorf("%q を通した", h)
		}
	}
}

// token の優先順: --token-file > PIX68K_TOKEN > PIX68K_TOKEN_FILE > 設定ファイルの token_file > token
func TestTokenPriority(t *testing.T) {
	f := newFake(t, ok("x\n"))
	dir := t.TempDir()
	write := func(name, v string) string {
		p := filepath.Join(dir, name)
		os.WriteFile(p, []byte(v+"\n"), 0o600)
		return p
	}
	flagF, envF, cfgF := write("flag", "from-flag"), write("env", "from-envfile"), write("cfg", "from-cfgfile")
	cfg := filepath.Join(dir, "config.json")
	os.WriteFile(cfg, []byte(fmt.Sprintf(`{"default":"p","hosts":{"p":{"host":%q,"token_file":%q,"token":"from-cfg"}}}`, f.host(), cfgF)), 0o600)
	cfg2 := filepath.Join(dir, "config2.json")
	os.WriteFile(cfg2, []byte(fmt.Sprintf(`{"default":"p","hosts":{"p":{"host":%q,"token":"from-cfg"}}}`, f.host())), 0o600)
	for _, c := range []struct {
		cfg  string
		env  map[string]string
		args []string
		want string
	}{
		{cfg, map[string]string{"PIX68K_TOKEN": "from-env", "PIX68K_TOKEN_FILE": envF}, []string{"--token-file", flagF}, "from-flag"},
		{cfg, map[string]string{"PIX68K_TOKEN": "from-env", "PIX68K_TOKEN_FILE": envF}, nil, "from-env"},
		{cfg, map[string]string{"PIX68K_TOKEN_FILE": envF}, nil, "from-envfile"},
		{cfg, map[string]string{}, nil, "from-cfgfile"},
		{cfg2, map[string]string{}, nil, "from-cfg"},
	} {
		var out, errb bytes.Buffer
		a := &app{stdout: &out, stderr: &errb, stdin: strings.NewReader(""), getenv: func(k string) string { return c.env[k] }}
		f.reqs = nil
		args := append(append([]string{"--config", c.cfg}, c.args...), "files", "ls")
		if code := a.run(context.Background(), args); code != 0 {
			t.Fatalf("%v: %d %s", args, code, errb.String())
		}
		if got := f.only(t).token; got != c.want {
			t.Errorf("%v %v: token %q、%q のはず", c.env, c.args, got, c.want)
		}
	}
	// 宛先: --host > PIX68K_HOST > default
	other := newFake(t, ok("y\n"))
	var out, errb bytes.Buffer
	a := &app{stdout: &out, stderr: &errb, stdin: strings.NewReader(""), getenv: func(k string) string {
		if k == "PIX68K_HOST" {
			return other.host()
		}
		return ""
	}}
	f.reqs = nil
	a.run(context.Background(), []string{"--config", cfg, "files", "ls"})
	if len(other.reqs) != 1 || len(f.reqs) != 0 {
		t.Errorf("PIX68K_HOST が default より優先されない")
	}
	a.run(context.Background(), []string{"--config", cfg, "--host", "p", "files", "ls"})
	if len(f.reqs) != 1 {
		t.Errorf("--host が PIX68K_HOST より優先されない")
	}
}

// net_token= のない口 (token なしでも /files を読める) では「通る」と言わない
func TestStatusNoTokenOnServer(t *testing.T) {
	f := newFake(t, ok("pix68k/\t0\n"))
	code, out, _ := f.run("", nil, "status")
	if code != 0 || strings.Contains(out, "token: 通る") || !strings.Contains(out, "token: 確かめていない") {
		t.Errorf("code %d\n%s", code, out)
	}
}

// 大きな PUT は Expect: 100-continue を付け、本体を読まずに断られたら 403 を受け取る
func TestFilesPutExpectContinue(t *testing.T) {
	var expect string
	f := newFake(t, nil)
	f.srv.Config.Handler = http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		expect = r.Header.Get("Expect")
		w.WriteHeader(403)
		io.WriteString(w, "bad token (X-Pix68k-Token, net_token=)\n")
	})
	local := filepath.Join(t.TempDir(), "big.bin")
	os.WriteFile(local, bytes.Repeat([]byte{1}, 8<<20), 0o644)
	for i := 0; i < 3; i++ {
		code, _, errs := f.run("", nil, "files", "put", local, "/big.bin")
		if code != exitForbidden {
			t.Fatalf("code %d err %q", code, errs)
		}
	}
	if expect != "100-continue" {
		t.Errorf("Expect %q", expect)
	}
}

// log -f は 403 や 404 では待たずに終える
func TestLogFollowStopsOnForbidden(t *testing.T) {
	f := newFake(t, func(w http.ResponseWriter, r *http.Request, _ []byte) {
		w.WriteHeader(403)
		io.WriteString(w, "not allowed from this address (net_allow=)\n")
	})
	ctx, cancel := context.WithTimeout(context.Background(), 3*time.Second)
	defer cancel()
	if code, _, _ := f.runCtx(ctx, "", nil, "log", "-f", "--interval", "0.2"); code != exitForbidden || ctx.Err() != nil {
		t.Errorf("code %d ctx %v", code, ctx.Err())
	}
}

// ファイルとディレクトリを取り違えない
func TestFilesTypeMismatch(t *testing.T) {
	file := newFake(t, func(w http.ResponseWriter, r *http.Request, _ []byte) {
		w.Header().Set("Content-Type", "application/octet-stream")
		w.Write([]byte{0, 1, 2})
	})
	if code, _, errs := file.run("", nil, "files", "ls", "/disk.hdf"); code != exitError || !strings.Contains(errs, "files get") {
		t.Errorf("ls にファイル: %d %q", code, errs)
	}
	dir := newFake(t, func(w http.ResponseWriter, r *http.Request, _ []byte) {
		w.Header().Set("Content-Type", "text/plain; charset=utf-8")
		io.WriteString(w, "a.cfg\t1\n")
	})
	d := t.TempDir()
	if code, _, errs := dir.run("", nil, "files", "get", "/pix68k", filepath.Join(d, "pix68k")); code != exitError || !strings.Contains(errs, "files ls") {
		t.Errorf("get にディレクトリ: %d %q", code, errs)
	}
	if ents, _ := os.ReadDir(d); len(ents) != 0 {
		t.Errorf("ファイルを作った: %v", ents)
	}
}

// 古いカーネルにない口 (thermal、screen) の 404 は 7
func TestUnsupportedGet(t *testing.T) {
	f := newFake(t, func(w http.ResponseWriter, r *http.Request, _ []byte) {
		w.WriteHeader(404)
		io.WriteString(w, "not found (GET / for the list)\n")
	})
	for _, args := range [][]string{{"thermal"}, {"screen", "-o", filepath.Join(t.TempDir(), "s.png")}} {
		if code, _, _ := f.run("", nil, args...); code != exitUnsupported {
			t.Errorf("%v: %d", args, code)
		}
	}
}

func TestScreenExtraArg(t *testing.T) {
	f := newFake(t, ok(""))
	if code, _, _ := f.run("", nil, "screen", "-o", "screen.png", "extra"); code != exitUsage || len(f.reqs) != 0 {
		t.Errorf("code %d", code)
	}
}

// - で始まる文字列は -- の後ろに書く
func TestTypeDoubleDash(t *testing.T) {
	f := newFake(t, ok("typed\n"))
	f.run("", nil, "type", "--", "-l\n")
	expect(t, f.only(t), "POST", "/type", true, "-l\n")
}

// 空白だけの PIX68K_TOKEN は、ないものとして設定ファイルの token を使う
func TestBlankEnvToken(t *testing.T) {
	f := newFake(t, ok("x\n"))
	dir := t.TempDir()
	cfg := filepath.Join(dir, "c.json")
	os.WriteFile(cfg, []byte(fmt.Sprintf(`{"default":"p","hosts":{"p":{"host":%q,"token":%q}}}`, f.host(), testToken)), 0o600)
	var out, errb bytes.Buffer
	a := &app{stdout: &out, stderr: &errb, stdin: strings.NewReader(""), getenv: func(k string) string {
		if k == "PIX68K_TOKEN" {
			return "  \n"
		}
		return ""
	}}
	if code := a.run(context.Background(), []string{"--config", cfg, "files", "rm", "/a"}); code != 0 {
		t.Fatal(code, errb.String())
	}
	if f.only(t).token != testToken {
		t.Errorf("token %q", f.reqs[0].token)
	}
}

func TestProfilesTruncated(t *testing.T) {
	f := newFake(t, ok("pix68k-a.cfg\t1\n...\n"))
	code, out, errs := f.run("", nil, "profiles")
	if code != 0 || out != "a\n" || !strings.Contains(errs, "途中で切った") {
		t.Errorf("code %d out %q err %q", code, out, errs)
	}
}

// 8193x1 の画素がそろっていても、上限を超えれば断る (64 ビットでも大きさの検査を確かめる)
func TestDecodePPMOverLimitWithPixels(t *testing.T) {
	_, err := decodePPM(ppm(8193, 1))
	if err == nil || !strings.Contains(err.Error(), "大きすぎる") {
		t.Errorf("%v", err)
	}
	if _, err := decodePPM(ppm(8192, 1)); err != nil {
		t.Errorf("8192 を断った: %v", err)
	}
}

// token ありで 200、token なしで 403 以外 (503 など) なら「通る」と言わない
func TestStatusTokenUnconfirmed(t *testing.T) {
	f := newFake(t, func(w http.ResponseWriter, r *http.Request, _ []byte) {
		switch {
		case r.URL.Path == "/files/" && r.Header.Get("X-Pix68k-Token") == "":
			w.WriteHeader(503)
			io.WriteString(w, "the settings menu is open; try again later\n")
		default:
			ok("x\n")(w, r, nil)
		}
	})
	_, out, _ := f.run("", nil, "status")
	if strings.Contains(out, "token: 通る") || !strings.Contains(out, "token: 確かめられない") {
		t.Errorf("%s", out)
	}
}

// log -f は、読めていたあとに 403 / 404 になったら終える
func TestLogFollowStopsAfterOK(t *testing.T) {
	for _, st := range []int{403, 404} {
		var mu sync.Mutex
		n := 0
		f := newFake(t, func(w http.ResponseWriter, r *http.Request, _ []byte) {
			mu.Lock()
			n++
			k := n
			mu.Unlock()
			if k == 1 {
				io.WriteString(w, "a\n")
				return
			}
			w.WriteHeader(st)
			io.WriteString(w, "no\n")
		})
		ctx, cancel := context.WithTimeout(context.Background(), 3*time.Second)
		code, out, _ := f.runCtx(ctx, "", nil, "log", "-f", "--interval", "0.2")
		if code != statusExit(st) || out != "a\n" || ctx.Err() != nil {
			t.Errorf("%d: code %d out %q ctx %v", st, code, out, ctx.Err())
		}
		cancel()
	}
}

// 受け取る途中で止められたら (SIGTERM や Ctrl+C は ctx の取り消しになる)、元のファイルを残し、一時ファイルを消す
func TestFilesGetCancelKeepsOriginal(t *testing.T) {
	started := make(chan struct{})
	release := make(chan struct{})
	f := newFake(t, func(w http.ResponseWriter, r *http.Request, _ []byte) {
		w.Header().Set("Content-Type", "application/octet-stream")
		w.Header().Set("Content-Length", "100000")
		w.WriteHeader(200)
		w.Write(bytes.Repeat([]byte{'x'}, 1000))
		w.(http.Flusher).Flush()
		close(started)
		select {
		case <-release:
		case <-r.Context().Done():
		}
	})
	defer close(release)
	dir := t.TempDir()
	out := filepath.Join(dir, "pix68k.cfg")
	os.WriteFile(out, []byte("original\n"), 0o600)
	// 一時ファイルができて、受け取った 1000 バイトが書かれたのを見てから取り消す
	created := make(chan string, 1)
	afterTempCreate = func(name string) { created <- name }
	defer func() { afterTempCreate = nil }()
	ctx, cancel := context.WithCancel(context.Background())
	defer cancel()
	var tmpName string
	go func() {
		defer cancel()
		<-started
		tmpName = <-created
		deadline := time.Now().Add(5 * time.Second)
		for time.Now().Before(deadline) {
			if st, err := os.Stat(tmpName); err == nil && st.Size() >= 1000 {
				return
			}
			time.Sleep(5 * time.Millisecond)
		}
		t.Error("一時ファイルに書かれない")
	}()
	code, _, _ := f.runCtx(ctx, "", nil, "files", "get", "/pix68k/pix68k.cfg", out)
	<-ctx.Done()
	if tmpName == "" {
		t.Fatal("一時ファイルを作っていない")
	}
	if _, err := os.Stat(tmpName); !os.IsNotExist(err) {
		t.Errorf("一時ファイル %s が残った (%v)", tmpName, err)
	}
	if code != exitConnect {
		t.Errorf("code %d", code)
	}
	if b, _ := os.ReadFile(out); string(b) != "original\n" {
		t.Errorf("元のファイルが変わった: %q", b)
	}
	ents, _ := os.ReadDir(dir)
	if len(ents) != 1 {
		t.Errorf("一時ファイルが残った: %v", ents)
	}
}
