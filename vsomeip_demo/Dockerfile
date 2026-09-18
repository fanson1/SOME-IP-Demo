FROM ubuntu:24.04

RUN apt-get update && apt-get install -y --no-install-recommends \
        build-essential cmake git \
        libboost-dev libboost-system-dev libboost-filesystem-dev libboost-thread-dev \
    && rm -rf /var/lib/apt/lists/*

RUN git clone --depth 1 --branch v3.5.0 \
        https://github.com/COVESA/vsomeip /tmp/vsomeip \
    && cmake -S /tmp/vsomeip -B /tmp/vsomeip/build \
        -DCMAKE_BUILD_TYPE=Release \
        -DENABLE_SIGNAL_HANDLING=ON \
        -DDIAGNOSIS_ADDRESS=0x10 \
        -DCMAKE_INSTALL_PREFIX=/usr/local \
    && cmake --build /tmp/vsomeip/build -j"$(nproc)" \
    && cmake --install /tmp/vsomeip/build \
    && rm -rf /tmp/vsomeip/build

WORKDIR /app
COPY . .

RUN cmake -S . -B build && cmake --build build -j"$(nproc)"

ENV VSOMEIP_CONFIGURATION=/app/vsomeip.json

CMD ["./build/service"]