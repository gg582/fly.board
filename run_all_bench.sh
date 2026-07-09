#!/bin/bash
set -euo pipefail

cd /home/yjlee/fly.board

# Tunables used for the benchmark suite. These values aim to minimize
# first-paint latency while keeping per-response overhead low:
#   - shell assets are inlined so the initial HTML needs no extra round trips
#   - CDN libraries and images stay external to avoid bloating every page
#   - TCP_CORK is skipped for responses that fit in a single MSS
export FLYBOARD_INLINE_SHELL=1
export FLYBOARD_INLINE_CDN=0
export FLYBOARD_INLINE_IMAGES=0
export FLYBOARD_CORK_THRESHOLD=1460

echo "========================================"
echo "Running C10K benchmark"
echo "========================================"
bash run_c10k_bench.sh

echo ""
echo "========================================"
echo "Running C100K benchmark"
echo "========================================"
bash run_c100k_bench.sh

echo ""
echo "========================================"
echo "Running C1M benchmark"
echo "========================================"
bash run_c1m_bench.sh

echo ""
echo "========================================"
echo "All benchmarks complete"
echo "========================================"
