FROM debian:bookworm
RUN apt-get update && DEBIAN_FRONTEND=noninteractive apt-get install -y --no-install-recommends \
    build-essential git wget curl ca-certificates cpio unzip rsync bc file python3 \
    libncurses-dev flex bison libssl-dev libelf-dev xz-utils perl patch gzip bzip2 \
    && rm -rf /var/lib/apt/lists/*
RUN useradd -m -u 1000 builder
USER builder
WORKDIR /home/builder
