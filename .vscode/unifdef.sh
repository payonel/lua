#!/bin/bash

set -e
set -x

if [[ $# -eq 0 ]]; then
  echo "Usage: $0 <file1> [file2 ...]"
  exit 1
fi

vscode_dir=$(dirname $0)

for file in "$@"; do
  if [[ ! -f "$file" ]]; then
    echo "Skipping $file: not a file"
    continue
  fi
  
  sed 's/\t/  /g' -i "$file"
  for i in {1..6}; do
    sed -e ':a' -e 'N' -e '$!ba' -e 's/\\\n */ /g' -i "$file"
  done
done

unifdef -m -k -B -f ${vscode_dir}/filter.defs "$@"
