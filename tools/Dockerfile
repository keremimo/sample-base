FROM debian:bookworm-slim
RUN apt-get update && apt-get install -y --no-install-recommends \
    g++ make git ca-certificates pkg-config python3 patch \
    libasound2-dev libsystemd-dev dbus libdrm-dev libfreetype6-dev libfontconfig1-dev \
    fonts-liberation \
    && rm -rf /var/lib/apt/lists/*
WORKDIR /work
