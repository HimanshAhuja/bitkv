// Package resp is a minimal Redis-protocol (RESP2) client, enough to drive
// bitkv. Standard library only.
package resp

import (
	"bufio"
	"errors"
	"fmt"
	"io"
	"net"
	"strconv"
	"time"
)

// Client is a single connection. It is not safe for concurrent use; open one
// per goroutine.
type Client struct {
	conn net.Conn
	r    *bufio.Reader
	w    *bufio.Writer
}

// ServerError is an error reply ("-ERR ...") from the server, as opposed to a
// network or protocol failure.
type ServerError struct{ Msg string }

func (e *ServerError) Error() string { return e.Msg }

// Dial connects with a timeout.
func Dial(addr string, timeout time.Duration) (*Client, error) {
	c, err := net.DialTimeout("tcp", addr, timeout)
	if err != nil {
		return nil, err
	}
	return New(c), nil
}

// New wraps an existing connection (used by tests with net.Pipe).
func New(c net.Conn) *Client {
	return &Client{conn: c, r: bufio.NewReader(c), w: bufio.NewWriter(c)}
}

func (c *Client) Close() error { return c.conn.Close() }

// SetDeadline bounds the next round trip, so a hung server cannot hang a probe.
func (c *Client) SetDeadline(t time.Time) error { return c.conn.SetDeadline(t) }

// Do sends one command and returns the parsed reply: string, int64, nil
// (null bulk), []any (arrays), or a *ServerError.
func (c *Client) Do(args ...string) (any, error) {
	fmt.Fprintf(c.w, "*%d\r\n", len(args))
	for _, a := range args {
		fmt.Fprintf(c.w, "$%d\r\n%s\r\n", len(a), a)
	}
	if err := c.w.Flush(); err != nil {
		return nil, err
	}
	return c.read()
}

func (c *Client) line() (string, error) {
	s, err := c.r.ReadString('\n')
	if err != nil {
		return "", err
	}
	if len(s) < 2 || s[len(s)-2] != '\r' {
		return "", errors.New("resp: line not terminated by CRLF")
	}
	return s[:len(s)-2], nil
}

func (c *Client) read() (any, error) {
	l, err := c.line()
	if err != nil {
		return nil, err
	}
	if l == "" {
		return nil, errors.New("resp: empty reply")
	}
	switch l[0] {
	case '+':
		return l[1:], nil
	case '-':
		return nil, &ServerError{Msg: l[1:]}
	case ':':
		return strconv.ParseInt(l[1:], 10, 64)
	case '$':
		n, err := strconv.Atoi(l[1:])
		if err != nil {
			return nil, err
		}
		if n < 0 {
			return nil, nil // null bulk: key not found
		}
		buf := make([]byte, n+2)
		if _, err := io.ReadFull(c.r, buf); err != nil {
			return nil, err
		}
		return string(buf[:n]), nil
	case '*':
		n, err := strconv.Atoi(l[1:])
		if err != nil {
			return nil, err
		}
		out := make([]any, 0, max(n, 0))
		for i := 0; i < n; i++ {
			v, err := c.read()
			if err != nil {
				return nil, err
			}
			out = append(out, v)
		}
		return out, nil
	}
	return nil, fmt.Errorf("resp: unexpected reply type %q", l[0])
}
