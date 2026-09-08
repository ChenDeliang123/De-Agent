#!/usr/bin/env bash
set -euo pipefail

SCRIPT_SOURCE="${BASH_SOURCE[0]}"
while [ -L "${SCRIPT_SOURCE}" ]; do
  SCRIPT_DIR="$(cd -P "$(dirname "${SCRIPT_SOURCE}")" && pwd)"
  SCRIPT_SOURCE="$(readlink "${SCRIPT_SOURCE}")"
  case "${SCRIPT_SOURCE}" in
    /*) ;;
    *) SCRIPT_SOURCE="${SCRIPT_DIR}/${SCRIPT_SOURCE}" ;;
  esac
done
ROOT_DIR="$(cd -P "$(dirname "${SCRIPT_SOURCE}")/.." && pwd)"

BIN_DIR="${DE_BIN_DIR:-${HOME}/.npm-global/bin}"
mkdir -p "${BIN_DIR}"
ln -sf "${ROOT_DIR}/de" "${BIN_DIR}/de"
echo "installed: ${BIN_DIR}/de -> ${ROOT_DIR}/de"
echo "run 'de' from any directory; use 'de help' for commands"
