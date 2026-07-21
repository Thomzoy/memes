#include <Arduino.h>
#include <WiFi.h>
#include <HTTPClient.h>
#include <BackgroundAudio.h>
#include <ESP32I2SAudio.h>
#include <OneButton.h>
#include <algorithm>
#include <ResponsiveAnalogRead.h>
#include "../include/secrets.h"

constexpr uint32_t WIFI_CONNECT_TIMEOUT_MS = 20000;
constexpr uint32_t RECONNECT_BACKOFF_MS = 1000;
constexpr size_t STREAM_BUFFER_SIZE = 32 * 1024;
constexpr size_t CHUNK_BUFFER_SIZE = 1024;
constexpr size_t LOW_WATERMARK = 1024;
constexpr int BUTTON_PIN = 32;
constexpr int POT_PIN = 34;
constexpr uint32_t POT_PRINT_INTERVAL_MS = 200;

// MAX98357 I2S wiring 
constexpr int I2S_BCLK_PIN = 27;
constexpr int I2S_LRCK_PIN = 26;
constexpr int I2S_DOUT_PIN = 25;

const char* STREAM_URL_BASE = "https://github.com/Thomzoy/memes/releases/latest/download/";
const char* STREAM_URL_SUFFIX = ".mp3";
const char* TRACK_COUNT_URL = "https://github.com/Thomzoy/memes/releases/latest/download/number.txt";
char streamUrl[128] = {0};
const char* GITHUB_PAT = ""; // Optional: set token to access private content
int gTrackCount = 60;

const char* WIFI_SSID = APP_WIFI_SSID;
const char* WIFI_PASSWORD = APP_WIFI_PASSWORD;

ESP32I2SAudio i2sAudio(I2S_BCLK_PIN, I2S_LRCK_PIN, I2S_DOUT_PIN);
BackgroundAudioMP3Class<RawDataBuffer<STREAM_BUFFER_SIZE>> mp3(i2sAudio);
NetworkClientSecure tlsClient;
HTTPClient http;
uint8_t readBuffer[CHUNK_BUFFER_SIZE];
uint32_t nextReconnectMs = 0;
bool playbackFinished = false;
bool streamExhausted = false;
bool startPlaybackRequested = false;
int32_t streamContentLength = -1;
size_t streamBytesRead = 0;
OneButton button(BUTTON_PIN, true, true);

bool fetchTrackCount();
void selectRandomStreamUrl();
void disconnectHttpStream();

void onButtonClick() {
  Serial.println("Button pressed");

  if (startPlaybackRequested && !playbackFinished) {
    Serial.println("Switching to another random track...");
  }

  disconnectHttpStream();
  selectRandomStreamUrl();

  streamExhausted = false;
  playbackFinished = false;
  streamContentLength = -1;
  streamBytesRead = 0;
  nextReconnectMs = 0;
  startPlaybackRequested = true;

  if (mp3.paused()) {
    mp3.unpause();
  }
  Serial.print("Ready to play: ");
  Serial.println(streamUrl);
}

bool fetchTrackCount() {
  if (!WiFi.isConnected()) {
    Serial.println("Cannot fetch track count: Wi-Fi not connected.");
    return false;
  }

  NetworkClientSecure countTlsClient;
  HTTPClient countHttp;
  countTlsClient.setInsecure();

  Serial.printf("Fetching track count from '%s'...\n", TRACK_COUNT_URL);
  if (!countHttp.begin(countTlsClient, TRACK_COUNT_URL)) {
    Serial.println("Failed to begin track count request.");
    return false;
  }

  countHttp.setReuse(false);
  countHttp.setConnectTimeout(10000);
  countHttp.setTimeout(10000);
  countHttp.setFollowRedirects(HTTPC_FORCE_FOLLOW_REDIRECTS);
  countHttp.addHeader("User-Agent", "esp32-background-audio");
  if (strlen(GITHUB_PAT) > 0) {
    countHttp.addHeader("Authorization", String("Bearer ") + GITHUB_PAT);
  }

  int code = countHttp.GET();
  if (code != HTTP_CODE_OK) {
    Serial.printf("Track count fetch failed, HTTP code: %d\n", code);
    countHttp.end();
    return false;
  }

  String payload = countHttp.getString();
  countHttp.end();
  payload.trim();

  int parsedCount = payload.toInt();
  if (parsedCount <= 0) {
    Serial.printf("Invalid track count payload: '%s'\n", payload.c_str());
    return false;
  }

  gTrackCount = parsedCount;
  Serial.printf("Track count updated: %d\n", gTrackCount);
  return true;
}

void selectRandomStreamUrl() {
  randomSeed(static_cast<uint32_t>(esp_random()));
  int track = random(1, gTrackCount + 1); // 1..gTrackCount inclusive
  char fileName[8];
  snprintf(fileName, sizeof(fileName), "%04d", track);
  snprintf(streamUrl, sizeof(streamUrl), "%s%s%s", STREAM_URL_BASE, fileName, STREAM_URL_SUFFIX);
  Serial.printf("Selected track: %d/%d (%s)\n", track, gTrackCount, streamUrl);
}

void connectToWifi() {
  Serial.print("Connecting to Wi-Fi: ");
  Serial.println(WIFI_SSID);

  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);

  const uint32_t start = millis();
  while (WiFi.status() != WL_CONNECTED && (millis() - start) < WIFI_CONNECT_TIMEOUT_MS) {
    Serial.print('.');
    delay(500);
  }
  Serial.println();

  if (WiFi.status() == WL_CONNECTED) {
    Serial.print("Wi-Fi connected. IP: ");
    Serial.println(WiFi.localIP());
  } else {
    Serial.println("Wi-Fi connection timeout.");
  }
}

void setup() {
  Serial.begin(115200);
  button.attachClick(onButtonClick);
  pinMode(POT_PIN, INPUT);
  analogReadResolution(12);
  connectToWifi();

  if (WiFi.status() != WL_CONNECTED) {
    Serial.println("Cannot start audio stream without Wi-Fi.");
    return;
  }

  if (!fetchTrackCount()) {
    Serial.printf("Using fallback track count: %d\n", gTrackCount);
  }

  tlsClient.setInsecure();
  mp3.setGain(0.5f);
  mp3.begin();

  Serial.println("Press button to select and start a track.");
}

void disconnectHttpStream() {
  http.end();
  tlsClient.stop();
}

bool ensureHttpConnected() {
  if (playbackFinished || streamExhausted) {
    return false;
  }

  if (http.connected()) {
    return true;
  }

  if (millis() < nextReconnectMs) {
    return false;
  }

  Serial.printf("(Re)connecting to '%s'...\n", streamUrl);
  disconnectHttpStream();

  if (!http.begin(tlsClient, streamUrl)) {
    Serial.println("HTTP begin failed.");
    nextReconnectMs = millis() + RECONNECT_BACKOFF_MS;
    return false;
  }

  http.setReuse(false);
  http.setConnectTimeout(10000);
  http.setTimeout(15000);
  http.setFollowRedirects(HTTPC_FORCE_FOLLOW_REDIRECTS);
  http.addHeader("User-Agent", "esp32-background-audio");
  if (strlen(GITHUB_PAT) > 0) {
    http.addHeader("Authorization", String("Bearer ") + GITHUB_PAT);
  }

  int code = http.GET();
  if (code != HTTP_CODE_OK) {
    Serial.print("HTTP GET failed, code: ");
    Serial.println(code);
    disconnectHttpStream();
    nextReconnectMs = millis() + RECONNECT_BACKOFF_MS;
    return false;
  }

  streamContentLength = http.getSize();
  streamBytesRead = 0;

  Serial.println("HTTP stream connected.");
  return true;
}

void pumpHttpToMp3() {
  WiFiClient* stream = http.getStreamPtr();
  bool gotDataThisPass = false;

  while (stream->connected()) {
    int availableBytes = stream->available();
    if (availableBytes <= 0) {
      break;
    }

    size_t httpAvail = static_cast<size_t>(availableBytes);
    if (httpAvail > CHUNK_BUFFER_SIZE) {
      httpAvail = CHUNK_BUFFER_SIZE;
    }
    size_t mp3Avail = mp3.availableForWrite();
    if (!httpAvail || !mp3Avail) {
      break;
    }

    size_t toRead = (httpAvail < mp3Avail) ? httpAvail : mp3Avail;
    int read = stream->read(readBuffer, toRead);
    if (read <= 0) {
      break;
    }

    mp3.write(readBuffer, read);
    gotDataThisPass = true;
    streamBytesRead += static_cast<size_t>(read);

    if (streamContentLength > 0 && streamBytesRead >= static_cast<size_t>(streamContentLength)) {
      streamExhausted = true;
      Serial.println("HTTP payload fully read; draining MP3 buffer.");
      disconnectHttpStream();
      break;
    }
  }

  // Fallback EOF detection for responses where content-length is unknown.
  if (!streamExhausted && !stream->connected() && !stream->available() && gotDataThisPass) {
    streamExhausted = true;
    Serial.println("HTTP stream closed by server; draining MP3 buffer.");
    disconnectHttpStream();
  }
}

void loop() {
  static uint32_t lastStatsMs = 0;
  static uint32_t lastPotPrintMs = 0;
  static int lastMappedValue = -1;
  static bool wasPaused = false;

  button.tick();

  if (millis() - lastPotPrintMs >= POT_PRINT_INTERVAL_MS) {
    lastPotPrintMs = millis();
    int value = analogRead(POT_PIN);
    // Map raw ADC value to 0..20, then to gain 0.0..2.0.
    int mapped_value = map(value, 0, 4094, 20, 0);
    float float_value = static_cast<float>(mapped_value) / 10.0f;

    if (mapped_value != lastMappedValue) {
      lastMappedValue = mapped_value;
      mp3.setGain(float_value);
      Serial.printf("pot34_raw=%d mapped=%d gain=%.1f\n", value, mapped_value, float_value);
          Serial.printf("buffer=%d frames=%lu underflows=%lu errors=%lu dumps=%lu\n",
                  mp3.available(), mp3.frames(), mp3.underflows(), mp3.errors(), mp3.dumps());
    }
  }

  if (!startPlaybackRequested) {
    delay(20);
    return;
  }

  if (playbackFinished) {
    delay(250);
    return;
  }

  if (streamExhausted) {
    if (mp3.available() < LOW_WATERMARK) {
      Serial.println("Stream ended, stopping playback.");
      disconnectHttpStream();
      mp3.pause();
      playbackFinished = true;
      startPlaybackRequested = false;
    }
    delay(10);
    return;
  }

  if (!WiFi.isConnected()) {
    Serial.println("Wi-Fi lost, reconnecting...");
    disconnectHttpStream();
    nextReconnectMs = millis() + RECONNECT_BACKOFF_MS;
    connectToWifi();
    delay(250);
    return;
  }

  if (!ensureHttpConnected()) {
    delay(100);
    return;
  }

  pumpHttpToMp3();

  if (http.connected() && mp3.available() < LOW_WATERMARK) {
    mp3.pause();
    if (!wasPaused) {
      Serial.println("MP3 buffer low, pausing playback.");
      wasPaused = true;
    }
  } else if (mp3.paused() && mp3.available() > (STREAM_BUFFER_SIZE / 2)) {
    mp3.unpause();
    wasPaused = false;
  }

  if (millis() - lastStatsMs > 1000) {
    lastStatsMs = millis();
    Serial.printf("buffer=%d frames=%lu underflows=%lu errors=%lu dumps=%lu\n",
                  mp3.available(), mp3.frames(), mp3.underflows(), mp3.errors(), mp3.dumps());
  }

  if (!http.connected() && streamContentLength > 0 && streamBytesRead >= static_cast<size_t>(streamContentLength)) {
    streamExhausted = true;
  }
}