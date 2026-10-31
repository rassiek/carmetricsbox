# CarMetrics development environment: firmware build + ESP32 emulator (QEMU)
# + web UI tests - the same toolchain used during development.
#
#   docker build -t carmetrics-dev devenv
#   docker run --rm -v "${PWD}:/work" carmetrics-dev tools/cm.sh selftest
#
ARG BASE_IMAGE=ubuntu:24.04
FROM ${BASE_IMAGE}
ARG WITH_WEBTEST=1
ENV DEBIAN_FRONTEND=noninteractive

RUN apt-get update && apt-get install -y --no-install-recommends \
      ca-certificates curl git xz-utils bzip2 unzip zip python3 python3-pil python3-serial \
      libslirp0 libsdl2-2.0-0 libglib2.0-0 libgcrypt20 \
    && rm -rf /var/lib/apt/lists/*

COPY versions.env setup-toolchain.sh /opt/cm/
RUN bash /opt/cm/setup-toolchain.sh

# Optional: headless Chromium for the web UI tests (adds ~500 MB)
RUN if [ "$WITH_WEBTEST" = "1" ]; then \
      . /opt/cm/versions.env && \
      curl -fsSL "https://nodejs.org/dist/v${NODE_VERSION}/node-v${NODE_VERSION}-linux-x64.tar.xz" \
        | tar xJ -C /usr/local --strip-components=1 && \
      npm install -g "playwright@${PLAYWRIGHT_VERSION}" && \
      npx -y "playwright@${PLAYWRIGHT_VERSION}" install --with-deps chromium && \
      rm -rf /var/lib/apt/lists/* /root/.npm; \
    fi

ENV CM_PREFIX=/opt/cm \
    ARDUINO_DIRECTORIES_USER=/opt/cm/Arduino \
    ARDUINO_DIRECTORIES_DATA=/opt/cm/arduino15 \
    NODE_PATH=/usr/local/lib/node_modules \
    PATH=/opt/cm/bin:$PATH
WORKDIR /work
CMD ["bash"]
