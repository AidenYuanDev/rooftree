# The blog in one image, for a host that can't build it: Ubuntu 24.04's OpenSSL is 3.0, the framework
# needs 3.2. Ubuntu 26.04 has everything as packages. ship.sh builds it from the files git knows of:
#
#   git ls-files -z -co --exclude-standard | tar --null -T - -c | docker build -f apps/blog/deploy/docker/Dockerfile -
#
# The image holds the server, apps/blog with its node_modules (dbmate, the prerender build) and node;
# config, site, uploads and backups are the host's (compose.yaml).
#
# --build-arg BLOG_BINARY_STAGE=prebuilt takes the server from prebuilt/blog in the context, built and
# tested on this Ubuntu by CI, instead of building it.

ARG BLOG_BINARY_STAGE=build

FROM ubuntu:26.04 AS build
RUN apt-get update && DEBIAN_FRONTEND=noninteractive apt-get install -y --no-install-recommends \
      g++ cmake ninja-build pkg-config \
      libboost-dev libboost-filesystem-dev libboost-json-dev libboost-url-dev libssl-dev libpq-dev \
      libyaml-cpp-dev libutfcpp-dev catch2 libargon2-dev libqrencode-dev libreadline-dev libcurl4-openssl-dev libjemalloc-dev \
    && rm -rf /var/lib/apt/lists/*
# The framework is written against fmt 12 (StringFormatFwd.h names its v12 namespace), Ubuntu has 10:
# a static one in /usr/local, found before the system's
ADD https://github.com/fmtlib/fmt/archive/refs/tags/12.2.0.tar.gz /tmp/fmt.tar.gz
RUN tar -xzf /tmp/fmt.tar.gz -C /tmp \
    && cmake -S /tmp/fmt-12.2.0 -B /tmp/fmt-build -G Ninja -DCMAKE_BUILD_TYPE=Release \
         -DCMAKE_POSITION_INDEPENDENT_CODE=ON -DFMT_TEST=OFF -DFMT_DOC=OFF \
    && cmake --build /tmp/fmt-build --target install \
    && rm -rf /tmp/fmt*
# FlatBuffers (flatc and the static library), the version Arch has: Ubuntu's 23.5 is older
ADD https://github.com/google/flatbuffers/archive/refs/tags/v25.12.19.tar.gz /tmp/flatbuffers.tar.gz
RUN tar -xzf /tmp/flatbuffers.tar.gz -C /tmp \
    && cmake -S /tmp/flatbuffers-25.12.19 -B /tmp/flatbuffers-build -G Ninja -DCMAKE_BUILD_TYPE=Release \
         -DCMAKE_POSITION_INDEPENDENT_CODE=ON -DFLATBUFFERS_BUILD_TESTS=OFF \
    && cmake --build /tmp/flatbuffers-build --target install \
    && rm -rf /tmp/flatbuffers*
COPY . /src
WORKDIR /src
# The tests run against this image's libraries; the database ones are skipped (no database here).
RUN cmake -B build -G Ninja -DCMAKE_BUILD_TYPE=Release -DEXAMPLES=0 \
    && cmake --build build \
    && ctest --test-dir build --output-on-failure

# Never nothing to copy: a builder without BuildKit runs this stage even when it is unused
FROM ubuntu:26.04 AS prebuilt
COPY apps/blog/deploy/docker/Dockerfile prebuilt/blo[g] /src/build/bin/

FROM ${BLOG_BINARY_STAGE} AS binary

FROM ubuntu:26.04
RUN apt-get update && DEBIAN_FRONTEND=noninteractive apt-get install -y --no-install-recommends \
      libboost-filesystem1.90.0 libboost-json1.90.0 libboost-url1.90.0 libssl3t64 libpq5 \
      libyaml-cpp0.8 libargon2-1 libqrencode4 libreadline8t64 libcurl4t64 libjemalloc2 ca-certificates libcap2-bin \
      nodejs npm postgresql-client \
    && npm install -g pnpm@12.14.0 \
    && apt-get purge -y --auto-remove npm \
    && rm -rf /var/lib/apt/lists/* /root/.npm
# The host's directories (compose.yaml) belong to this uid
RUN groupadd --system --gid 2000 blog \
    && useradd --system --uid 2000 --gid blog --home-dir /var/lib/blog --create-home --shell /usr/sbin/nologin blog

COPY --from=binary /src/build/bin/blog /opt/blog/bin/blog
# Ports 80 and 443 as blog, the host's network (no root, no proxy)
RUN setcap cap_net_bind_service=+ep /opt/blog/bin/blog \
    && ! ldd /opt/blog/bin/blog | grep 'not found'

COPY --chown=blog:blog apps/blog /opt/blog/src/apps/blog
USER blog
WORKDIR /opt/blog/src/apps/blog
RUN pnpm install --frozen-lockfile

WORKDIR /var/lib/blog
ENTRYPOINT ["/opt/blog/src/apps/blog/deploy/docker/entrypoint.sh"]
