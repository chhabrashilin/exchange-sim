#!/usr/bin/env bash
# Fetches Liquibook (https://github.com/enewhuis/liquibook) for the head-to-head benchmark.
# It is deliberately not vendored: it keeps its own license and this repository stays dependency-free.
#
#   scripts/fetch_liquibook.sh [dest]        then:  cmake -DEXSIM_LIQUIBOOK_DIR=<dest> ...
set -euo pipefail
DEST=${1:-third_party/liquibook}
if [ ! -d "$DEST/.git" ]; then
  git clone --depth 1 https://github.com/enewhuis/liquibook.git "$DEST"
fi
git -C "$DEST" log -1 --format='liquibook at %H (%cd)'
