#!/usr/bin/env bash
set -euo pipefail

BIN_DIR="${DE_BIN_DIR:-${HOME}/.npm-global/bin}"
TARGET="${BIN_DIR}/de"
if [ -L "${TARGET}" ]; then
  rm -f "${TARGET}"
  echo "removed ${TARGET}"
else
  echo "not installed: ${TARGET}"
fi
