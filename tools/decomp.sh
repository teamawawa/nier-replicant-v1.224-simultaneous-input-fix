#!/bin/sh
# Decompile one or more VAs (hex, e.g. 0x1403d3f20) from the Ghidra project.
S=/tmp/claude-1000/-mnt-data-code-nier-replicant-kbm-controller/91ab27a8-b8bb-46ea-98fb-d5744f639670/scratchpad
ghidra-analyzeHeadless "$S/ghidra_proj" nier \
  -process "NieR Replicant ver.1.22474487139.exe" -noanalysis -readOnly \
  -scriptPath "$(dirname "$0")/../scripts/ghidra" -postScript Decomp.java "$@" 2>&1 \
  | sed -n '/==== /,$p' | sed 's/^INFO  //; s/ (GhidraScript)  *$//' \
  | sed '/(HeadlessAnalyzer)/d'
