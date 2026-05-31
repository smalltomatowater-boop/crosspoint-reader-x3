#!/bin/sh
set -e

PORT=${1:-/dev/cu.usbmodem1301}

/opt/homebrew/bin/pio run -t upload --upload-port "$PORT"
