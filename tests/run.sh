#!/bin/sh
# Runs the host-side unit tests inside the ACAP SDK image (needs gcc, jansson and glib from apt).
set -e
cd "$(dirname "$0")/.."
docker run --rm --platform=linux/amd64 -v "$PWD":/src:ro axisecp/acap-native-sdk:12.11.0-aarch64-ubuntu24.04 sh -c '
  apt-get update -qq >/dev/null && apt-get install -y -qq gcc libjansson-dev libglib2.0-dev >/dev/null &&
  gcc -Wall -Wextra -Werror -fsanitize=address,undefined -g \
      $(pkg-config --cflags glib-2.0) /src/tests/test_dedupe.c /src/app/src/dedupe.c \
      $(pkg-config --libs glib-2.0) -ljansson -lm -o /tmp/test_dedupe &&
  /tmp/test_dedupe'
