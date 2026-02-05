#!/bin/bash

set -e
set -x

if [[ $# -eq 0 ]]; then
  echo "Usage: $0 <file1> [file2 ...]"
  exit 1
fi

clang-format -i "$@"
