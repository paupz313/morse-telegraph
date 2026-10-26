/*
 * MORSE TELEGRAPH -- Stage 1.5 firmware for M5Stack ATOM Echo
 *
 * WHAT THIS DOES
 *   Two directions now.
 *   IN   Polls your Telegram bot and plays every incoming message as Morse
 *        beeps through the Atom's speaker. Same 600 Hz and same timing as
 *        the Mac prototype.
 *   OUT  The button is a telegraph key. Tap out Morse, and the device decodes
 *        it and sends it to Telegram as ordinary text -- your friend reads
 *        "HELLO", not dots and dashes.
 *
 * FIRST-RUN SETUP (no recompiling needed to change Wi-Fi or token)
 *   1. Power it up. The LED pulses BLUE and it creates a Wi-Fi network
 *      called "MorseTelegraph-XXXX".
 *   2. Join that network from your phone. A setup page opens by itself.
 *   3. Pick your home Wi-Fi (2.4 GHz only -- the ESP32 cannot see 5 GHz),
 *      type its password, and paste your Telegram bot token.
 *   4. Save. It reboots, connects, and plays "HI" to confirm.
 *
 * USING THE KEY (the whole top face of the Atom is the button)
 *   quick tap            -> dot        )  you hear a live sidetone while
 *   longer press         -> dash       )  you hold it, like a real key
 *   pause ~0.6s          -> ends the letter
 *   pause ~1.4s          -> adds a space
 *   pause ~3s            -> SENDS what you keyed, then two green blinks
 *   hold 3s              -> throw away what you have keyed so far (red blink)
 *   press while it beeps -> stop the incoming message playing
 *
 *   It replies to whoever last messaged the bot. If nobody has ever messaged
 *   it, there is no one to reply to and the LED blinks red -- send it one
 *   Telegram message first to introduce yourself.
 *
 * REOPENING SETUP
 *   Unplug it, hold the button down, plug it back in. Keep holding until the
 *   LED goes red. Wi-Fi and token are forgotten and the setup page returns.
 *
 * THE LED
 *   blue pulse   connecting, or waiting for setup
 *   green        connected, idle, listening
 *   white        beeping, or keying
 *   amber        you have unsent keyed text waiting
 *   red          error -- check the serial monitor
 *
 * NOTE ON SHARING: this build talks to Telegram directly, so the token lives
 * on the device and this device is yours alone. One bot cannot feed several
 * devices -- Telegram hands each message to whoever polls first. Sharing with
 * friends needs the Stage 2 bridge server.
 */

#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <WiFiManager.h>
#include <ArduinoJson.h>
#include <Adafruit_NeoPixel.h>
#include <Preferences.h>
#include <driver/i2s.h>
#include <math.h>

// ---------- ATOM Echo hardware pins (do not change) ----------
static const int I2S_BCLK_PIN = 19;
static const int I2S_LRCK_PIN = 33;
static const int I2S_DOUT_PIN = 22;
static const int LED_PIN      = 27;
static const int BUTTON_PIN   = 39;   // active LOW, external pull-up

// ---------- Morse playback: identical to the Python prototype ----------
static const int   SAMPLE_RATE   = 44100;
static const float BEEP_FREQ_HZ  = 600.0f;   // pitch of the beep
static const float AMPLITUDE     = 0.4f;     // matches the 0.4 in the Python

static const float DOT_SEC        = 0.15f;   // adjust to change overall speed
static const float DASH_SEC       = DOT_SEC * 3;
static const float SYMBOL_GAP_SEC = DOT_SEC;
static const float LETTER_GAP_SEC = DOT_SEC * 3;
static const float WORD_GAP_SEC   = DOT_SEC * 7;

static const float FADE_SEC = 0.01f;         // 10 ms edge ramp, kills clicks

// Speaker loudness, 0.0 to 1.0. A hardware volume knob, not part of the Morse
// logic -- the little Atom speaker is harsh at full scale.
static float speakerVolume = 0.7f;

// ---------- Keying: how YOUR thumb is read ----------
// Deliberately separate from the playback timing above and much more forgiving,
// because no human thumb keys to 150 ms precision. Loosen these if you find it
// turning your dots into dashes.
static const unsigned long KEY_DEBOUNCE_MS   = 25;    // shorter = contact noise
static const unsigned long KEY_DOT_MAX_MS    = 250;   // press under this = dot
static const unsigned long KEY_LETTER_GAP_MS = 600;   // pause that ends a letter
static const unsigned long KEY_WORD_GAP_MS   = 1400;  // pause that adds a space
static const unsigned long KEY_SEND_GAP_MS   = 3000;  // pause that transmits
static const unsigned long KEY_CLEAR_HOLD_MS = 3000;  // hold that wipes the draft
static const unsigned int  KEY_MAX_CHARS     = 120;   // draft length ceiling

// ---------- Telegram ----------
static const char *TELEGRAM_HOST = "https://api.telegram.org/bot";
static const int   POLL_TIMEOUT_SEC = 20;    // long-poll window

// ---------- Cross-core plumbing ----------
// Core 0 runs the network (a 20 s long-poll blocks, and must never block the
// key). Core 1 runs the button and all audio. They only talk through these.
struct MsgBuf { char text[200]; };
QueueHandle_t rxQueue;    // network task  -> audio loop: play this
QueueHandle_t txQueue;    // audio loop    -> network task: send this

// ---------- State ----------
Adafruit_NeoPixel led(1, LED_PIN, NEO_GRB + NEO_KHZ800);
Preferences prefs;

String botToken    = "";
String botName     = "";
int64_t lastChatId = 0;          // who we reply to
bool   shouldSaveConfig = false;
bool   abortPlayback    = false;

static const char *MORSE_TABLE[36] = {
  ".-",    "-...",  "-.-.",  "-..",   ".",     "..-.",  // A B C D E F
  "--.",   "....",  "..",    ".---",  "-.-",   ".-..",  // G H I J K L
  "--",    "-.",    "---",   ".--.",  "--.-",  ".-.",   // M N O P Q R
  "...",   "-",     "..-",   "...-",  ".--",   "-..-",  // S T U V W X
  "-.--",  "--..",                                      // Y Z
  "-----", ".----", "..---", "...--", "....-",          // 0 1 2 3 4
  ".....", "-....", "--...", "---..", "----."           // 5 6 7 8 9
};

static const char *morseFor(char c) {
  if (c >= 'A' && c <= 'Z') return MORSE_TABLE[c - 'A'];
  if (c >= '0' && c <= '9') return MORSE_TABLE[26 + (c - '0')];
  return nullptr;                       // unsupported char: skipped in silence
}

static char charFor(const String &code) {
  for (int i = 0; i < 36; i++)
    if (code.equals(MORSE_TABLE[i]))
      return (i < 26) ? ('A' + i) : ('0' + i - 26);
  return 0;                             // not a real Morse letter
}

// ---------------------------------------------------------------- LED ----

void ledSet(uint8_t r, uint8_t g, uint8_t b) {
  led.setPixelColor(0, led.Color(r, g, b));
  led.show();
}

void ledPulse(uint8_t r, uint8_t g, uint8_t b) {
  float phase = (millis() % 2000) / 2000.0f;
  float k = (sinf(phase * 2 * PI) + 1.0f) / 2.0f;
  k = 0.08f + k * 0.92f;
  ledSet(r * k, g * k, b * k);
}

void ledBlink(uint8_t r, uint8_t g, uint8_t b, int times) {
  for (int i = 0; i < times; i++) {
    ledSet(r, g, b); delay(120);
    ledSet(0, 0, 0);  delay(120);
  }
}

// The one place the resting colour is decided: white while the key is down,
// amber while a keyed draft is waiting, green when idle. Rate limited -- each
// show() briefly disables interrupts, and doing that every millisecond steals
// time from the Wi-Fi stack on the other core.
void ledRefresh(bool keying, bool draftPending) {
  static unsigned long last = 0;
  if (millis() - last < 50) return;
  last = millis();
  if      (keying)       ledSet(60, 60, 60);
  else if (draftPending) ledSet(70, 35, 0);
  else                   ledSet(0, 40, 0);
}

// -------------------------------------------------------------- audio ----

void audioBegin() {
  i2s_config_t cfg = {};
  cfg.mode                 = (i2s_mode_t)(I2S_MODE_MASTER | I2S_MODE_TX);
  cfg.sample_rate          = SAMPLE_RATE;
  cfg.bits_per_sample      = I2S_BITS_PER_SAMPLE_16BIT;
  cfg.channel_format       = I2S_CHANNEL_FMT_ONLY_RIGHT;
  cfg.communication_format = I2S_COMM_FORMAT_STAND_I2S;
  cfg.intr_alloc_flags     = ESP_INTR_FLAG_LEVEL1;
  cfg.dma_buf_count        = 8;
  cfg.dma_buf_len          = 256;
  cfg.use_apll             = false;
  cfg.tx_desc_auto_clear   = true;

  i2s_pin_config_t pins = {};
  pins.bck_io_num   = I2S_BCLK_PIN;
  pins.ws_io_num    = I2S_LRCK_PIN;
  pins.data_out_num = I2S_DOUT_PIN;
  pins.data_in_num  = I2S_PIN_NO_CHANGE;

  i2s_driver_install(I2S_NUM_0, &cfg, 0, NULL);
  i2s_set_pin(I2S_NUM_0, &pins);
  i2s_zero_dma_buffer(I2S_NUM_0);
}

// --- playback: one stretch of audio, a tone if toneOn, otherwise silence.
// Writing real silence rather than delay() keeps the DMA fed and timing exact.
void playSegment(float durationSec, bool toneOn) {
  const int total = (int)(SAMPLE_RATE * durationSec);
  if (total <= 0) return;

  int fadeLen = (int)(SAMPLE_RATE * FADE_SEC);
  if (fadeLen > total / 2) fadeLen = total / 2;

  static int16_t buf[512];
  int i = 0;
  while (i < total) {
    if (digitalRead(BUTTON_PIN) == LOW) { abortPlayback = true; break; }

    int n = (total - i < 512) ? (total - i) : 512;
    for (int k = 0; k < n; k++) {
      float s = 0.0f;
      if (toneOn) {
        int idx = i + k;
        s = AMPLITUDE * sinf(2.0f * PI * BEEP_FREQ_HZ * idx / SAMPLE_RATE);
        if (fadeLen > 1) {
          if (idx < fadeLen)
            s *= (float)idx / (fadeLen - 1);
          else if (idx >= total - fadeLen)
            s *= (float)(total - 1 - idx) / (fadeLen - 1);
        }
      }
      buf[k] = (int16_t)(s * speakerVolume * 32767.0f);
    }
    size_t written = 0;
    i2s_write(I2S_NUM_0, buf, n * sizeof(int16_t), &written, portMAX_DELAY);
    i += n;
  }
}

void beep(float durationSec) {
  playSegment(durationSec, true);
  playSegment(SYMBOL_GAP_SEC, false);
}

// Mirrors play_morse_message() from the Python prototype, symbol for symbol.
void playMorseMessage(const String &text) {
  Serial.printf("Playing message: %s\n", text.c_str());
  abortPlayback = false;
  ledSet(60, 60, 60);

  String up = text;
  up.toUpperCase();

  for (unsigned int n = 0; n < up.length() && !abortPlayback; n++) {
    char c = up[n];

    if (c == ' ') {
      playSegment(WORD_GAP_SEC, false);
      continue;
    }

    const char *code = morseFor(c);
    if (code == nullptr) continue;      // skip punctuation, accents, emoji

    for (int s = 0; code[s] != '\0' && !abortPlayback; s++) {
      if      (code[s] == '.') beep(DOT_SEC);
      else if (code[s] == '-') beep(DASH_SEC);
    }

    playSegment(LETTER_GAP_SEC - SYMBOL_GAP_SEC, false);
    yield();
  }

  i2s_zero_dma_buffer(I2S_NUM_0);
  if (abortPlayback) Serial.println("Playback stopped by button.");
}

// --- sidetone: the live tone you hear while holding the key down.
// Phase carries across calls so the note is continuous, and a 5 ms envelope
// ramp on each edge stops it clicking.
static float sidePhase = 0.0f;
static float sideEnv   = 0.0f;

void sidetoneChunk(bool on) {
  static int16_t buf[128];
  const float step    = 2.0f * PI * BEEP_FREQ_HZ / SAMPLE_RATE;
  const float envStep = 1.0f / (SAMPLE_RATE * 0.005f);
  const float target  = on ? 1.0f : 0.0f;

  for (int i = 0; i < 128; i++) {
    if      (sideEnv < target) sideEnv = fminf(target, sideEnv + envStep);
    else if (sideEnv > target) sideEnv = fmaxf(target, sideEnv - envStep);

    float s = AMPLITUDE * sideEnv * sinf(sidePhase);
    sidePhase += step;
    if (sidePhase > 2 * PI) sidePhase -= 2 * PI;
    buf[i] = (int16_t)(s * speakerVolume * 32767.0f);
  }
  size_t written = 0;
  i2s_write(I2S_NUM_0, buf, sizeof(buf), &written, portMAX_DELAY);
}

// ------------------------------------------------------------- config ----

void onSaveConfig() { shouldSaveConfig = true; }

void factoryReset() {
  Serial.println("Wiping saved Wi-Fi and token...");
  ledSet(80, 0, 0);
  prefs.begin("morse", false);
  prefs.clear();
  prefs.end();
  WiFiManager wm;
  wm.resetSettings();
  delay(500);
  ESP.restart();
}

void startPortalIfNeeded() {
  prefs.begin("morse", true);
  botToken   = prefs.getString("token", "");
  lastChatId = prefs.getLong64("chat", 0);
  prefs.end();

  WiFiManager wm;
  WiFiManagerParameter tokenParam(
      "token", "Telegram bot token (from BotFather)", botToken.c_str(), 80);
  wm.addParameter(&tokenParam);
  wm.setSaveConfigCallback(onSaveConfig);
  wm.setConfigPortalTimeout(300);       // 5 min, then reboot and retry
  wm.setAPCallback([](WiFiManager *m) {
    Serial.println("Setup portal open. Join Wi-Fi: " + m->getConfigPortalSSID());
  });

  char apName[32];
  snprintf(apName, sizeof(apName), "MorseTelegraph-%04X",
           (uint16_t)(ESP.getEfuseMac() & 0xFFFF));

  if (!wm.autoConnect(apName)) {
    Serial.println("Setup timed out, restarting.");
    ESP.restart();
  }

  if (shouldSaveConfig) {
    botToken = String(tokenParam.getValue());
    botToken.trim();
    prefs.begin("morse", false);
    prefs.putString("token", botToken);
    prefs.end();
    Serial.println("Token saved.");
  }
}

void rememberChat(int64_t id) {
  if (id == lastChatId || id == 0) return;
  lastChatId = id;
  prefs.begin("morse", false);
  prefs.putLong64("chat", id);
  prefs.end();
}

// ----------------------------------------------------------- telegram ----

String urlEncode(const String &s) {
  String out;
  for (unsigned int i = 0; i < s.length(); i++) {
    char c = s[i];
    if (isalnum((unsigned char)c)) {
      out += c;
    } else {
      char hex[4];
      snprintf(hex, sizeof(hex), "%%%02X", (uint8_t)c);
      out += hex;
    }
  }
  return out;
}

bool telegramGet(const String &path, JsonDocument &doc, JsonDocument *filter) {
  WiFiClientSecure client;
  client.setInsecure();               // Telegram's cert chain, unverified
  HTTPClient https;
  https.setTimeout((POLL_TIMEOUT_SEC + 5) * 1000);
  https.setConnectTimeout(10000);

  String url = String(TELEGRAM_HOST) + botToken + path;
  if (!https.begin(client, url)) return false;

  int code = https.GET();
  if (code != HTTP_CODE_OK) {
    Serial.printf("Telegram HTTP %d\n", code);
    https.end();
    return false;
  }

  DeserializationError err = filter
      ? deserializeJson(doc, https.getStream(),
                        DeserializationOption::Filter(*filter))
      : deserializeJson(doc, https.getStream());
  https.end();

  if (err) {
    Serial.printf("JSON error: %s\n", err.c_str());
    return false;
  }
  return true;
}

bool verifyToken() {
  JsonDocument doc;
  if (!telegramGet("/getMe", doc, nullptr)) return false;
  if (!doc["ok"].as<bool>()) return false;
  botName = doc["result"]["username"].as<String>();
  Serial.println("Connected to bot: @" + botName);
  return true;
}

bool telegramSend(const String &text) {
  if (lastChatId == 0) {
    Serial.println("Nowhere to send: nobody has messaged this bot yet.");
    return false;
  }
  String path = "/sendMessage?chat_id=" + String((long long)lastChatId) +
                "&text=" + urlEncode(text);
  JsonDocument doc;
  if (!telegramGet(path, doc, nullptr)) return false;
  bool ok = doc["ok"].as<bool>();
  Serial.printf(ok ? "Sent: %s\n" : "Send failed: %s\n", text.c_str());
  return ok;
}

void pollTelegram() {
  static long lastUpdateId = 0;

  JsonDocument filter;
  filter["result"][0]["update_id"] = true;
  filter["result"][0]["message"]["text"] = true;
  filter["result"][0]["message"]["chat"]["id"] = true;
  filter["result"][0]["message"]["from"]["first_name"] = true;

  String path = "/getUpdates?limit=1&timeout=" + String(POLL_TIMEOUT_SEC);
  if (lastUpdateId > 0) path += "&offset=" + String(lastUpdateId);

  JsonDocument doc;
  if (!telegramGet(path, doc, &filter)) {
    vTaskDelay(pdMS_TO_TICKS(3000));
    return;
  }

  for (JsonObject update : doc["result"].as<JsonArray>()) {
    lastUpdateId = update["update_id"].as<long>() + 1;
    const char *text = update["message"]["text"];
    const char *from = update["message"]["from"]["first_name"];
    rememberChat(update["message"]["chat"]["id"].as<int64_t>());

    if (text) {
      Serial.printf("New message from %s: %s\n", from ? from : "unknown", text);
      MsgBuf m;
      strlcpy(m.text, text, sizeof(m.text));   // long messages are trimmed
      xQueueSend(rxQueue, &m, 0);
    }
  }
}

// Core 0. Owns the network and nothing else, so its 20 s long-poll can block
// freely without ever stalling the key on core 1.
void netTask(void *arg) {
  for (;;) {
    if (WiFi.status() != WL_CONNECTED) {
      Serial.println("Wi-Fi dropped, reconnecting...");
      WiFi.reconnect();
      vTaskDelay(pdMS_TO_TICKS(5000));
      continue;
    }
    MsgBuf out;
    while (xQueueReceive(txQueue, &out, 0) == pdTRUE) telegramSend(String(out.text));
    pollTelegram();
    vTaskDelay(pdMS_TO_TICKS(50));
  }
}

// ------------------------------------------------------------ the key ----

static bool          keyDown       = false;
static unsigned long pressStart    = 0;
static unsigned long lastRelease   = 0;
static String        pendingSymbol = "";   // dots/dashes of the letter in progress
static String        draft         = "";   // decoded text not yet sent

void clearDraft(const char *why) {
  Serial.printf("Draft cleared (%s)\n", why);
  pendingSymbol = "";
  draft = "";
  ledBlink(80, 0, 0, 1);
}

void sendDraft() {
  draft.trim();
  if (draft.length() == 0) { pendingSymbol = ""; return; }

  Serial.println("Keyed: " + draft);
  MsgBuf m;
  strlcpy(m.text, draft.c_str(), sizeof(m.text));
  if (xQueueSend(txQueue, &m, 0) == pdTRUE) ledBlink(0, 80, 0, 2);
  else                                      ledBlink(80, 0, 0, 2);
  draft = "";
  pendingSymbol = "";
}

void handleKey() {
  bool down = (digitalRead(BUTTON_PIN) == LOW);

  // --- held down: sound the sidetone and time the press
  if (down) {
    if (!keyDown) { keyDown = true; pressStart = millis(); }
    sidetoneChunk(true);

    if (millis() - pressStart > KEY_CLEAR_HOLD_MS) {
      while (digitalRead(BUTTON_PIN) == LOW) { sidetoneChunk(false); }
      keyDown = false;
      lastRelease = millis();
      clearDraft("held");
    }
    return;
  }

  // --- just released: that press was a dot or a dash
  if (keyDown) {
    keyDown = false;
    unsigned long held = millis() - pressStart;
    lastRelease = millis();
    if (held >= KEY_DEBOUNCE_MS) {
      pendingSymbol += (held < KEY_DOT_MAX_MS) ? '.' : '-';
      Serial.printf("  %s (%lums)\n", pendingSymbol.c_str(), held);
    }
    return;
  }

  // --- up: let the sidetone finish its release ramp
  if (sideEnv > 0.0001f) { sidetoneChunk(false); return; }

  // --- up and quiet: measure the pause and act on it
  unsigned long idle = millis() - lastRelease;

  if (pendingSymbol.length() > 0 && idle > KEY_LETTER_GAP_MS) {
    char c = charFor(pendingSymbol);
    if (c && draft.length() < KEY_MAX_CHARS) {
      draft += c;
      Serial.println("Draft: " + draft);
    } else if (!c) {
      Serial.println("Not a letter, dropped: " + pendingSymbol);
    }
    pendingSymbol = "";
    return;
  }

  if (pendingSymbol.length() == 0 && draft.length() > 0) {
    if (idle > KEY_SEND_GAP_MS) {
      sendDraft();
    } else if (idle > KEY_WORD_GAP_MS && !draft.endsWith(" ")) {
      draft += ' ';
    }
  }
}

// --------------------------------------------------------------- main ----

void setup() {
  Serial.begin(115200);
  delay(300);
  Serial.println("\nMorse Telegraph -- ATOM Echo");

  pinMode(BUTTON_PIN, INPUT);
  led.begin();
  led.setBrightness(60);
  ledSet(0, 0, 60);

  audioBegin();

  // Hold the button while powering up to force the setup portal open.
  if (digitalRead(BUTTON_PIN) == LOW) {
    delay(50);
    if (digitalRead(BUTTON_PIN) == LOW) factoryReset();
  }

  startPortalIfNeeded();

  Serial.println("Wi-Fi connected: " + WiFi.SSID());
  Serial.println("IP address: " + WiFi.localIP().toString());

  if (botToken.length() < 20) {
    Serial.println("No bot token set. Reboot holding the button to reopen setup.");
    while (true) { ledPulse(80, 0, 0); delay(20); }
  }

  while (!verifyToken()) {
    Serial.println("Token rejected. Reboot holding the button to reopen setup.");
    for (int i = 0; i < 250; i++) { ledPulse(80, 0, 0); delay(20); }
  }

  if (lastChatId == 0)
    Serial.println("No chat known yet -- send the bot a Telegram message so it "
                   "knows who to reply to.");

  playMorseMessage("HI");               // boot confirmation beep

  rxQueue = xQueueCreate(4, sizeof(MsgBuf));
  txQueue = xQueueCreate(4, sizeof(MsgBuf));
  xTaskCreatePinnedToCore(netTask, "net", 12288, NULL, 1, NULL, 0);

  lastRelease = millis();
}

void loop() {
  handleKey();

  MsgBuf in;
  if (xQueueReceive(rxQueue, &in, 0) == pdTRUE) {
    playMorseMessage(String(in.text));
    // A press that stopped playback must not also register as a dot.
    while (digitalRead(BUTTON_PIN) == LOW) delay(10);
    keyDown = false;
    lastRelease = millis();
  }

  ledRefresh(keyDown, draft.length() > 0 || pendingSymbol.length() > 0);
  delay(1);
}
