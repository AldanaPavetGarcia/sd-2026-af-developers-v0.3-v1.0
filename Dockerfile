FROM alpine:3.20 AS builder

RUN apk add --no-cache \
    build-base \
    ninja \
    python3 \
    py3-pip

RUN python3 -m venv /opt/cmake \
    && /opt/cmake/bin/pip install --no-cache-dir "cmake>=3.30,<3.31"

ENV PATH="/opt/cmake/bin:${PATH}"

WORKDIR /workspace

COPY CMakeLists.txt CMakePresets.json ./
COPY include ./include
COPY lib ./lib
COPY src ./src
COPY examples ./examples

RUN cmake -S . -B build \
    -G Ninja \
    -DCMAKE_BUILD_TYPE=Release \ 
    -DEXODUS_BUILD_TESTS=OFF \
    -DEXODUS_BUILD_BENCHMARKS=OFF

RUN cmake --build build --target eop_server --parallel \
    && strip build/eop_server

FROM alpine:3.20 AS runtime

RUN apk add --no-cache libstdc++ busybox-extras \
    && addgroup -S exodus \
    && adduser -S exodus -G exodus

WORKDIR /app

COPY --from=builder /workspace/build/eop_server ./eop_server

RUN chown exodus:exodus /app/eop_server

USER exodus

EXPOSE 9026

ENTRYPOINT ["./eop_server"]
