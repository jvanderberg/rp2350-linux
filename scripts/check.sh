#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/.."
. scripts/preflight.sh
need_tool python3 'Install Python 3.9 or newer.'
exec python3 scripts/check.py "$@"
