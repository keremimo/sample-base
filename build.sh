#!/bin/sh
set -eu
cd "$(dirname "$0")"
python3 tools/dependencies.py
docker build --platform linux/arm64 -t sample-base-builder tools
docker run --rm --platform linux/arm64 -v "$PWD:/work" sample-base-builder \
    sh -c 'make -j4 && python3 -m unittest discover -s tests -v && build/sample-base --self-test build/test.wav && build/sample-base --preview build/preview.png'
