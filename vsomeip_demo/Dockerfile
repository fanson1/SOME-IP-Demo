FROM ubuntu:24.04

RUN apt-get update && apt-get install -y --no-install-recommends \
        build-essential cmake git \
        libboost-dev libboost-system-dev libboost-filesystem-dev libboost-thread-dev libdbus-1-dev \
    && rm -rf /var/lib/apt/lists/*

RUN curl -fsSL https://github.com/COVESA/vsomeip/archive/refs/tags/3.7.6.tar.gz -o /tmp/vsomeip.tgz \
    && mkdir -p /tmp/vsomeip-src \
    && tar -xzf /tmp/vsomeip.tgz -C /tmp/vsomeip-src --strip-components=1 \
    && cmake -S /tmp/vsomeip-src -B /tmp/vsomeip-src/build \
        -DCMAKE_BUILD_TYPE=Release \
        -DENABLE_SIGNAL_HANDLING=ON \
        -DDIAGNOSIS_ADDRESS=0x10 \
        -DCMAKE_INSTALL_PREFIX=/usr/local \
    && cmake --build /tmp/vsomeip-src/build -j"$(nproc)" \
    && cmake --install /tmp/vsomeip-src/build \
    && rm -rf /tmp/vsomeip-src/build

WORKDIR /app
COPY . .

RUN cmake -S . -B build && cmake --build build -j"$(nproc)"

ENV VSOMEIP_CONFIGURATION=/app/vsomeip.json

CMD ["./build/service"]