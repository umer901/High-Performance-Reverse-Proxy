#!/usr/bin/env bash
set -euo pipefail

PID="${1:-}"
DURATION="${2:-30}"

if [[ -z "${PID}" ]]; then
  echo "usage: $0 <hprp-pid> [duration-seconds]" >&2
  exit 2
fi

mkdir -p profiles
perf record -F 99 -g -p "${PID}" -- sleep "${DURATION}"
perf report --stdio > "profiles/perf-report-${PID}.txt"
echo "wrote profiles/perf-report-${PID}.txt"
