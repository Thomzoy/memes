uv run python -m yt_dlp \
  --flat-playlist \
  --print webpage_url \
  "http://www.youtube.com/playlist?list=PLum-UT-EsfCWXirYrz4y-RI972xf8i-Eu" \
  > sources.txt

cat instagram_saved.txt >> sources.txt

uv run python -m yt_dlp --js-runtimes node \
  --verbose \
  --cookies-from-browser chrome \
  --download-archive downloaded.txt \
  --batch-file sources.txt \
  --ignore-errors \
  --extract-audio \
  --audio-format mp3 \
  --audio-quality 0 \
  --postprocessor-args "ExtractAudio:-af loudnorm=I=-16:TP=-1.5:LRA=11 -ar 44100" \
  --embed-metadata \
  -o "data/%(autonumber)04d.%(ext)s"

ls -1 data/*.mp3 2>/dev/null | wc -l | tr -d ' ' > number.txt
