package resp

import (
	"bufio"
	"errors"
	"net"
	"testing"
)

// fakeServer answers each request on the server side of a net.Pipe with the
// next canned reply.
func fakeServer(t *testing.T, replies ...string) *Client {
	t.Helper()
	cli, srv := net.Pipe()
	go func() {
		r := bufio.NewReader(srv)
		for _, rep := range replies {
			// Consume one request: "*N\r\n" then N x ("$len\r\n" "data\r\n").
			hdr, err := r.ReadString('\n')
			if err != nil {
				return
			}
			var n int
			for _, ch := range hdr[1 : len(hdr)-2] {
				n = n*10 + int(ch-'0')
			}
			for i := 0; i < 2*n; i++ {
				if _, err := r.ReadString('\n'); err != nil {
					return
				}
			}
			srv.Write([]byte(rep))
		}
	}()
	t.Cleanup(func() { cli.Close(); srv.Close() })
	return New(cli)
}

func TestReplyTypes(t *testing.T) {
	c := fakeServer(t, "+PONG\r\n", ":42\r\n", "$5\r\nhello\r\n", "$-1\r\n", "*2\r\n:1\r\n+ok\r\n")
	if v, _ := c.Do("PING"); v != "PONG" {
		t.Fatalf("simple string: %v", v)
	}
	if v, _ := c.Do("INCR", "k"); v != int64(42) {
		t.Fatalf("integer: %v", v)
	}
	if v, _ := c.Do("GET", "k"); v != "hello" {
		t.Fatalf("bulk: %v", v)
	}
	if v, err := c.Do("GET", "missing"); v != nil || err != nil {
		t.Fatalf("null bulk: %v %v", v, err)
	}
	v, _ := c.Do("X")
	arr, ok := v.([]any)
	if !ok || len(arr) != 2 || arr[0] != int64(1) || arr[1] != "ok" {
		t.Fatalf("array: %v", v)
	}
}

func TestServerError(t *testing.T) {
	c := fakeServer(t, "-ERR boom\r\n")
	_, err := c.Do("NOPE")
	var se *ServerError
	if !errors.As(err, &se) || se.Msg != "ERR boom" {
		t.Fatalf("want ServerError, got %v", err)
	}
}

func TestBinarySafeBulk(t *testing.T) {
	c := fakeServer(t, "$4\r\na\r\nb\r\n")
	if v, _ := c.Do("GET", "k"); v != "a\r\nb" {
		t.Fatalf("got %q", v)
	}
}
