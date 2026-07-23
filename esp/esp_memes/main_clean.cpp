// main_clean.cpp — tidied rewrite of src/main.cpp.
//
// To use it, replace the contents of src/main.cpp with this file (PlatformIO
// builds every file under src/, so only one may define setup()/loop()).
//
// Startup-latency improvements over the original:
//  1. Wi-Fi modem sleep is disabled (lower latency on every packet) and the
//     connect poll runs every 50 ms instead of 500 ms, so we no longer waste
//     up to ~450 ms after association. Wi-Fi association is also started
//     before audio/I2S init so the two overlap.
//  2. The first track is selected and pre-buffered (paused) right after boot.
//     A button press just unpauses the decoder, so playback is effectively
//     instant instead of paying DNS + two TLS handshakes (github.com plus the
//     CDN redirect) + buffering, which used to cost 2-4 s per press.
//  3. When a track finishes, the next random track is pre-buffered
//     immediately, so every subsequent press is instant as well.
//  4. Playback starts once 8 KB is buffered instead of 16 KB.
//  5. Reconnects resume with an HTTP Range request from the current offset
//     instead of re-downloading (and replaying) the file from byte 0.
//  6. The track-count fetch reuses the global TLS client and HTTPClient
//     instead of allocating a second ~40 KB TLS context on the heap.

#include <Arduino.h>
#include <WiFi.h>
#include <HTTPClient.h>
#include <BackgroundAudio.h>
#include <ESP32I2SAudio.h>
#include <OneButton.h>
#include <algorithm>
#include "secrets.h"

// ---------------------------------------------------------------------------
// Configuration
// ---------------------------------------------------------------------------

constexpr uint32_t WIFI_CONNECT_TIMEOUT_MS = 20000;
constexpr uint32_t WIFI_POLL_INTERVAL_MS = 50;
constexpr uint32_t RECONNECT_BACKOFF_MS = 1000;
constexpr uint32_t HTTP_CONNECT_TIMEOUT_MS = 10000;
constexpr uint32_t HTTP_READ_TIMEOUT_MS = 15000;

constexpr size_t STREAM_BUFFER_SIZE = 32 * 1024;
constexpr size_t CHUNK_BUFFER_SIZE = 1024;
// Pause playback when the buffer drops below LOW_WATERMARK; start (or resume)
// once START_WATERMARK bytes are available.
constexpr size_t LOW_WATERMARK = 1024;
constexpr size_t START_WATERMARK = 8 * 1024;

constexpr int BUTTON_PIN = 32;
constexpr int POT_PIN = 34;
constexpr uint32_t POT_READ_INTERVAL_MS = 200;

// MAX98357 I2S wiring
constexpr int I2S_BCLK_PIN = 27;
constexpr int I2S_LRCK_PIN = 26;
constexpr int I2S_DOUT_PIN = 25;

const char* const STREAM_URL_FMT =
    "https://github.com/Thomzoy/memes/releases/latest/download/%04d.mp3";
const char* const TRACK_COUNT_URL =
    "https://github.com/Thomzoy/memes/releases/latest/download/number.txt";
const char* const GITHUB_PAT = "";  // Optional: token for private content
constexpr int FALLBACK_TRACK_COUNT = 60;

// ---------------------------------------------------------------------------
// Globals
// ---------------------------------------------------------------------------

ESP32I2SAudio i2sAudio(I2S_BCLK_PIN, I2S_LRCK_PIN, I2S_DOUT_PIN);
BackgroundAudioMP3Class<RawDataBuffer<STREAM_BUFFER_SIZE>> mp3(i2sAudio);
NetworkClientSecure tlsClient;
HTTPClient http;
OneButton button(BUTTON_PIN, true, true);

uint8_t readBuffer[CHUNK_BUFFER_SIZE];
char streamUrl[128] = {0};
int trackCount = FALLBACK_TRACK_COUNT;

bool playRequested = false;    // user wants to hear the current track
bool streamExhausted = false;  // whole HTTP payload has been received
int32_t streamTotalBytes = -1; // total file size, -1 if unknown
size_t streamBytesRead = 0;    // bytes received so far (across reconnects)
uint32_t nextReconnectMs = 0;

// ---------------------------------------------------------------------------
// Wi-Fi
// ---------------------------------------------------------------------------

void startWifiConnect() {
  WiFi.mode(WIFI_STA);
  WiFi.setSleep(false);  // modem power-save adds latency to every packet
  WiFi.begin(APP_WIFI_SSID, APP_WIFI_PASSWORD);
}

bool waitForWifi() {
  const uint32_t start = millis();
  while (!WiFi.isConnected() && (millis() - start) < WIFI_CONNECT_TIMEOUT_MS) {
    delay(WIFI_POLL_INTERVAL_MS);
  }
  if (WiFi.isConnected()) {
    Serial.printf("Wi-Fi connected, IP: %s\n", WiFi.localIP().toString().c_str());
    return true;
  }
  Serial.println("Wi-Fi connection timeout.");
  return false;
}

// ---------------------------------------------------------------------------
// HTTP streaming
// ---------------------------------------------------------------------------

void applyRequestHeaders(HTTPClient& client) {
  client.setConnectTimeout(HTTP_CONNECT_TIMEOUT_MS);
  client.setTimeout(HTTP_READ_TIMEOUT_MS);
  client.setFollowRedirects(HTTPC_FORCE_FOLLOW_REDIRECTS);
  client.setReuse(false);
  client.addHeader("User-Agent", "esp32-background-audio");
  if (strlen(GITHUB_PAT) > 0) {
    client.addHeader("Authorization", String("Bearer ") + GITHUB_PAT);
  }
}

void disconnectStream() {
  http.end();
  tlsClient.stop();
}

void fetchTrackCount() {
  Serial.printf("Fetching track count from '%s'...\n", TRACK_COUNT_URL);
  if (!http.begin(tlsClient, TRACK_COUNT_URL)) {
    Serial.println("Failed to begin track count request.");
    return;
  }
  applyRequestHeaders(http);

  int parsed = 0;
  const int code = http.GET();
  if (code == HTTP_CODE_OK) {
    String payload = http.getString();
    payload.trim();
    parsed = payload.toInt();
  } else {
    Serial.printf("Track count fetch failed, HTTP code: %d\n", code);
  }
  disconnectStream();

  if (parsed > 0) {
    trackCount = parsed;
    Serial.printf("Track count: %d\n", trackCount);
  } else {
    Serial.printf("Using fallback track count: %d\n", trackCount);
  }
}

// Picks a new random track and resets streaming state. When `play` is false
// the track is only pre-buffered; a button press will start it instantly.
void startTrack(bool play) {
  disconnectStream();
  mp3.pause();
  mp3.flush();  // drop any leftover audio from the previous track

  const int track = random(1, trackCount + 1);
  snprintf(streamUrl, sizeof(streamUrl), STREAM_URL_FMT, track);
  Serial.printf("Selected track %d/%d: %s\n", track, trackCount, streamUrl);

  streamExhausted = false;
  streamTotalBytes = -1;
  streamBytesRead = 0;
  nextReconnectMs = 0;
  playRequested = play;
}

bool ensureStreamConnected() {
  if (http.connected()) {
    return true;
  }
  if (millis() < nextReconnectMs) {
    return false;
  }

  Serial.printf("Connecting to '%s' (offset %u)...\n", streamUrl,
                static_cast<unsigned>(streamBytesRead));
  disconnectStream();

  if (!http.begin(tlsClient, streamUrl)) {
    Serial.println("HTTP begin failed.");
    nextReconnectMs = millis() + RECONNECT_BACKOFF_MS;
    return false;
  }
  applyRequestHeaders(http);

  // Resume from the current offset instead of replaying the file from 0.
  if (streamBytesRead > 0) {
    char range[40];
    snprintf(range, sizeof(range), "bytes=%u-", static_cast<unsigned>(streamBytesRead));
    http.addHeader("Range", range);
  }

  const int code = http.GET();
  if (code == HTTP_CODE_PARTIAL_CONTENT) {
    streamTotalBytes = (http.getSize() > 0)
                           ? static_cast<int32_t>(streamBytesRead) + http.getSize()
                           : -1;
  } else if (code == HTTP_CODE_OK) {
    if (streamBytesRead > 0) {
      // Server ignored the Range header; restart from a clean buffer.
      mp3.flush();
      streamBytesRead = 0;
    }
    streamTotalBytes = http.getSize();
  } else {
    Serial.printf("HTTP GET failed, code: %d\n", code);
    disconnectStream();
    nextReconnectMs = millis() + RECONNECT_BACKOFF_MS;
    return false;
  }

  Serial.println("HTTP stream connected.");
  return true;
}

void finishStream(const char* reason) {
  streamExhausted = true;
  Serial.printf("Stream complete (%s), %u bytes; draining MP3 buffer.\n", reason,
                static_cast<unsigned>(streamBytesRead));
  disconnectStream();
}

void pumpStreamToDecoder() {
  WiFiClient* stream = http.getStreamPtr();
  if (!stream) {
    return;
  }

  while (true) {
    const size_t writable = mp3.availableForWrite();
    const int available = stream->available();
    if (writable == 0 || available <= 0) {
      break;
    }

    const size_t toRead =
        std::min({static_cast<size_t>(available), writable, CHUNK_BUFFER_SIZE});
    const int read = stream->read(readBuffer, toRead);
    if (read <= 0) {
      break;
    }

    mp3.write(readBuffer, read);
    streamBytesRead += static_cast<size_t>(read);

    if (streamTotalBytes > 0 &&
        streamBytesRead >= static_cast<size_t>(streamTotalBytes)) {
      finishStream("payload fully received");
      return;
    }
  }

  // EOF fallback for responses without a Content-Length. When the length is
  // known, a premature close is handled by a Range reconnect instead.
  if (streamTotalBytes <= 0 && streamBytesRead > 0 && !stream->connected() &&
      !stream->available()) {
    finishStream("closed by server");
  }
}

// ---------------------------------------------------------------------------
// Controls
// ---------------------------------------------------------------------------

void onButtonClick() {
  Serial.println("Button pressed.");
  if (streamUrl[0] == '\0') {
    return;  // Wi-Fi never came up, nothing armed yet
  }
  if (playRequested) {
    Serial.println("Switching to another random track...");
    startTrack(true);
  } else {
    playRequested = true;  // track is pre-buffered: starts on the next loop pass
  }
}

void updateVolumeFromPot() {
  static uint32_t lastReadMs = 0;
  static int lastStep = -1;

  if (millis() - lastReadMs < POT_READ_INTERVAL_MS) {
    return;
  }
  lastReadMs = millis();

  int sum = 0;
  for (int i = 0; i < 4; ++i) {
    sum += analogRead(POT_PIN);
  }
  const int raw = sum / 4;
  const int step = map(raw, 0, 4094, 20, 0);  // inverted pot, 21 steps
  if (step != lastStep) {
    lastStep = step;
    const float gain = static_cast<float>(step) / 10.0f;  // 0.0 .. 2.0
    mp3.setGain(gain);
    Serial.printf("volume: raw=%d gain=%.1f\n", raw, gain);
  }
}

void logStreamStats() {
  static uint32_t lastMs = 0;
  if (millis() - lastMs < 1000) {
    return;
  }
  lastMs = millis();
  Serial.printf("buffer=%d frames=%lu underflows=%lu errors=%lu dumps=%lu read=%u/%d\n",
                mp3.available(), mp3.frames(), mp3.underflows(), mp3.errors(),
                mp3.dumps(), static_cast<unsigned>(streamBytesRead),
                static_cast<int>(streamTotalBytes));
}

// ---------------------------------------------------------------------------
// Arduino entry points
// ---------------------------------------------------------------------------

void setup() {
  Serial.begin(115200);

  // Start Wi-Fi association first; it proceeds while the rest initialises.
  startWifiConnect();

  button.attachClick(onButtonClick);
  pinMode(POT_PIN, INPUT);
  analogReadResolution(12);
  randomSeed(esp_random());

  tlsClient.setInsecure();
  mp3.setGain(0.5f);
  mp3.begin();
  mp3.pause();  // stay silent until the button is pressed

  if (!waitForWifi()) {
    Serial.println("Cannot stream without Wi-Fi; retrying in loop.");
    return;
  }

  fetchTrackCount();
  startTrack(false);  // pre-buffer the first track for an instant start
  Serial.println("Ready: press the button to play.");
}

void loop() {
  button.tick();
  updateVolumeFromPot();

  if (!WiFi.isConnected()) {
    Serial.println("Wi-Fi lost, reconnecting...");
    disconnectStream();
    startWifiConnect();
    if (waitForWifi() && streamUrl[0] == '\0') {
      // Setup never completed (booted without Wi-Fi): finish it now.
      fetchTrackCount();
      startTrack(false);
    }
    return;
  }

  if (streamUrl[0] == '\0') {
    delay(100);
    return;
  }

  // Keep the decoder buffer fed. While idle (pre-buffered, waiting for the
  // button) a dead connection is left alone; playback resumes it via Range.
  if (!streamExhausted) {
    const bool deferReconnect = !playRequested && !http.connected() &&
                                static_cast<size_t>(mp3.available()) >= START_WATERMARK;
    if (!deferReconnect && ensureStreamConnected()) {
      pumpStreamToDecoder();
    }
  }

  if (playRequested) {
    const size_t buffered = static_cast<size_t>(mp3.available());

    if (mp3.paused()) {
      if (buffered >= START_WATERMARK || (streamExhausted && buffered > 0)) {
        mp3.unpause();
      }
    } else if (!streamExhausted && buffered < LOW_WATERMARK) {
      Serial.println("MP3 buffer low, pausing playback.");
      mp3.pause();
    }

    if (streamExhausted && buffered < LOW_WATERMARK) {
      Serial.println("Track finished; pre-buffering the next one.");
      startTrack(false);
    }

    logStreamStats();
  }

  delay(playRequested ? 1 : 10);
}
