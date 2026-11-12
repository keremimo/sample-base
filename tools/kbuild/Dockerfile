# SPDX-License-Identifier: AGPL-3.0-only
# Builds kernel/ modules against the MPC's kernel (tools/build_kernel_modules.sh):
# the compiler and tools a prepared tree in .local/kbuild needs.
FROM ubuntu:24.04
RUN apt-get update -qq && DEBIAN_FRONTEND=noninteractive apt-get install -y -qq --no-install-recommends \
        gcc-13 make flex bison bc libssl-dev libelf-dev python3 kmod \
    && ln -sf /usr/bin/gcc-13 /usr/local/bin/gcc && rm -rf /var/lib/apt/lists/*
