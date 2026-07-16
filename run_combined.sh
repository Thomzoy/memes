# Get existing videos from the release
gh release download --pattern "*.mp3" --dir data --repo https://github.com/Thomzoy/memes

# Get the instagram saved videos
set -euo pipefail

GIST_ID="25c4117e75a9777e8464f5136f6a7a42"
OUTPUT="instagram_saved.txt"

gh api "gists/${GIST_ID}" \
  --jq '.files["instagram_saved.txt"].content' \
  > "$OUTPUT"

echo "Saved $OUTPUT"

# Install dependencies
uv sync

# Get the youtube playlist
uv run python -m yt_dlp \
  --flat-playlist \
  --print webpage_url \
  "http://www.youtube.com/playlist?list=PLum-UT-EsfCWXirYrz4y-RI972xf8i-Eu" \
  > sources.txt

# Combine the instagram saved videos with the youtube playlist
cat instagram_saved.txt >> sources.txt

# Download the videos
# Browser cookies only make sense locally; disable in CI with USE_BROWSER_COOKIES=0
COOKIES_ARGS=()
if [ "${USE_BROWSER_COOKIES:-1}" = "1" ]; then
  COOKIES_ARGS+=(--cookies-from-browser chrome)
fi

uv run python -m yt_dlp --js-runtimes node \
  --verbose \
  "${COOKIES_ARGS[@]}" \
  --download-archive downloaded.txt \
  --batch-file sources.txt \
  --ignore-errors \
  --extract-audio \
  --audio-format mp3 \
  --audio-quality 0 \
  --postprocessor-args "ExtractAudio:-af loudnorm=I=-16:TP=-1.5:LRA=11 -ar 44100" \
  --embed-metadata \
  -o "data/%(autonumber)04d.%(ext)s"

# Count the number of videos downloaded
ls -1 data/*.mp3 2>/dev/null | wc -l | tr -d ' ' > number.txt


https://api.github.com/gists/25c4117e75a9777e8464f5136f6a7a42