// bitkv-server: exposes the engine over the Redis protocol.
//
// Architecture: ONE thread, ONE epoll instance, non-blocking sockets. This is
// the classic Redis design. Every command runs to completion before the next
// one starts, so commands are atomic without any locking in this file. The
// cost: a slow command (or an fsync in --sync always mode) stalls every
// client. That trade-off is worth being able to explain.
//
// Linux only (epoll). On macOS or Windows, run it in Docker or WSL.

#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <signal.h>
#include <sys/epoll.h>
#include <sys/socket.h>
#include <unistd.h>

#include <chrono>
#include <cerrno>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

#include "bitkv/db.h"
#include "commands.h"
#include "metrics.h"
#include "resp.h"

namespace {

volatile std::sig_atomic_t g_stop = 0;
void OnSignal(int) { g_stop = 1; }

// A client whose unread replies exceed this is dropped rather than letting it
// consume unbounded memory (e.g. pipelining GETs without reading the output).
constexpr size_t kMaxOutputBuffer = 64 * 1024 * 1024;

struct Conn {
  int fd = -1;
  bool http = false;   // a Prometheus / probe connection, not a RESP client
  std::string in;
  std::string out;
  size_t out_pos = 0;  // bytes of `out` already sent
  bool closing = false;
  bool want_write = false;
};

bool SetNonBlocking(int fd) {
  int flags = ::fcntl(fd, F_GETFL, 0);
  return flags >= 0 && ::fcntl(fd, F_SETFL, flags | O_NONBLOCK) == 0;
}

void Usage(const char* prog) {
  std::fprintf(stderr,
               "usage: %s [--dir PATH] [--port N] [--metrics-port N (0=off)]\n"
               "          [--sync always|everysec|none] [--no-group-commit]\n"
               "          [--max-file-size BYTES]\n",
               prog);
}

}  // namespace

int main(int argc, char** argv) {
  std::string dir = "./bitkv-data";
  int port = 6380;  // 6379 is real Redis; avoid colliding with it
  int metrics_port = 9121;
  bitkv::Options opts;

  for (int i = 1; i < argc; ++i) {
    std::string a = argv[i];
    auto next = [&]() -> const char* {
      if (i + 1 >= argc) {
        Usage(argv[0]);
        std::exit(2);
      }
      return argv[++i];
    };
    if (a == "--dir") dir = next();
    else if (a == "--port") port = std::atoi(next());
    else if (a == "--metrics-port") metrics_port = std::atoi(next());
    else if (a == "--max-file-size") opts.max_file_size = std::strtoull(next(), nullptr, 10);
    else if (a == "--no-group-commit") opts.group_commit = false;
    else if (a == "--sync") {
      std::string m = next();
      if (m == "always") opts.sync_mode = bitkv::SyncMode::kAlways;
      else if (m == "everysec") opts.sync_mode = bitkv::SyncMode::kEverySec;
      else if (m == "none") opts.sync_mode = bitkv::SyncMode::kNone;
      else { Usage(argv[0]); return 2; }
    } else { Usage(argv[0]); return 2; }
  }

  std::unique_ptr<bitkv::DB> db;
  bitkv::Status s = bitkv::DB::Open(opts, dir, &db);
  if (!s.ok()) {
    std::fprintf(stderr, "open failed: %s\n", s.ToString().c_str());
    return 1;
  }
  bitkv::Stats st = db->GetStats();
  std::fprintf(stderr, "bitkv: recovered %llu keys from %llu files in %s",
               (unsigned long long)st.live_keys, (unsigned long long)st.data_files, dir.c_str());
  if (st.recovered_torn_bytes)
    std::fprintf(stderr, " (truncated %llu torn bytes)",
                 (unsigned long long)st.recovered_torn_bytes);
  std::fprintf(stderr, "\n");

  ::signal(SIGPIPE, SIG_IGN);  // a client hanging up must not kill the server
  struct sigaction sa {};
  sa.sa_handler = OnSignal;
  ::sigaction(SIGINT, &sa, nullptr);
  ::sigaction(SIGTERM, &sa, nullptr);

  int one = 1;
  auto make_listener = [&](int p) -> int {
    int fd = ::socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0);
    ::setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(static_cast<uint16_t>(p));
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    if (::bind(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0 ||
        ::listen(fd, 1024) != 0 || !SetNonBlocking(fd)) {
      std::perror("listen");
      std::exit(1);
    }
    return fd;
  };

  int ep = ::epoll_create1(EPOLL_CLOEXEC);
  auto watch = [&](int fd) {
    epoll_event ev{};
    ev.events = EPOLLIN;
    ev.data.fd = fd;
    ::epoll_ctl(ep, EPOLL_CTL_ADD, fd, &ev);
  };
  const int lfd = make_listener(port);
  watch(lfd);
  std::fprintf(stderr, "bitkv: listening on port %d\n", port);
  int mfd = -1;
  if (metrics_port > 0) {
    mfd = make_listener(metrics_port);
    watch(mfd);
    std::fprintf(stderr, "bitkv: metrics and health on http://0.0.0.0:%d/metrics\n", metrics_port);
  }
  bitkv::Metrics metrics;

  std::unordered_map<int, std::unique_ptr<Conn>> conns;
  std::vector<epoll_event> events(256);
  std::vector<std::string> args;
  char rbuf[64 * 1024];

  auto close_conn = [&](Conn* c) {
    if (!c->http) metrics.ConnectionClosed();
    ::epoll_ctl(ep, EPOLL_CTL_DEL, c->fd, nullptr);
    ::close(c->fd);
    conns.erase(c->fd);
  };

  // Sends as much of c->out as the socket accepts. Returns false on error.
  auto flush = [&](Conn* c) -> bool {
    while (c->out_pos < c->out.size()) {
      ssize_t n = ::send(c->fd, c->out.data() + c->out_pos, c->out.size() - c->out_pos,
                         MSG_NOSIGNAL);
      if (n < 0) {
        if (errno == EAGAIN || errno == EWOULDBLOCK) break;
        if (errno == EINTR) continue;
        return false;
      }
      c->out_pos += static_cast<size_t>(n);
    }
    if (c->out_pos == c->out.size()) {
      c->out.clear();
      c->out_pos = 0;
    }
    // Only ask epoll for writability while there is something left to send;
    // otherwise a level-triggered EPOLLOUT would wake us constantly.
    const bool want = !c->out.empty();
    if (want != c->want_write) {
      epoll_event e{};
      e.events = uint32_t(EPOLLIN) | (want ? uint32_t(EPOLLOUT) : 0u);
      e.data.fd = c->fd;
      ::epoll_ctl(ep, EPOLL_CTL_MOD, c->fd, &e);
      c->want_write = want;
    }
    return true;
  };

  while (!g_stop) {
    int n = ::epoll_wait(ep, events.data(), static_cast<int>(events.size()), 200);
    if (n < 0) {
      if (errno == EINTR) continue;
      std::perror("epoll_wait");
      break;
    }
    for (int i = 0; i < n; ++i) {
      const int fd = events[i].data.fd;

      if (fd == lfd || fd == mfd) {
        for (;;) {
          int cfd = ::accept4(fd, nullptr, nullptr, SOCK_NONBLOCK | SOCK_CLOEXEC);
          if (cfd < 0) break;  // EAGAIN: no more pending connections
          ::setsockopt(cfd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
          epoll_event e{};
          e.events = EPOLLIN;
          e.data.fd = cfd;
          ::epoll_ctl(ep, EPOLL_CTL_ADD, cfd, &e);
          auto conn = std::make_unique<Conn>();
          conn->fd = cfd;
          conn->http = (fd == mfd);
          if (!conn->http) metrics.ConnectionOpened();
          conns[cfd] = std::move(conn);
        }
        continue;
      }

      auto it = conns.find(fd);
      if (it == conns.end()) continue;
      Conn* c = it->second.get();

      if (events[i].events & (EPOLLERR | EPOLLHUP)) {
        close_conn(c);
        continue;
      }

      if (events[i].events & EPOLLIN) {
        bool peer_closed = false;
        for (;;) {
          ssize_t r = ::recv(fd, rbuf, sizeof(rbuf), 0);
          if (r > 0) {
            c->in.append(rbuf, static_cast<size_t>(r));
            continue;
          }
          if (r == 0) peer_closed = true;
          else if (errno == EINTR) continue;
          else if (errno != EAGAIN && errno != EWOULDBLOCK) peer_closed = true;
          break;
        }

        if (c->http) {
          // Minimal HTTP/1.1 for Prometheus scrapes and Kubernetes probes:
          // wait for the end of the request headers, answer, close.
          const size_t hdr_end = c->in.find("\r\n\r\n");
          if (hdr_end == std::string::npos) {
            if (peer_closed || c->in.size() > 8192) { close_conn(c); continue; }
          } else {
            const size_t sp1 = c->in.find(' ');
            const size_t sp2 = c->in.find(' ', sp1 + 1);
            const std::string method = c->in.substr(0, sp1);
            const std::string path =
                sp1 == std::string::npos ? "" : c->in.substr(sp1 + 1, sp2 - sp1 - 1);
            std::string status = "200 OK", type = "text/plain; charset=utf-8", body;
            if (method != "GET") {
              status = "405 Method Not Allowed";
              body = "only GET\n";
            } else if (path == "/metrics") {
              type = "text/plain; version=0.0.4; charset=utf-8";
              body = metrics.Render(db->GetStats());
            } else if (path == "/healthz") {
              body = "ok\n";  // liveness: the event loop is responsive
            } else if (path == "/readyz") {
              db->Size();      // readiness: the database is open and serving
              body = "ready\n";
            } else {
              status = "404 Not Found";
              body = "not found\n";
            }
            c->out = "HTTP/1.1 " + status + "\r\nContent-Type: " + type +
                     "\r\nContent-Length: " + std::to_string(body.size()) +
                     "\r\nConnection: close\r\n\r\n" + body;
            c->in.clear();
            c->closing = true;
          }
          if (!flush(c)) { close_conn(c); continue; }
          if (c->closing && c->out.empty()) close_conn(c);
          continue;
        }

        // Execute every complete command in the buffer (pipelining).
        size_t pos = 0;
        while (!c->closing) {
          size_t used = 0;
          std::string err;
          auto pr = bitkv::resp::ParseCommand(std::string_view(c->in).substr(pos), &args,
                                              &used, &err);
          if (pr == bitkv::resp::ParseResult::kIncomplete) break;
          if (pr == bitkv::resp::ParseResult::kError) {
            bitkv::resp::Error(&c->out, err);
            c->closing = true;
            break;
          }
          pos += used;
          bool quit = false;
          const size_t reply_start = c->out.size();
          const auto t0 = std::chrono::steady_clock::now();
          bitkv::ExecuteCommand(db.get(), args, &c->out, &quit);
          metrics.ObserveCommand(
              args.empty() ? "unknown" : bitkv::NormalizeCommand(args[0]),
              std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count(),
              c->out.size() > reply_start && c->out[reply_start] == '-');
          if (quit) c->closing = true;
        }
        c->in.erase(0, pos);

        if (peer_closed || c->out.size() > kMaxOutputBuffer) {
          close_conn(c);
          continue;
        }
      }

      if (!flush(c)) {
        close_conn(c);
        continue;
      }
      if (c->closing && c->out.empty()) close_conn(c);
    }
  }

  std::fprintf(stderr, "\nbitkv: shutting down, syncing to disk\n");
  for (auto& [fd, c] : conns) ::close(fd);
  ::close(lfd);
  if (mfd >= 0) ::close(mfd);
  ::close(ep);
  db.reset();  // destructor performs the final fsync
  return 0;
}
