#!/bin/bash
# Produce analysis/rea_<name>_raw.json via REA's Ghidra lane for a wrapped ELF in work/
source ~/tools/rea-env.sh
cd "$(dirname "$0")/.."
for n in "$@"; do
  rea search work/$n.elf --kind procedures --pattern .  --mode regex --format json > work/rea_$n.json 2> work/rea_$n.log
  echo "$n done: $?" >> work/rea_status.txt
done
