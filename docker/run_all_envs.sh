#!/usr/bin/env bash
set -euo pipefail

cd /work

# Match .github/workflows/PR_All_envs.yml matrix envs
DEFAULT_ENVS=(
  m5stack-cardputer
  m5stack-cardputer-adv
  m5stack-cplus2
  m5stack-cplus1_1
  m5stack-core
  m5stack-core2
  m5stack-cores3
  m5stack-dinmeter
  m5stack-sticks3
  lilygo-t-embed-cc1101
  lilygo-t-embed
  lilygo-t-display-s3
  lilygo-t-display-ttgo
  xk404
)

if [[ -n "${PIO_ENVS:-}" ]]; then
  # Space-separated list
  read -r -a ENVS <<<"${PIO_ENVS}"
else
  ENVS=("${DEFAULT_ENVS[@]}")
fi

source "${IDF_PATH}/export.sh"
idf.py --version

JOBS="${PIO_JOBS:-1}"

failed=()

for ((i = 0; i < ${#ENVS[@]}; i += JOBS)); do
  batch=("${ENVS[@]:i:JOBS}")

  pids=()
  pid_envs=()
  for e in "${batch[@]}"; do
    echo "===== ESP-IDF build ${e} ====="
    python3 tools/board.py "${e}" build &
    pids+=("$!")
    pid_envs+=("${e}")
  done

  for idx in "${!pids[@]}"; do
    pid="${pids[$idx]}"
    e="${pid_envs[$idx]}"
    if ! wait "${pid}"; then
      failed+=("${e}")
    fi
  done
done

if ((${#failed[@]} > 0)); then
  echo "Build failures: ${failed[*]}" >&2
  exit 1
fi

exit 0
