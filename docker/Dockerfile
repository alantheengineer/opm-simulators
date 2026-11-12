# Copyright 2026 Dr Alan Tominey
# SPDX-License-Identifier: GPL-3.0-or-later
#
# flow with the step control server.
#
# Build from the root of opm-simulators:
#   docker build -f docker/Dockerfile -t opm-flow .
#
# Run a deck under control, with the token in the mounted folder:
#   docker run -v /path/to/case:/data -p 50051:50051 \
#     -e CONTROL_TOKEN_FILE=/data/token opm-flow /data/CASE.DATA --output-dir=/data/out
#
# See docker/entrypoint.sh for the settings.

FROM ubuntu:24.04 AS build

ARG DEBIAN_FRONTEND=noninteractive
COPY .github/apt-packages.txt /tmp/apt-packages.txt
RUN apt-get update \
    && xargs -a /tmp/apt-packages.txt apt-get install -y --no-install-recommends git ca-certificates \
    && rm -rf /var/lib/apt/lists/*

# Branch, tag or commit of each dependency.
ARG OPM_COMMON_REPO=https://github.com/alantheengineer/opm-common.git
ARG OPM_COMMON_REF=deploy
ARG OPM_GRID_REPO=https://github.com/OPM/opm-grid.git
ARG OPM_GRID_REF=master
# Parallel compile jobs. Each flow source needs 2 to 4 GB of memory.
ARG JOBS=4

COPY docker/fetch.sh /usr/local/bin/fetch
RUN fetch "$OPM_COMMON_REPO" "$OPM_COMMON_REF" /src/opm-common \
    && cmake -S /src/opm-common -B /build/common -G Ninja -DCMAKE_BUILD_TYPE=Release \
       -DBUILD_TESTING=OFF -DOPM_ENABLE_PYTHON=OFF \
    && cmake --build /build/common --parallel "$JOBS"

RUN fetch "$OPM_GRID_REPO" "$OPM_GRID_REF" /src/opm-grid \
    && cmake -S /src/opm-grid -B /build/grid -G Ninja -DCMAKE_BUILD_TYPE=Release \
       -DBUILD_TESTING=OFF -Dopm-common_DIR=/build/common \
    && cmake --build /build/grid --parallel "$JOBS"

COPY . /src/opm-simulators
RUN cmake -S /src/opm-simulators -B /build/sim -G Ninja -DCMAKE_BUILD_TYPE=Release \
       -DBUILD_TESTING=OFF -DOPM_ENABLE_PYTHON=OFF -DUSE_OPENCL=OFF -DUSE_GPU_BRIDGE=OFF \
       -DOPM_ENABLE_CONTROL_SERVER=ON \
       -Dopm-common_DIR=/build/common -Dopm-grid_DIR=/build/grid \
    && cmake --build /build/sim --parallel "$JOBS" --target flow control_step_client \
    && client=$(find /build/sim -name control_step_client -type f -perm -u+x | head -n 1) \
    && install -D -t /opt/opm/bin /build/sim/bin/flow "$client" \
    && install -D /src/opm-simulators/docker/entrypoint.sh /opt/opm/bin/opm-entrypoint \
    && /src/opm-simulators/docker/runtime-packages.sh /opt/opm/bin/flow /opt/opm/bin/control_step_client \
       > /opt/opm/runtime-packages.txt

FROM ubuntu:24.04

ARG DEBIAN_FRONTEND=noninteractive
COPY --from=build /opt/opm /opt/opm
RUN apt-get update \
    && xargs -a /opt/opm/runtime-packages.txt apt-get install -y --no-install-recommends openmpi-bin \
    && rm -rf /var/lib/apt/lists/* \
    && if ldd /opt/opm/bin/flow | grep "not found"; then exit 1; fi \
    && useradd --create-home opm

USER opm
WORKDIR /home/opm
ENV PATH=/opt/opm/bin:$PATH \
    OMPI_MCA_btl_vader_single_copy_mechanism=none
EXPOSE 50051
ENTRYPOINT ["/opt/opm/bin/opm-entrypoint"]
