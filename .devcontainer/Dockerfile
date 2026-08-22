FROM debian:bookworm-slim AS base

ENV LANG=C.UTF-8 \
    LC_ALL=C.UTF-8

RUN apt-get update \
    && apt-get install -y --no-install-recommends \
        build-essential \
        cmake \
        ninja-build \
        clang-format \
        make \
        gdb \
        strace \
        netcat-openbsd \
        ca-certificates \
        openssh-client \
        curl \
        git \
        zip \
        unzip \
        pkg-config \
        linux-perf \
    && rm -rf /var/lib/apt/lists/*

RUN git clone --depth 1 https://github.com/microsoft/vcpkg /opt/vcpkg \
    && /opt/vcpkg/bootstrap-vcpkg.sh -disableMetrics \
    && ln -s /opt/vcpkg/vcpkg /usr/local/bin/vcpkg

ENV VCPKG_ROOT=/opt/vcpkg

WORKDIR /workspace

RUN curl -fsSL https://starship.rs/install.sh | sh -s -- -y \
   && echo 'eval "$(starship init bash)"' >> /etc/bash.bashrc

CMD ["bash"]
