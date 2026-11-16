/*
  Lully blanket — demo firmware for an ESP32 + WS2812B strip folded into a 16 x 20 zigzag grid.

  The phone app does the listening and the AI. It sends the blanket's colors (16 x 20 RGB = 960 bytes)
  about 10 times a second through a free public MQTT relay; this board receives them and glides
  smoothly between frames at ~60 fps.

  Arduino IDE setup (once):
    1. Boards Manager: install "esp32 by Espressif Systems". Board: "ESP32 Dev Module".
    2. Library Manager: install "FastLED" (by Daniel Garcia), "WiFiManager" (by tzapu), "PubSubClient" (by Nick O'Leary).
    3. Plug in the ESP32 with a USB data cable, pick its port, click Upload.

  First power-up: the board opens a Wi-Fi network called "Lully-setup". Join it from your phone, pick your
  home Wi-Fi, and note the pairing code shown there (you can change it). Type the same code into the
  app's "Blanket" button.
*/
#include <FastLED.h>
#include <WiFi.h>
#include <WiFiManager.h>
#include <PubSubClient.h>
#include <Preferences.h>

// ---------- your layout ----------
#define DATA_PIN     13        // strip data wire -> GPIO 13 (through a 330 ohm resistor if you have one)
#define COLS         16        // LEDs per folded row
#define ROWS         20        // number of rows (2 strips x 10 rows)
#define SERPENTINE   true      // rows alternate direction (zigzag fold)
#define MAX_MILLIAMPS 2000     // power limit for the whole strip (power bank USB: keep 2000 or less)
#define MAX_BRIGHT   140       // 0-255 overall ceiling; it's under fabric, this is plenty
// ----------------------------------

#define NUM_LEDS (COLS * ROWS)
CRGB leds[NUM_LEDS];
float cur[NUM_LEDS * 3], tgt[NUM_LEDS * 3];

const char* BROKER = "broker.emqx.io";   // free public MQTT relay (the app connects to the same one)
const int   PORT   = 1883;

WiFiClient net;
PubSubClient mqtt(net);
Preferences prefs;
char code[33] = "";
char topic[64];
uint32_t lastFrameMs = 0, testUntil = 0;
uint8_t bright = MAX_BRIGHT;
uint8_t gammaT[256];               // LEDs look too bright at low levels without this

int ledIndex(int x, int y) {               // grid (x,y) -> position along the strip
  if (SERPENTINE && (y & 1)) x = COLS - 1 - x;
  return y * COLS + x;
}

void onMessage(char* t, byte* p, unsigned int len) {
  if (len == NUM_LEDS * 3) {               // a full frame: 16x20 RGB, row by row
    for (int y = 0; y < ROWS; y++) for (int x = 0; x < COLS; x++) {
      int src = (y * COLS + x) * 3, dst = ledIndex(x, y) * 3;
      tgt[dst] = p[src]; tgt[dst + 1] = p[src + 1]; tgt[dst + 2] = p[src + 2];
    }
    lastFrameMs = millis();
  } else if (len == 1) {                   // brightness 0-255 (capped)
    bright = min<int>(p[0], MAX_BRIGHT); FastLED.setBrightness(bright);
  } else if (len == 4 && memcmp(p, "test", 4) == 0) {
    testUntil = millis() + 12000;          // walk through the rows so you can check the layout
  }
}

void connectMqtt() {
  while (!mqtt.connected()) {
    String id = "lully-" + String((uint32_t)ESP.getEfuseMac(), HEX);
    if (mqtt.connect(id.c_str())) { mqtt.subscribe(topic); Serial.printf("Relay connected, listening on %s\n", topic); }
    else { Serial.println("Relay not reachable, retrying..."); delay(2000); }
  }
}

void setup() {
  Serial.begin(115200);
  for (int i = 0; i < 256; i++) gammaT[i] = (uint8_t)(pow(i / 255.0, 2.0) * 255 + 0.5);
  FastLED.addLeds<WS2812B, DATA_PIN, GRB>(leds, NUM_LEDS);
  FastLED.setMaxPowerInVoltsAndMilliamps(5, MAX_MILLIAMPS);
  FastLED.setBrightness(bright);
  fill_solid(leds, NUM_LEDS, CRGB(40, 18, 6)); FastLED.show();   // warm "I'm awake" glow

  prefs.begin("lully", false);
  String saved = prefs.getString("code", "");
  if (saved.length() == 0) { uint32_t m = (uint32_t)ESP.getEfuseMac(); saved = "lully-" + String(m % 100000); }
  saved.toCharArray(code, sizeof(code));

  WiFiManager wm;
  WiFiManagerParameter pcode("code", "Pairing code (type this into the app)", code, 32);
  wm.addParameter(&pcode);
  wm.setConfigPortalTimeout(300);
  if (!wm.autoConnect("Lully-setup")) { ESP.restart(); }
  strncpy(code, pcode.getValue(), sizeof(code) - 1);
  prefs.putString("code", code);
  snprintf(topic, sizeof(topic), "lully/%s/frame", code);
  Serial.printf("Wi-Fi connected. Pairing code: %s\n", code);

  mqtt.setServer(BROKER, PORT);
  mqtt.setBufferSize(NUM_LEDS * 3 + 128);
  mqtt.setCallback(onMessage);
  mqtt.setKeepAlive(30);
  for (int i = 0; i < NUM_LEDS * 3; i++) { cur[i] = 0; tgt[i] = 0; }
}

void loop() {
  if (!mqtt.connected()) connectMqtt();
  mqtt.loop();

  static uint32_t lastShow = 0;
  uint32_t now = millis();
  if (now - lastShow < 16) return;        // ~60 fps
  lastShow = now;

  if (now < testUntil) {                  // layout test: one row at a time, first LED of each row red
    int row = ((testUntil - now) / 600) % ROWS; row = ROWS - 1 - row;
    fill_solid(leds, NUM_LEDS, CRGB::Black);
    for (int x = 0; x < COLS; x++) leds[ledIndex(x, row)] = CRGB(60, 60, 60);
    leds[ledIndex(0, row)] = CRGB(150, 0, 0);
    FastLED.show(); return;
  }

  // no frames for 20 s (phone closed): fade down to a faint warm glow
  bool stale = now - lastFrameMs > 20000;
  float k = stale ? 0.01f : 0.12f;        // glide toward the newest frame
  for (int i = 0; i < NUM_LEDS * 3; i++) {
    float target = stale ? ((i % 3 == 0) ? 18 : (i % 3 == 1) ? 8 : 3) : tgt[i];
    cur[i] += (target - cur[i]) * k;
  }
  for (int i = 0; i < NUM_LEDS; i++) leds[i] = CRGB(gammaT[(uint8_t)cur[i * 3]], gammaT[(uint8_t)cur[i * 3 + 1]], gammaT[(uint8_t)cur[i * 3 + 2]]);
  FastLED.show();
}
