// bitkvctl is the operations CLI for bitkv.
//
// It is shipped inside the container image and used as the Kubernetes
// readiness probe (`bitkvctl health`), by CI to drive load against a cluster
// (`bitkvctl load`), and by humans for day-to-day operations.
//
// Configuration: --addr / $BITKV_ADDR (default 127.0.0.1:6380) and
// --metrics / $BITKV_METRICS (default http://127.0.0.1:9121).
package main

import (
	"flag"
	"fmt"
	"io"
	"math/rand"
	"net/http"
	"os"
	"sort"
	"strings"
	"sync"
	"time"

	"github.com/HimanshAhuja/bitkv/ctl/internal/resp"
)

const usage = `bitkvctl: operate a bitkv server

usage: bitkvctl [--addr host:port] [--metrics url] [--timeout d] <command> [args]

commands:
  ping                 round-trip a PING
  health               exit 0 only if the server answers PING and /readyz
                       (used as the Kubernetes readiness probe)
  wait [--for 60s]     block until healthy or the deadline passes
  info                 print server statistics
  get KEY | set KEY VALUE | del KEY
  compact              run a merge (compaction) on the server
  load [--n N] [--c C] [--size B] [--read-pct P]
                       concurrent load generator; prints throughput and latency
`

type config struct {
	addr, metrics string
	timeout       time.Duration
}

func main() {
	cfg := config{}
	fs := flag.NewFlagSet("bitkvctl", flag.ExitOnError)
	fs.Usage = func() { fmt.Fprint(os.Stderr, usage) }
	fs.StringVar(&cfg.addr, "addr", envOr("BITKV_ADDR", "127.0.0.1:6380"), "server address")
	fs.StringVar(&cfg.metrics, "metrics", envOr("BITKV_METRICS", "http://127.0.0.1:9121"), "metrics base URL")
	fs.DurationVar(&cfg.timeout, "timeout", 2*time.Second, "per-request timeout")
	fs.Parse(os.Args[1:])
	if fs.NArg() == 0 {
		fs.Usage()
		os.Exit(2)
	}
	cmd, args := fs.Arg(0), fs.Args()[1:]

	var err error
	switch cmd {
	case "ping":
		err = simple(cfg, "PING")
	case "health":
		err = health(cfg)
	case "wait":
		err = wait(cfg, args)
	case "info":
		err = simple(cfg, "INFO")
	case "get":
		err = simple(cfg, append([]string{"GET"}, args...)...)
	case "set":
		err = simple(cfg, append([]string{"SET"}, args...)...)
	case "del":
		err = simple(cfg, append([]string{"DEL"}, args...)...)
	case "compact":
		err = simple(cfg, "COMPACT")
	case "load":
		err = load(cfg, args)
	default:
		fs.Usage()
		os.Exit(2)
	}
	if err != nil {
		fmt.Fprintln(os.Stderr, "bitkvctl:", err)
		os.Exit(1)
	}
}

func envOr(k, def string) string {
	if v := os.Getenv(k); v != "" {
		return v
	}
	return def
}

func dial(cfg config) (*resp.Client, error) {
	c, err := resp.Dial(cfg.addr, cfg.timeout)
	if err != nil {
		return nil, err
	}
	c.SetDeadline(time.Now().Add(cfg.timeout))
	return c, nil
}

func simple(cfg config, args ...string) error {
	c, err := dial(cfg)
	if err != nil {
		return err
	}
	defer c.Close()
	v, err := c.Do(args...)
	if err != nil {
		return err
	}
	switch x := v.(type) {
	case nil:
		fmt.Println("(nil)")
	case string:
		fmt.Println(strings.TrimRight(strings.ReplaceAll(x, "\r\n", "\n"), "\n"))
	default:
		fmt.Println(x)
	}
	return nil
}

// health checks both planes: the data plane (a RESP PING) and the control
// plane (/readyz). A process that accepts TCP but has a wedged event loop
// fails the PING with a timeout, which is why a plain TCP probe is not enough.
func health(cfg config) error {
	c, err := dial(cfg)
	if err != nil {
		return fmt.Errorf("data plane: %w", err)
	}
	defer c.Close()
	if v, err := c.Do("PING"); err != nil || v != "PONG" {
		return fmt.Errorf("data plane: PING returned %v, %v", v, err)
	}
	hc := http.Client{Timeout: cfg.timeout}
	r, err := hc.Get(cfg.metrics + "/readyz")
	if err != nil {
		return fmt.Errorf("control plane: %w", err)
	}
	io.Copy(io.Discard, r.Body)
	r.Body.Close()
	if r.StatusCode != http.StatusOK {
		return fmt.Errorf("control plane: /readyz returned %s", r.Status)
	}
	fmt.Println("healthy")
	return nil
}

func wait(cfg config, args []string) error {
	fs := flag.NewFlagSet("wait", flag.ExitOnError)
	limit := fs.Duration("for", 60*time.Second, "how long to wait")
	fs.Parse(args)
	deadline := time.Now().Add(*limit)
	for {
		if err := health(cfg); err == nil {
			return nil
		} else if time.Now().After(deadline) {
			return fmt.Errorf("not healthy after %s: %w", *limit, err)
		}
		time.Sleep(500 * time.Millisecond)
	}
}

// load runs C concurrent clients issuing N total operations and reports
// throughput and latency percentiles. Each goroutine owns its connection.
func load(cfg config, args []string) error {
	fs := flag.NewFlagSet("load", flag.ExitOnError)
	n := fs.Int("n", 100000, "total operations")
	conc := fs.Int("c", 16, "concurrent clients")
	size := fs.Int("size", 100, "value size in bytes")
	readPct := fs.Int("read-pct", 50, "percentage of operations that are GETs")
	keys := fs.Int("keys", 10000, "keyspace size")
	fs.Parse(args)

	value := strings.Repeat("v", *size)
	per := *n / *conc
	lat := make([][]time.Duration, *conc)
	errs := make([]error, *conc)
	var wg sync.WaitGroup
	start := time.Now()
	for w := 0; w < *conc; w++ {
		wg.Add(1)
		go func(w int) {
			defer wg.Done()
			c, err := resp.Dial(cfg.addr, cfg.timeout)
			if err != nil {
				errs[w] = err
				return
			}
			defer c.Close()
			rng := rand.New(rand.NewSource(int64(w) + 1))
			lat[w] = make([]time.Duration, 0, per)
			for i := 0; i < per; i++ {
				key := fmt.Sprintf("load:%d", rng.Intn(*keys))
				t0 := time.Now()
				c.SetDeadline(t0.Add(cfg.timeout))
				if rng.Intn(100) < *readPct {
					_, err = c.Do("GET", key)
				} else {
					_, err = c.Do("SET", key, value)
				}
				if err != nil {
					errs[w] = err
					return
				}
				lat[w] = append(lat[w], time.Since(t0))
			}
		}(w)
	}
	wg.Wait()
	elapsed := time.Since(start)
	for _, e := range errs {
		if e != nil {
			return e
		}
	}
	var all []time.Duration
	for _, l := range lat {
		all = append(all, l...)
	}
	sort.Slice(all, func(i, j int) bool { return all[i] < all[j] })
	pct := func(p float64) time.Duration { return all[int(p/100*float64(len(all)-1))] }
	fmt.Printf("ops=%d clients=%d read=%d%% elapsed=%s\n", len(all), *conc, *readPct, elapsed.Round(time.Millisecond))
	fmt.Printf("throughput=%.0f ops/s  p50=%s  p99=%s  p99.9=%s  max=%s\n",
		float64(len(all))/elapsed.Seconds(), pct(50), pct(99), pct(99.9), all[len(all)-1])
	return nil
}
