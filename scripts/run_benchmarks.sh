#!/usr/bin/env bash
# Rebuilds in Release and writes every benchmark result in the README to docs/results/.
# Needs ~2 GB of free disk for the generated feed and at least 4 cores for the pinned runs.
set -euo pipefail
cd "$(dirname "$0")/.."
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release >/dev/null
cmake --build build -j >/dev/null
out=docs/results
mkdir -p "$out"
feed=${FEED:-/tmp/itch_synthetic_50m.itch}

./build/itch_gen --messages 50000000 --out "$feed" > "$out/generator.json"

: > "$out/replay.jsonl"
for opts in "--parse-only" "--prefetch 0 --no-huge-pages" "--prefetch 0" "--prefetch 8"; do
  for run in 1 2 3; do
    ./build/itch_replay --file "$feed" $opts >> "$out/replay.jsonl"
  done
done
./build/itch_replay --file "$feed" --prefetch 8 --latency > "$out/replay_latency.json"

: > "$out/queue.jsonl"
for run in 1 2 3 4 5; do
  ./build/queue_bench --pin | tr -d '\n' >> "$out/queue.jsonl"
  echo >> "$out/queue.jsonl"
done

: > "$out/feed.jsonl"
for opts in "--rate 1000000 --pin --no-prefetch" "--rate 1000000 --pin" "--rate 3000000 --pin" "--rate 0 --pin"; do
  for run in 1 2 3; do
    ./build/feed_bench --messages 5000000 $opts | tr -d '\n' >> "$out/feed.jsonl"
    echo >> "$out/feed.jsonl"
  done
done
rm -f "$feed"
echo "results in $out"
