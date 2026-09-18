FROM ubuntu:24.04

RUN apt-get update && apt-get install -y --no-install-recommends \
        build-essential cmake libvsomeip-dev \
    && rm -rf /var/lib/apt/lists/*

WORKDIR /app
COPY . .

RUN cmake -S . -B build \
    && cmake --build build -j"$(nproc)"

ENV VSOMEIP_CONFIGURATION=/app/vsomeip.json

CMD ["./build/service"]