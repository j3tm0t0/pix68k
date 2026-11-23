package main

import (
	"bytes"
	"context"
	"io"
	"net/http"
	"os"
	"path/filepath"
	"strings"
	"testing"
	"time"
)

func TestCompareVersions(t *testing.T) {
	for _, c := range []struct {
		from, to string
		want     int
	}{
		{"v0.2.3", "v0.2.4", orderNewer}, {"v0.2.9", "v0.2.10", orderNewer}, {"v0.3.0", "v0.2.9", orderOlder},
		{"v0.2.4-beta.1", "v0.2.4", orderNewer}, {"v0.2.4", "v0.2.4-beta.9", orderOlder},
		{"v0.2.4-beta.2", "v0.2.4-beta.10", orderNewer}, {"v0.2.4", "v0.2.4", orderSame},
		{"93d98d6e", "v0.2.4", orderUnknown}, {"v0.2.4", "nightly", orderUnknown}, {"", "v0.2.4", orderUnknown},
		{"v0.2", "v0.2.4", orderUnknown}, {"v0.2.4-rc1", "v0.2.4", orderUnknown},
	} {
		if got := compareVersions(c.from, c.to); got != c.want {
			t.Errorf("%s -> %s: %d", c.from, c.to, got)
		}
	}
}

func TestKernelPairs(t *testing.T) {
	for r, p := range kernelPairs {
		if len(p[0]) != len(p[1]) || p[0] != releaseKernels[r] || otherKernel(r, p[0]) != p[1] || otherKernel(r, p[1]) != p[0] {
			t.Error(r, p)
		}
	}
	if otherKernel("9", "") != "" || otherKernel("4", "kernel8.img") != "" {
		t.Error("知らない名前")
	}
}

func TestTrybootTableEmpty(t *testing.T) {
	// tryboot を使う処理がないうちは、機種を足さない
	if len(trybootVerified) != 0 {
		t.Error(trybootVerified)
	}
}

// Pi の偽物。ファイルと、動いている版を持つ
type fakePi struct {
	files           map[string][]byte
	version, commit string
	boots           int
	fail            string // この要求 ("PUT /files/…" など) を 500 (failStatus) にする
	failStatus      int
	failBody        string
	drive           string
	corrupt         string // この PUT の中身を壊して置く
	breakSwitch     bool   // config.txt を書き損じる
	noBoot          bool   // 再起動しても古い版のまま
	noEndpoint      bool
	ops             []string
}

func (p *fakePi) handle(w http.ResponseWriter, r *http.Request, body []byte) {
	op := r.Method + " " + r.URL.Path
	if r.Method != "GET" {
		p.ops = append(p.ops, op)
	}
	if op == p.fail {
		st := p.failStatus
		if st == 0 {
			st = 500
		}
		w.WriteHeader(st)
		io.WriteString(w, p.failBody)
		return
	}
	switch {
	case r.URL.Path == "/":
		if !p.noEndpoint {
			io.WriteString(w, "POST /config/kernel?from=A\n")
		}
	case r.URL.Path == "/version":
		io.WriteString(w, "version="+p.version+"\ncommit="+p.commit+"\nraspi=4\nmodel=Raspberry Pi 400\ndrive="+p.drive+"\n")
	case strings.HasPrefix(r.URL.Path, "/files/"):
		n := strings.TrimPrefix(r.URL.Path, "/files/")
		if r.Method == "PUT" {
			if (r.URL.Query().Get("unused") == "1") != strings.HasSuffix(n, ".img") {
				w.WriteHeader(400)
				return
			}
			if n == p.corrupt {
				body = append([]byte("x"), body...)
			}
			p.files[n] = body
			w.WriteHeader(201)
			return
		}
		b, ok := p.files[n]
		if !ok {
			http.NotFound(w, r)
			return
		}
		w.Write(b)
	case strings.HasPrefix(r.URL.Path, "/sha256/"):
		b, ok := p.files[strings.TrimPrefix(r.URL.Path, "/sha256/")]
		if !ok {
			http.NotFound(w, r)
			return
		}
		io.WriteString(w, "sha256="+sha256Hex(b)+"\n")
	case op == "POST /config/kernel":
		q := r.URL.Query()
		cfg := string(p.files["config.txt"])
		if sha256Hex(p.files[q.Get("to")]) != q.Get("sha256") || !strings.Contains(cfg, "kernel="+q.Get("from")) {
			w.WriteHeader(409)
			return
		}
		cfg = strings.Replace(cfg, "kernel="+q.Get("from"), "kernel="+q.Get("to"), 1)
		if p.breakSwitch {
			cfg = cfg[1:]
		}
		p.files["config.txt"] = []byte(cfg)
	case op == "POST /reboot":
		p.boots++
		if p.noBoot {
			return
		}
		name, _, _, _ := findKernel(string(p.files["config.txt"]), modelSections("4", "Raspberry Pi 400"))
		if m := reKernelVersion.FindSubmatch(p.files[name]); m != nil {
			p.version, p.commit = string(m[1]), string(m[2])
		}
	default:
		http.NotFound(w, r)
	}
}

func TestUpdateApply(t *testing.T) {
	const cfg = releaseConfig
	oldK := []byte("version v0.2.4, commit abc1234\x00 old /config/kernel")
	newK := []byte("version v0.3.0, commit def5678\x00 new /config/kernel")
	z := makeZip(t, map[string][]byte{"kernel8-rpi4.img": newK})
	no := []string{"--no-tryboot"}
	for _, c := range []struct {
		name     string
		args     []string
		set      func(p *fakePi)
		code     int
		want     string
		switched bool
		writes   bool
	}{
		{"更新", no, nil, 0, "v0.3.0 が動いている", true, true},
		{"既定は断る", nil, nil, exitError, "--no-tryboot", false, false},
		{"分からない版", no, func(p *fakePi) {
			p.version = "abc1234"
			p.files["kernel8-rpi4.img"] = []byte("version abc1234, commit abc1234")
		}, exitError, "--force", false, false},
		{"古くなる", no, func(p *fakePi) {
			p.version = "v0.4.0"
			p.files["kernel8-rpi4.img"] = []byte("version v0.4.0, commit abc1234")
		}, exitError, "--allow-downgrade", false, false},
		{"古くする", []string{"--no-tryboot", "--allow-downgrade"}, func(p *fakePi) {
			p.version = "v0.4.0"
			p.files["kernel8-rpi4.img"] = []byte("version v0.4.0, commit abc1234")
		}, 0, "動いている", true, true},
		{"LAN 起動", no, func(p *fakePi) { p.commit = "zzz" }, exitError, "何も書いていない", false, false},
		{"口がない", no, func(p *fakePi) { p.noEndpoint = true }, exitUnsupported, "/config/kernel", false, false},
		{"知らない名前", no, func(p *fakePi) {
			p.files["config.txt"] = []byte("kernel=my.img\n")
			p.files["my.img"] = oldK
		}, exitError, "入れ替える名前", false, false},
		{"同じ版", no, func(p *fakePi) { p.files["kernel8-rpi4.img"] = newK; p.version, p.commit = "v0.3.0", "def5678" }, 0, "同じカーネル", false, false},
		{"同じ版で中身が違う", []string{"--no-tryboot", "--force"}, func(p *fakePi) {
			p.files["kernel8-rpi4.img"] = append([]byte("x"), newK...)
			p.version, p.commit = "v0.3.0", "def5678"
		}, exitError, "確かめられない。何も", false, false},
		{"PUT の失敗", no, func(p *fakePi) { p.fail = "PUT /files/pix68k8-rpi4.img" }, exitError, "替えていない", false, true},
		{"PUT の壊れ", no, func(p *fakePi) { p.corrupt = "pix68k8-rpi4.img" }, exitError, "SHA-256 が違う", false, true},
		{"update.txt の失敗", no, func(p *fakePi) { p.fail = "PUT /files/pix68k/update.txt" }, exitError, "替えていない", false, true},
		{"切り替えの失敗", no, func(p *fakePi) { p.fail = "POST /config/kernel" }, exitError, "再起動も電源を切ることもせず", false, true},
		{"切り替えの前に断られた (409)", no, func(p *fakePi) { p.fail, p.failStatus = "POST /config/kernel", 409 }, exitBusy, "替えていない", false, true},
		{"切り替えの前に断られた (503)", no, func(p *fakePi) { p.fail, p.failStatus = "POST /config/kernel", 503 }, exitBusy, "替えていない", false, true},
		{"切り替えで読めない", no, func(p *fakePi) {
			p.fail, p.failBody = "POST /config/kernel", "cannot read the sector; not written\n"
		}, exitError, "替えていない", false, true},
		{"切り替えの書き込みの失敗", no, func(p *fakePi) {
			p.fail, p.failBody = "POST /config/kernel", "WRITE ERROR: check config.txt before rebooting\n"
		}, exitError, "再起動も電源を切ることもせず", false, true},
		{"kernel= がセクタをまたぐ", no, func(p *fakePi) {
			p.files["config.txt"] = []byte("#" + strings.Repeat("x", 494) + "\n[pi4]\nkernel=kernel8-rpi4.img\n")
		}, exitError, "3 バイト (半角の文字) 増やす", false, false},
		{"切り替えの書き損じ", no, func(p *fakePi) { p.breakSwitch = true }, exitError, "再起動も電源を切ることもせず", true, true},
		{"起動しない", no, func(p *fakePi) { p.noBoot = true }, exitConnect, "確かめられない", true, true},
	} {
		t.Run(c.name, func(t *testing.T) {
			p := &fakePi{drive: "SD:", version: "v0.2.4", commit: "abc1234", files: map[string][]byte{"config.txt": []byte(cfg), "kernel8-rpi4.img": oldK, "pix68k8-rpi4.img": []byte("older")}}
			if c.set != nil {
				c.set(p)
			}
			before := string(p.files["config.txt"])
			wasOld := string(p.files["kernel8-rpi4.img"])
			f := newFake(t, p.handle)
			dir := t.TempDir()
			run := func(args ...string) (int, string) {
				var out bytes.Buffer
				env := map[string]string{"PIX68K_TOKEN": testToken, "XDG_CONFIG_HOME": t.TempDir()}
				a := &app{stdout: &out, stderr: &out, stdin: strings.NewReader(""), getenv: func(k string) string { return env[k] },
					releases: fakeRelease(t, z, sha256Hex(z)), updateWait: 50 * time.Millisecond, updateInterval: time.Millisecond, backupDir: dir}
				code := a.run(context.Background(), append([]string{"--host", f.host(), "update"}, args...))
				return code, out.String()
			}
			code, out := run(c.args...)
			if code != c.code || !strings.Contains(out, c.want) || strings.Contains(out, testToken) {
				t.Errorf("code %d\n%s", code, out)
			}
			if (string(p.files["config.txt"]) != before) != c.switched {
				t.Errorf("config.txt: %q", p.files["config.txt"])
			}
			if (len(p.ops) > 0) != c.writes {
				t.Error(p.ops)
			}
			if string(p.files["kernel8-rpi4.img"]) != wasOld {
				t.Error("今のカーネルを書き換えた")
			}
			if code != 0 && code != exitConnect && p.boots != 0 {
				t.Error("失敗したのに再起動した")
			}
			if c.writes {
				g, _ := filepath.Glob(filepath.Join(dir, "pix68k-config-*.txt"))
				if len(g) != 1 {
					t.Fatal(g)
				}
				if b, _ := os.ReadFile(g[0]); string(b) != before {
					t.Error("控えが違う")
				}
			}
			if c.name == "起動しない" {
				n := p.boots
				if code, out := run("--rollback"); code != 0 || p.boots != n || !strings.Contains(out, "再起動していない") || string(p.files["config.txt"]) != before {
					t.Error(code, out)
				}
			}
			if c.name != "更新" {
				return
			}
			if got := strings.Join(p.ops, ","); got != "PUT /files/pix68k8-rpi4.img,PUT /files/pix68k/update.txt,POST /config/kernel,POST /reboot" {
				t.Error(got)
			}
			// 前の版が変わっていれば戻さない
			p.files["kernel8-rpi4.img"] = []byte("changed")
			if code, out := run("--rollback"); code != exitError || !strings.Contains(out, "違う") {
				t.Error(code, out)
			}
			p.files["kernel8-rpi4.img"] = oldK
			// LAN から起動したほかの版では戻さない
			p.version = "v0.2.9"
			if code, out := run("--rollback"); code != exitError || !strings.Contains(out, "ではない") {
				t.Error(code, out)
			}
			p.version, p.drive = "v0.3.0", "USB:"
			if code, out := run("--rollback"); code != exitError || !strings.Contains(out, "SD カードから起動") {
				t.Error(code, out)
			}
			p.drive, p.noEndpoint = "", true
			if code, out := run("--rollback"); code != exitUnsupported {
				t.Error(code, out)
			}
			p.drive, p.noEndpoint = "SD:", false
			if code, out := run("--rollback"); code != 0 || p.version != "v0.2.4" || string(p.files["config.txt"]) != before {
				t.Error(code, out)
			}
			if code, out := run("--rollback"); code != exitError || !strings.Contains(out, "戻せる版がない") {
				t.Error(code, out)
			}
		})
	}
}

// /version のない古いカーネルにしたときは、動いたかを確かめられないので 0 で終えない
func TestUpdateToOldKernel(t *testing.T) {
	oldK := []byte("version v0.4.0, commit abc1234")
	newK := []byte("no version")
	z := makeZip(t, map[string][]byte{"kernel8-rpi4.img": newK})
	p := &fakePi{drive: "SD:", version: "v0.4.0", commit: "abc1234", files: map[string][]byte{"config.txt": []byte(releaseConfig), "kernel8-rpi4.img": oldK}}
	f := newFake(t, p.handle)
	var out bytes.Buffer
	env := map[string]string{"PIX68K_TOKEN": testToken, "XDG_CONFIG_HOME": t.TempDir()}
	a := &app{stdout: &out, stderr: &out, stdin: strings.NewReader(""), getenv: func(k string) string { return env[k] },
		releases: fakeRelease(t, z, sha256Hex(z)), updateWait: 50 * time.Millisecond, updateInterval: time.Millisecond, backupDir: t.TempDir()}
	code := a.run(context.Background(), []string{"--host", f.host(), "update", "--no-tryboot", "--allow-downgrade"})
	if code != exitConnect || p.boots != 1 || !strings.Contains(out.String(), "この命令で戻せない") {
		t.Error(code, p.boots, out.String())
	}
}
