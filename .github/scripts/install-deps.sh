#!/usr/bin/env bash
# What CI builds and tests with, on Ubuntu 26.04 as root (the job's container): the packages of
# apps/blog/deploy/docker/Dockerfile's build stage, fmt 12 and FlatBuffers built the same way, and for
# the blog's tests node and pnpm. Keep the versions in step with the Dockerfile.

set -euo pipefail

export DEBIAN_FRONTEND=noninteractive
apt-get update
apt-get install -y --no-install-recommends \
  ca-certificates curl git ccache \
  g++ cmake ninja-build pkg-config \
  libboost-dev libboost-filesystem-dev libboost-json-dev libboost-url-dev libssl-dev libpq-dev \
  libyaml-cpp-dev libutfcpp-dev catch2 libargon2-dev libqrencode-dev libreadline-dev libcurl4-openssl-dev libjemalloc-dev \
  nodejs npm
rm -rf /var/lib/apt/lists/*
npm install -g pnpm@12.14.0

# Ubuntu has fmt 10 and FlatBuffers 23.5: static ones in /usr/local, found before the system's
build() {
  local name=$1 url=$2; shift 2
  curl --retry 5 --retry-all-errors --retry-delay 5 -fsSL "$url" | tar -xz -C /tmp
  cmake -S "/tmp/$name" -B "/tmp/$name-build" -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_POSITION_INDEPENDENT_CODE=ON "$@"
  cmake --build "/tmp/$name-build" --target install
  rm -rf "/tmp/$name" "/tmp/$name-build"
}
build fmt-12.2.0 https://github.com/fmtlib/fmt/archive/refs/tags/12.2.0.tar.gz -DFMT_TEST=OFF -DFMT_DOC=OFF
build flatbuffers-25.12.19 https://github.com/google/flatbuffers/archive/refs/tags/v25.12.19.tar.gz -DFLATBUFFERS_BUILD_TESTS=OFF
