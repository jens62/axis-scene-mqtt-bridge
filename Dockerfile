ARG ARCH=aarch64
ARG VERSION=12.11.0
ARG UBUNTU_VERSION=24.04
ARG REPO=axisecp
ARG SDK=acap-native-sdk

FROM ${REPO}/${SDK}:${VERSION}-${ARCH}-ubuntu${UBUNTU_VERSION}

# libmosquitto is not part of the ACAP SDK sysroot: cross-compile it statically (no TLS).
ARG MOSQUITTO_VERSION=2.0.22
RUN . /opt/axis/acapsdk/environment-setup* && \
    cd /tmp && \
    curl -fsSL https://mosquitto.org/files/source/mosquitto-${MOSQUITTO_VERSION}.tar.gz | tar xz && \
    cd mosquitto-${MOSQUITTO_VERSION} && \
    make -C lib CROSS_COMPILE= WITH_TLS=no WITH_THREADING=yes WITH_SRV=no WITH_DOCS=no \
        WITH_STATIC_LIBRARIES=yes WITH_SHARED_LIBRARIES=no libmosquitto.a && \
    install -d /opt/mosquitto/lib /opt/mosquitto/include && \
    install -m 644 lib/libmosquitto.a /opt/mosquitto/lib/ && \
    install -m 644 include/mosquitto.h /opt/mosquitto/include/ && \
    rm -rf /tmp/mosquitto-${MOSQUITTO_VERSION}

COPY ./app /opt/app/
WORKDIR /opt/app
RUN . /opt/axis/acapsdk/environment-setup* && acap-build .
