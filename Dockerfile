FROM debian:stable-slim AS builder

RUN apt-get update && \
    apt-get install -y --no-install-recommends build-essential ca-certificates git make && \
    rm -rf /var/lib/apt/lists/*

WORKDIR /src
COPY . .

RUN make release

FROM debian:stable-slim

COPY --from=builder /src/fconcat /usr/local/bin/fconcat

ENTRYPOINT ["/usr/local/bin/fconcat"]
CMD ["--help"]
