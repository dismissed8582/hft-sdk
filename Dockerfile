# syntax=docker/dockerfile:1
FROM ubuntu:24.04 AS build

ARG DEBIAN_FRONTEND=noninteractive
RUN apt-get update && apt-get install -y --no-install-recommends \
        build-essential cmake git zlib1g-dev ca-certificates \
    && rm -rf /var/lib/apt/lists/*

WORKDIR /src
COPY . .
RUN cmake -S . -B build \
        -DCMAKE_BUILD_TYPE=Release \
        -DCMAKE_INSTALL_PREFIX=/opt/hft \
        -DHFT_BUILD_TESTS=OFF \
        -DHFT_BUILD_BENCHMARKS=OFF \
        -DHFT_BUILD_EVALUATOR=OFF \
        -DHFT_BUILD_REPLAY=OFF \
        -DHFT_BUILD_PYTHON=OFF \
        -DHFT_BUILD_SHARED_SDK=ON \
        -DHFT_BUILD_FEED=ON \
    && cmake --build build -j "$(nproc)" \
    && cmake --install build

FROM ubuntu:24.04

ARG DEBIAN_FRONTEND=noninteractive
RUN apt-get update && apt-get install -y --no-install-recommends \
        libstdc++6 ca-certificates \
    && rm -rf /var/lib/apt/lists/*

COPY --from=build /opt/hft /opt/hft
COPY config/hft_feed.conf /etc/hft/hft_feed.conf

ENV LD_LIBRARY_PATH=/opt/hft/lib
EXPOSE 9100
ENTRYPOINT ["/opt/hft/bin/hft_feed"]
CMD ["--config", "/etc/hft/hft_feed.conf"]
