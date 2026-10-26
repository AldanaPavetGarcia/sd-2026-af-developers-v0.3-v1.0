# EOP edge device: the native_sim build of the firmware (ADR-014), so a simulated bunker runs in
# docker-compose and Kubernetes like any other service. Build from the repository root:
#   docker build -f firmware/eop-edge/Dockerfile -t eop-edge:local .
# Each container is one device; --node-id, --bunker-name, --broker-host and --broker-port
# override the prj.conf values (see firmware/eop-edge/README.md).

# Zephyr 4.4 needs Python >= 3.12, newer than Debian 12's 3.11: this image is Debian 12 (same glibc
# as the runtime below) with Python 3.12.
FROM python:3.12-slim-bookworm AS build

RUN apt-get update \
    && apt-get install -y --no-install-recommends \
        ca-certificates git cmake ninja-build gcc libc6-dev make file \
    && rm -rf /var/lib/apt/lists/*

ENV EOP_ZEPHYR_WS=/zephyr-ws
WORKDIR /src
# The workspace depends only on the pin, so it stays cached while the sources change.
COPY firmware/eop-edge/west.yml firmware/eop-edge/requirements-test.txt firmware/eop-edge/
COPY scripts/zephyr-setup.sh scripts/
RUN bash scripts/zephyr-setup.sh

COPY firmware/eop-edge firmware/eop-edge
RUN . /zephyr-ws/.venv/bin/activate \
    && ZEPHYR_BASE=/zephyr-ws/zephyr ZEPHYR_TOOLCHAIN_VARIANT=host \
       west build -b native_sim/native/64 -d /build firmware/eop-edge

# glibc runtime matching the build stage (Debian 12), no shell, non-root.
FROM gcr.io/distroless/cc-debian12:nonroot
COPY --from=build /build/zephyr/zephyr.exe /eop-edge
ENTRYPOINT ["/eop-edge"]
