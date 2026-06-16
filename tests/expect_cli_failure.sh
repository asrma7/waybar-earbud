#!/usr/bin/env sh
set -eu

binary="$1"
expected_status="$2"
expected_text="$3"
shift 3

set +e
output="$("$binary" "$@" 2>&1)"
status="$?"
set -e

printf '%s\n' "$output"

if [ "$status" -ne "$expected_status" ]; then
  echo "expected exit status $expected_status, got $status" >&2
  exit 1
fi

case "$output" in
  *"$expected_text"*)
    ;;
  *)
    echo "expected output to contain: $expected_text" >&2
    exit 1
    ;;
esac
