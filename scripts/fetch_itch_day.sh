#!/usr/bin/env bash
# Downloads one day of real NASDAQ TotalView-ITCH 5.0 data (about 4 GB compressed, 8 GB
# uncompressed) from NASDAQ's public sample directory, then replays it.
#   scripts/fetch_itch_day.sh [12302019]
set -euo pipefail
day=${1:-12302019}
file="${day}.NASDAQ_ITCH50"
url="https://emi.nasdaq.com/ITCH/Nasdaq%20ITCH/${file}.gz"
if [[ ! -f "$file" ]]; then
  curl -fL --retry 3 -o "${file}.gz" "$url"
  gunzip "${file}.gz"
fi
cd "$(dirname "$0")/.."
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release >/dev/null
cmake --build build -j >/dev/null
./build/itch_replay --file "$OLDPWD/$file" --expected-orders 4000000
