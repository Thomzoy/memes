# Ensure the data directory exists
mkdir -p data

# Get existing videos from the release
echo "Getting existing videos from the release"
#gh release download --pattern "*.mp3" --dir data_old --repo https://github.com/Thomzoy/memes

# Get the instagram saved videos
# Note: no `set -e` here on purpose — the script should keep going even if a step fails
set -uo pipefail

echo "Getting instagram saved videos list"

GIST_ID="25c4117e75a9777e8464f5136f6a7a42"
OUTPUT="instagram_saved.txt"

gh api "gists/${GIST_ID}" \
  --jq '.files["instagram_saved.txt"].content' \
  > "$OUTPUT"

echo "Saved $OUTPUT"

# Install dependencies
uv sync

echo "Getting youtube playlist"

# Get the youtube playlist
uv run python -m yt_dlp \
  --flat-playlist \
  --print webpage_url \
  "http://www.youtube.com/playlist?list=PLum-UT-EsfCWXirYrz4y-RI972xf8i-Eu" \
  > sources.txt

# Combine the instagram saved videos with the youtube playlist
cat instagram_saved.txt >> sources.txt

echo "Downloading videos"
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
  -o "data/%(id)s.%(ext)s"

echo "Renumbering videos"
# Renumber the newly downloaded files so they follow the sequential IDs used in
# data_old (4-digit, 0-left-padded), starting at 1 + the biggest ID from data_old.
# The IDs in data_old are a contiguous sequence, so the biggest ID equals the count.
max_id=$(ls -1 data_old/*.mp3 2>/dev/null | wc -l | tr -d ' ')

next_id=$((max_id + 1))
# Rename in download order (oldest first) so IDs follow the order videos were fetched.
for f in $(ls -1tr data/*.mp3 2>/dev/null); do
  new=$(printf "data/%04d.mp3" "$next_id")
  mv "$f" "$new"
  next_id=$((next_id + 1))
done

# Move all previously released files from data_old into data.
if [ -d data_old ]; then
  cp data_old/*.mp3 data/ 2>/dev/null || true
  # rmdir data_old 2>/dev/null || true
fi

echo "Counting videos"
# Count the number of videos
ls -1 data/*.mp3 2>/dev/null | wc -l | tr -d ' ' > number.txt

echo "Downloaded $(cat number.txt) videos"
