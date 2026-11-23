// pix68kcli: pix68k のベアメタル版の HTTP の口 (docs/network-api.md) を操作する。
package main

import (
	"context"
	"flag"
	"fmt"
	"io"
	"os"
	"os/signal"
	"strings"
	"syscall"
	"time"
)

// ビルドのときに -ldflags "-X main.version=..." で書き換える
var version = "dev"

const usageText = `使い方: pix68kcli [全体のオプション] <命令> [引数]

命令:
  status                      つながるか、口の一覧、token が通るか、温度とクロック、Pi の型、速度
  log [-f] [-n 行] [--interval 秒]
                              ログ (-f で追いかける)
  screen [-o ファイル]        画面を PNG で保存 (既定 screen.png。.ppm なら PPM のまま、- なら標準出力)
  thermal                     温度、クロック、ファン (/thermal をそのまま)
  reboot [--profile 名前]     ディスクと SRAM を書いてから再起動 (--profile は pix68k-<名前>.cfg で起動)
  profiles                    起動の組み合わせ (/pix68k/pix68k-*.cfg) の一覧
  type [文字列... | -]        X68000 のキーとして打つ (\n、\t、\b、\e は口が読む。- なら標準入力)
  key down [--mod YY] XX      USB の usage ID (16 進) のキーを押したままにする
  key up [XX]                 キーを離す (XX がなければすべて)
  message [--sec N] [文字列... | -]
                              画面の左上に N 秒出す (既定 10、0 なら消すまで)
  message --clear             メッセージを消す
  files ls [パス]             SD カードの一覧 (既定 /)
  files get パス [手元 | -]   読む (手元のファイル名の既定はパスの最後の名前)
  files put 手元 パス         書く (パスが / で終われば手元の名前を付ける)
  files rm パス               消す
  fuk-read [--drive N] ファイル
                              FUK のドライブ N (既定 0) のディスク (2HD) を XDF に読み出す (読む間 X68000 は止まる)
  update --check [--tag 版]   今のカーネルと、配布している版 (既定は最新) を比べる (書き換えはしない)
  update [--tag 版] --no-tryboot [--allow-downgrade] [--force]
                              カーネルを入れ替えて再起動する (README の「更新」を読んでから)
  update --rollback           更新の前の版に戻す
  api                         口の使い方の一覧 (GET /) をそのまま
  config                      決まった宛先と token の出どころ (token の値は出さない)
  version                     この版

全体のオプション:
  --host 名前|アドレス        宛先 (設定ファイルの名前か、アドレス。PIX68K_HOST)
  --config ファイル           設定ファイル (PIX68KCLI_CONFIG。既定 ~/.config/pix68kcli/config.json)
  --token-file ファイル       token を読むファイル (PIX68K_TOKEN、PIX68K_TOKEN_FILE も使える)
  -v                          送った要求と応答の状態を標準エラーに出す (token の値は出さない)

終了コード: 0 成功、1 そのほかの失敗、2 使い方の誤り、3 つながらない、4 断られた (403)、
            5 ない (404)、6 使用中・動いていない (409、503)、7 カーネルにその口がない
`

type app struct {
	stdout   io.Writer
	stderr   io.Writer
	stdin    io.Reader
	getenv   func(string) string
	releases *releaseSource // 試験用 (nil なら GitHub)
	// 試験用 (0 なら既定)
	updateWait, updateInterval time.Duration
	backupDir                  string // config.txt の控えを書く所 (空なら今のディレクトリ)
}

func main() {
	ctx, stop := signal.NotifyContext(context.Background(), os.Interrupt, syscall.SIGTERM)
	defer stop()
	a := &app{stdout: os.Stdout, stderr: os.Stderr, stdin: os.Stdin, getenv: os.Getenv}
	os.Exit(a.run(ctx, os.Args[1:]))
}

func (a *app) run(ctx context.Context, args []string) int {
	err := a.runErr(ctx, args)
	if err == nil {
		return exitOK
	}
	if err == flag.ErrHelp {
		return exitOK
	}
	fmt.Fprintln(a.stderr, "pix68kcli: "+err.Error())
	return exitCode(err)
}

func (a *app) runErr(ctx context.Context, args []string) error {
	fs := flag.NewFlagSet("pix68kcli", flag.ContinueOnError)
	fs.SetOutput(io.Discard)
	host := fs.String("host", "", "")
	cfgPath := fs.String("config", "", "")
	tokenFile := fs.String("token-file", "", "")
	verbose := fs.Bool("v", false, "")
	help := fs.Bool("h", false, "")
	fs.BoolVar(help, "help", false, "")
	if err := fs.Parse(args); err != nil {
		return usageErr(err.Error() + " (pix68kcli -h で使い方)")
	}
	rest := fs.Args()
	if *help || len(rest) == 0 || rest[0] == "help" {
		fmt.Fprint(a.stdout, usageText)
		if len(rest) == 0 && !*help {
			return &cliError{exitUsage, "命令がない"}
		}
		return nil
	}
	cmd, cargs := rest[0], rest[1:]
	if cmd == "version" {
		fmt.Fprintln(a.stdout, "pix68kcli "+version)
		return nil
	}
	explicit := *cfgPath != ""
	path := *cfgPath
	if path == "" {
		path = defaultConfigPath(a.getenv)
		explicit = a.getenv("PIX68KCLI_CONFIG") != ""
	}
	cfg, err := loadConfig(expandHome(path), explicit)
	if err != nil {
		return &cliError{exitUsage, err.Error()}
	}
	t, err := resolveTarget(cfg, resolveOpts{host: *host, tokenFile: *tokenFile, getenv: a.getenv})
	if err != nil {
		return err
	}
	var vw io.Writer
	if *verbose {
		vw = a.stderr
	}
	c := newClient(t, vw)
	switch cmd {
	case "status":
		return a.cmdStatus(ctx, c, cargs)
	case "log":
		return a.cmdLog(ctx, c, cargs)
	case "screen":
		return a.cmdScreen(ctx, c, cargs)
	case "thermal":
		return a.cmdSimpleGet(ctx, c, cargs, "/thermal")
	case "api":
		return a.cmdSimpleGet(ctx, c, cargs, "/")
	case "reboot":
		return a.cmdReboot(ctx, c, cargs)
	case "profiles":
		return a.cmdProfiles(ctx, c, cargs)
	case "type":
		return a.cmdType(ctx, c, cargs)
	case "key":
		return a.cmdKey(ctx, c, cargs)
	case "message", "msg":
		return a.cmdMessage(ctx, c, cargs)
	case "files":
		return a.cmdFiles(ctx, c, cargs)
	case "update":
		return a.cmdUpdate(ctx, c, cargs)
	case "fuk-read":
		return a.cmdFukRead(ctx, c, cargs)
	case "config":
		return a.cmdConfig(t, path)
	}
	return usageErr(fmt.Sprintf("知らない命令 %q (pix68kcli -h で使い方)", cmd))
}

func (a *app) cmdConfig(t *Target, path string) error {
	fmt.Fprintf(a.stdout, "config: %s\n", path)
	if t.Name != "" {
		fmt.Fprintf(a.stdout, "name: %s\n", t.Name)
	}
	fmt.Fprintf(a.stdout, "host: %s\n", t.Host)
	if t.Token != "" {
		fmt.Fprintf(a.stdout, "token: %s から (%d 文字)\n", t.TokenSrc, len(t.Token))
	} else {
		fmt.Fprintln(a.stdout, "token: なし")
	}
	return nil
}

// 命令ごとのオプションを読む。fs.Parse は最初の引数で止まるので、オプションは引数より前に書く
func parseFlags(fs *flag.FlagSet, args []string) error {
	fs.SetOutput(io.Discard)
	if err := fs.Parse(args); err != nil {
		if err == flag.ErrHelp {
			return usageErr(fs.Name() + ": 使い方は pix68kcli -h")
		}
		return usageErr(fs.Name() + ": " + err.Error())
	}
	return nil
}

func joinArgs(a *app, args []string) (string, error) {
	if len(args) == 1 && args[0] == "-" {
		b, err := io.ReadAll(a.stdin)
		if err != nil {
			return "", err
		}
		return string(b), nil
	}
	return strings.Join(args, " "), nil
}
