package main

import (
	"context"
	"errors"
	"fmt"
	"io"
	"net"
	"net/http"
	"net/url"
	"os"
	"strings"
	"time"
)

// 終了コード
const (
	exitOK          = 0
	exitError       = 1 // ほかの失敗 (手元のファイル、口の 400 / 500 など)
	exitUsage       = 2 // 使い方の誤り、設定ファイルの誤り
	exitConnect     = 3 // つながらない、時間切れ
	exitForbidden   = 4 // 403 (token が違う、net_allow=、Host:)
	exitNotFound    = 5 // 404
	exitBusy        = 6 // 409、503 (使っているディスクのイメージ、設定メニュー、X68000 が動いていない)
	exitUnsupported = 7 // 動いているカーネルにその口がない
)

// 終了コードを持つ誤り
type cliError struct {
	code int
	msg  string
}

func (e *cliError) Error() string { return e.msg }

func usageErr(msg string) error { return &cliError{exitUsage, msg} }

func exitCode(err error) int {
	var ce *cliError
	if errors.As(err, &ce) {
		return ce.code
	}
	return exitError
}

// 口が 2xx 以外を返したときの誤り。本文 (口の英語の説明) をそのまま添える
type httpError struct {
	host   string
	method string
	path   string
	status int
	body   string
}

func (e *httpError) Error() string {
	b := strings.TrimSpace(e.body)
	if b == "" {
		b = http.StatusText(e.status)
	}
	return fmt.Sprintf("%s: %s %s: %d %s", e.host, e.method, e.path, e.status, b)
}

func statusExit(status int) int {
	switch status {
	case 403:
		return exitForbidden
	case 404:
		return exitNotFound
	case 409, 503:
		return exitBusy
	}
	return exitError
}

type Client struct {
	target  *Target
	hc      *http.Client
	verbose io.Writer // nil なら出さない
	// 要求ごとの時間切れの既定 (0 なら切らない)
	timeout time.Duration
}

func newClient(t *Target, verbose io.Writer) *Client {
	tr := &http.Transport{
		Proxy: nil, // token を LAN の外 (プロキシ) に出さない
		DialContext: (&net.Dialer{
			Timeout: 5 * time.Second,
		}).DialContext,
		DisableKeepAlives:  true, // 口は同時に 1 つしか受けず、毎回 Connection: close を返す
		DisableCompression: true,
		// 口は 100 Continue を、token や使用中のイメージを確かめたあとに返す (host_net.cpp の PutFile)。
		// 断られたときに本体を送り始めず、403 / 409 / 503 を受け取れるようにする
		ExpectContinueTimeout: 3 * time.Second,
	}
	return &Client{
		target: t,
		hc: &http.Client{
			Transport: tr,
			// 転送先に token を持って行かない
			CheckRedirect: func(*http.Request, []*http.Request) error { return http.ErrUseLastResponse },
		},
		verbose: verbose,
		timeout: 30 * time.Second,
	}
}

// path は /files/... のように頭に / を付けたもの。escapedPath はそれを URL に書く形にしたもの
type reqSpec struct {
	method      string
	escapedPath string // 例 /files/pix68k/a%20b.cfg
	query       url.Values
	body        io.Reader
	length      int64 // body の長さ (body があるときは必ず決める。口は chunked を断る)
	expect      bool  // Expect: 100-continue を付ける (大きな本体)
	auth        bool  // token を付ける
	timeout     time.Duration
	noTimeout   bool
}

func (c *Client) url(escapedPath string, q url.Values) (*url.URL, error) {
	s := "http://" + c.target.Host + escapedPath
	if len(q) > 0 {
		s += "?" + q.Encode()
	}
	return url.Parse(s)
}

// 要求を送り、応答を返す。2xx 以外なら本文を読んで httpError にする。呼んだ側が resp.Body を閉じる
func (c *Client) do(ctx context.Context, r reqSpec) (*http.Response, context.CancelFunc, error) {
	u, err := c.url(r.escapedPath, r.query)
	if err != nil {
		return nil, nil, err
	}
	timeout := c.timeout
	if r.timeout > 0 {
		timeout = r.timeout
	}
	cancel := context.CancelFunc(func() {})
	if !r.noTimeout && timeout > 0 {
		ctx, cancel = context.WithTimeout(ctx, timeout)
	}
	req, err := http.NewRequestWithContext(ctx, r.method, u.String(), r.body)
	if err != nil {
		cancel()
		return nil, nil, err
	}
	if r.body != nil {
		req.ContentLength = r.length
		if r.length == 0 {
			req.Body = http.NoBody
		}
	}
	req.Header.Set("User-Agent", "pix68kcli/"+version)
	if r.expect && r.length > 0 {
		req.Header.Set("Expect", "100-continue")
	}
	if r.auth && c.target.Token != "" {
		req.Header.Set("X-Pix68k-Token", c.target.Token)
	}
	if c.verbose != nil {
		tok := ""
		if r.auth && c.target.Token != "" {
			tok = " (token: " + c.target.TokenSrc + ")"
		}
		fmt.Fprintf(c.verbose, "> %s %s%s\n", r.method, u.RequestURI(), tok)
	}
	t0 := time.Now()
	resp, err := c.hc.Do(req)
	if err != nil {
		cancel()
		return nil, nil, c.connErr(r, err)
	}
	if c.verbose != nil {
		fmt.Fprintf(c.verbose, "< %s (%d ms)\n", resp.Status, time.Since(t0).Milliseconds())
	}
	if resp.StatusCode < 200 || resp.StatusCode > 299 {
		b, _ := io.ReadAll(io.LimitReader(resp.Body, 4096))
		resp.Body.Close()
		cancel()
		p := u.Path
		return nil, nil, &cliError{statusExit(resp.StatusCode), (&httpError{c.target.Host, r.method, p, resp.StatusCode, string(b)}).Error()}
	}
	return resp, cancel, nil
}

func (c *Client) connErr(r reqSpec, err error) error {
	var ue *url.Error
	if errors.As(err, &ue) {
		err = ue.Err
	}
	msg := err.Error()
	switch {
	case errors.Is(err, context.DeadlineExceeded) || os.IsTimeout(err):
		msg = "時間切れ (" + msg + ")"
	case errors.Is(err, context.Canceled):
		msg = "取り消した"
	}
	return &cliError{exitConnect, fmt.Sprintf("%s: %s %s: つながらない: %s", c.target.Host, r.method, r.escapedPath, msg)}
}

// 本文を全部読んで返す
func (c *Client) fetch(ctx context.Context, r reqSpec) ([]byte, error) {
	resp, cancel, err := c.do(ctx, r)
	if err != nil {
		return nil, err
	}
	defer cancel()
	defer resp.Body.Close()
	b, err := io.ReadAll(resp.Body)
	if err != nil {
		return nil, c.connErr(r, err)
	}
	return b, nil
}

// SD カードのパス (/pix68k/pix68k.cfg など) を /files/... の URL の形にする
func filesPath(p string) string {
	p = strings.ReplaceAll(p, `\`, "/")
	if !strings.HasPrefix(p, "/") {
		p = "/" + p
	}
	segs := strings.Split(p, "/")
	for i, s := range segs {
		segs[i] = url.PathEscape(s)
	}
	return "/files" + strings.Join(segs, "/")
}
