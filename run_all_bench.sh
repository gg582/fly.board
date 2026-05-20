#!/bin/bash
set -euo pipefail

cd /home/yjlee/fly.board

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
