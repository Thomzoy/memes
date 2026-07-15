uv run python -m yt_dlp --js-runtimes node \
  --verbose \
  --cookies-from-browser chrome \
  --download-archive downloaded.txt \
  --ignore-errors \
  --extract-audio \
  --audio-format mp3 \
  --audio-quality 0 \
  --embed-metadata \
  -o "data/%(playlist_index)04d.%(ext)s" \
  "http://www.youtube.com/playlist?list=PLum-UT-EsfCWXirYrz4y-RI972xf8i-Eu"

ls -1 data/*.mp3 2>/dev/null | wc -l | tr -d ' ' > number.txt
