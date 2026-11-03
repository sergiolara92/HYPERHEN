#!/usr/bin/env bash
set -euo pipefail
task_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
python3 "${task_root}/scripts/test_plugin_game_runtime.py" "${1:-${task_root}/build-test}"
