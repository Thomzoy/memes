#!/usr/bin/env bash
set -euo pipefail

GIST_ID="25c4117e75a9777e8464f5136f6a7a42"
OUTPUT="instagram_saved.txt"

curl -s "https://api.github.com/gists/${GIST_ID}" \
  | jq -r '.files["instagram_saved.txt"].content' \
  > "$OUTPUT"

echo "Saved $OUTPUT"