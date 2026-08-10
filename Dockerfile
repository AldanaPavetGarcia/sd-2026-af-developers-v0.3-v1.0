FROM alpine:3.20 AS builder

RUN apk add --no-cache \
    build-base \
    ninja \
    python3 \
    py3-pip \
    git \
    curl \
    zip \
    unzip \
    tar \
    bash \
    linux-headers \
    pkgconf \
    perl

RUN python3 -m venv /opt/cmake \
    && /opt/cmake/bin/pip install --no-cache-dir "cmake>=3.30,<3.31"

ENV PATH="/opt/cmake/bin:${PATH}"

ENV VCPKG_FORCE_SYSTEM_BINARIES=1

# Install vcpkg
# Pinned to a fixed release tag (not `master` HEAD): an unpinned clone previously broke CI
# when vcpkg's SPDX-generation helper (scripts/cmake/z_vcpkg_spdx.cmake) started using
# `string(JSON ... STRING_ENCODE ...)`, a CMake subcommand our pinned CMake (<3.31) doesn't
# support. 2026.06.24 is the last release tag before that change landed.
ENV VCPKG_ROOT=/opt/vcpkg
RUN git clone --branch 2026.06.24 --depth 1 https://github.com/microsoft/vcpkg.git $VCPKG_ROOT \
    && $VCPKG_ROOT/bootstrap-vcpkg.sh -disableMetrics \
    && echo "set(VCPKG_BUILD_TYPE release)" >> $VCPKG_ROOT/triplets/x64-linux.cmake

# Limit parallel compilation jobs inside vcpkg to avoid OOM / computer crashes
ENV VCPKG_MAX_CONCURRENCY=2

WORKDIR /workspace

# Copy only vcpkg.json first so dependencies are cached in a separate Docker layer
COPY vcpkg.json ./

ARG EXODUS_BUILD_TESTS=OFF
ARG HPC_BUILD_TESTS=OFF

# Pre-install dependencies (only opentelemetry-cpp by default, gtest and benchmark if tests are enabled)
# We use BuildKit cache mounts to cache downloads and vcpkg compilation state
RUN --mount=type=cache,target=/root/.cache/vcpkg \
    --mount=type=cache,target=/opt/vcpkg/downloads \
    if [ "$EXODUS_BUILD_TESTS" = "ON" ] || [ "$HPC_BUILD_TESTS" = "ON" ]; then \
        $VCPKG_ROOT/vcpkg install --triplet x64-linux --x-feature=tests --x-feature=benchmarks; \
    else \
        $VCPKG_ROOT/vcpkg install --triplet x64-linux; \
    fi

# Copy the rest of the source code (changes to these will NOT trigger rebuilding of vcpkg dependencies)
COPY CMakeLists.txt CMakePresets.json ./
COPY include ./include
COPY lib ./lib
COPY src ./src
COPY examples ./examples
COPY tests ./tests
COPY hpc-engine ./hpc-engine

# Run cmake configuration. We set VCPKG_MANIFEST_INSTALL=OFF because the dependencies
# are already installed in /workspace/vcpkg_installed.
RUN cmake -S . -B build \
    -G Ninja \
    -DCMAKE_BUILD_TYPE=Release \
    -DEXODUS_BUILD_TESTS=OFF \
    -DEXODUS_BUILD_BENCHMARKS=OFF \
    -DHPC_BUILD_TESTS=OFF \
    -DHPC_BUILD_BENCHMARKS=OFF \
    -DVCPKG_MANIFEST_INSTALL=OFF \
    -DVCPKG_INSTALLED_DIR=/workspace/vcpkg_installed \
    -DCMAKE_TOOLCHAIN_FILE=$VCPKG_ROOT/scripts/buildsystems/vcpkg.cmake

# Build the executable, limiting parallel compiler processes to avoid OOM
RUN cmake --build build --target eop_server --parallel 2 \
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
