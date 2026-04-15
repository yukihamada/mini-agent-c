FROM debian:bookworm-slim AS builder

RUN apt-get update && apt-get install -y \
    build-essential \
    libcurl4-openssl-dev \
    libmicrohttpd-dev \
    libsqlite3-dev \
    libssl-dev \
    --no-install-recommends \
    && rm -rf /var/lib/apt/lists/*

WORKDIR /build
COPY agent.v10.c cJSON.c cJSON.h ./
RUN gcc -O2 -o agent.v10 agent.v10.c cJSON.c -lcurl -lm

COPY bot.c cJSON.c cJSON.h ./
RUN gcc -O2 -Wall -o bot bot.c cJSON.c \
    -lmicrohttpd -lcurl -lsqlite3 -lssl -lcrypto -lpthread -lm

# ── Runtime ──
FROM debian:bookworm-slim

RUN apt-get update && apt-get install -y \
    libcurl4 \
    libmicrohttpd12 \
    libsqlite3-0 \
    libssl3 \
    --no-install-recommends \
    && rm -rf /var/lib/apt/lists/*

WORKDIR /app
COPY --from=builder /build/agent.v10 /app/agent.v10
COPY --from=builder /build/bot        /app/bot
RUN chmod +x /app/agent.v10 /app/bot

EXPOSE 8080
CMD ["/app/bot"]
