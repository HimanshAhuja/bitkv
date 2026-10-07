# syntax=docker/dockerfile:1
# Multi-stage build: compile in throwaway stages, ship only two binaries.

# ---- C++ server --------------------------------------------------------
FROM ubuntu:24.04 AS server-build
RUN apt-get update && apt-get install -y --no-install-recommends g++ cmake make \
    && rm -rf /var/lib/apt/lists/*
WORKDIR /src
COPY CMakeLists.txt ./
COPY include include
COPY src src
COPY server server
COPY tools tools
COPY bench bench
RUN cmake -B build -DCMAKE_BUILD_TYPE=Release -DBITKV_BUILD_TESTS=OFF \
    && cmake --build build -j --target bitkv-server

# ---- Go CLI ------------------------------------------------------------
FROM golang:1.22 AS ctl-build
WORKDIR /src
COPY ctl/ ./
# Static binary: no libc dependency, runs on any base image.
RUN CGO_ENABLED=0 go build -trimpath -ldflags="-s -w" -o /bitkvctl .

# ---- runtime -----------------------------------------------------------
FROM ubuntu:24.04
COPY --from=server-build /src/build/bitkv-server /usr/local/bin/bitkv-server
COPY --from=ctl-build /bitkvctl /usr/local/bin/bitkvctl

# Non-root. /data is owned by group 0 and group-writable so the container
# also runs under an arbitrary UID, as OpenShift's restricted SCC assigns.
RUN useradd --system --uid 10001 --gid 0 --no-create-home bitkv \
    && mkdir -p /data && chown 10001:0 /data && chmod 0775 /data
USER 10001
VOLUME ["/data"]
EXPOSE 6380 9121

HEALTHCHECK --interval=10s --timeout=3s --start-period=30s \
    CMD ["bitkvctl", "health"]

# exec form: bitkv-server is PID 1 and receives SIGTERM directly, so it can
# fsync before exiting when Docker or Kubernetes stops it.
ENTRYPOINT ["bitkv-server"]
CMD ["--dir", "/data", "--port", "6380", "--metrics-port", "9121", "--sync", "everysec"]
