/*
  Lully blanket — demo firmware for an ESP32 + WS2812B strip folded into a 16 x 20 zigzag grid.

  The phone app does the listening and the AI. It sends the blanket's colors (16 x 20 RGB = 960 bytes)
  about 10 times a second through a free public MQTT relay; this board receives them and glides
  smoothly between frames at ~60 fps.

  Arduino IDE setup (once):
    1. Boards Manager: install "esp32 by Espressif Systems". Board: "ESP32 Dev Module".
    2. Library Manager: install "FastLED" (by Daniel Garcia), "WiFiManager" (by tzapu), "PubSubClient" (by Nick O'Leary).
    3. Plug in the ESP32 with a USB data cable, pick its port, click Upload.

  First power-up, or when the saved Wi-Fi isn't found (or after holding BOOT for 3 s while it runs): the board opens a Wi-Fi network called "Lully-setup" (password: lullylight). Join it from your phone, pick your
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
#define STRIPS       1         // how many 160-LED strips are chained (160 LEDs each)
#define ROWS         10        // rows on your board (one strip: 10, two strips: 20)
#define COLS         14        // LEDs you can SEE in each row
#define TURN_LEDS    2         // LEDs hidden inside each Z-fold corner (kept off). Cut-and-soldered rows: 0
// check: ROWS*COLS + (ROWS-1)*TURN_LEDS must be <= STRIPS*160   (10*14 + 9*2 = 158  OK)
#define SERPENTINE   true      // rows alternate direction (zigzag fold)
#define MAX_MILLIAMPS (STRIPS * 1000)  // power limit for the whole setup (one strip: 1 A)
#define MAX_BRIGHT   140       // 0-255 overall ceiling; it's under fabric, this is plenty
// ----------------------------------

#define NUM_LEDS (STRIPS * 160)
#define GRID (COLS * ROWS)
static_assert(ROWS * COLS + (ROWS - 1) * TURN_LEDS <= NUM_LEDS, "Layout uses more LEDs than the strip has");
#define FRAME_COLS 16          // the app always sends a 16 x 20 picture
#define FRAME_ROWS 20
#define FRAME_BYTES (FRAME_COLS * FRAME_ROWS * 3)
CRGB leds[NUM_LEDS];
float cur[GRID * 3], tgt[GRID * 3];      // one entry per visible LED, in grid order

const char* BROKER = "broker.emqx.io";   // free public MQTT relay (the app connects to the same one)
const int   PORT   = 1883;

WiFiClient net;
PubSubClient mqtt(net);
Preferences prefs;
char code[33] = "";
char topic[64];
uint32_t lastFrameMs = 0, testUntil = 0;
uint8_t bright = MAX_BRIGHT;
uint8_t spacing = 1;                 // light only every Nth LED along the strip, to preview wider LED spacing
uint8_t gammaT[256];               // LEDs look too bright at low levels without this

int ledIndex(int x, int y) {               // grid (x,y) -> position along the strip, skipping corner LEDs
  if (SERPENTINE && (y & 1)) x = COLS - 1 - x;
  return y * (COLS + TURN_LEDS) + x;
}

void onMessage(char* t, byte* p, unsigned int len) {
  if (len == FRAME_BYTES) {                // a 16x20 RGB picture, row by row; squeezed to fit however many rows we have
    for (int y = 0; y < ROWS; y++) {
      int r0 = y * FRAME_ROWS / ROWS, r1 = (y + 1) * FRAME_ROWS / ROWS; if (r1 <= r0) r1 = r0 + 1;
      for (int x = 0; x < COLS; x++) {
        int c0 = x * FRAME_COLS / COLS, c1 = (x + 1) * FRAME_COLS / COLS; if (c1 <= c0) c1 = c0 + 1;
        float a = 0, b = 0, c = 0; int n = 0;
        for (int r = r0; r < r1; r++) for (int q = c0; q < c1; q++) { int src = (r * FRAME_COLS + q) * 3; a += p[src]; b += p[src + 1]; c += p[src + 2]; n++; }
        int dst = (y * COLS + x) * 3;
        tgt[dst] = a / n; tgt[dst + 1] = b / n; tgt[dst + 2] = c / n;
      }
    }
    lastFrameMs = millis();
  } else if (len == 1) {                   // brightness 0-255 (capped)
    bright = min<int>(p[0], MAX_BRIGHT); FastLED.setBrightness(bright);
  } else if (len == 3 && p[0] == 's' && p[1] == 'p') {   // spacing preview: "sp" + N
    spacing = max<int>(1, p[2]); prefs.putUChar("spacing", spacing);
    Serial.printf("Spacing preview: every %d LED(s)\n", spacing);
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
  spacing = prefs.getUChar("spacing", 1);
  String saved = prefs.getString("code", "");
  if (saved.length() == 0) {               // random, hard-to-guess default pairing code
    const char* al = "abcdefghjkmnpqrstuvwxyz23456789"; saved = "lully-";
    for (int i = 0; i < 8; i++) saved += al[esp_random() % 31];
  }
  saved.toCharArray(code, sizeof(code));

  WiFiManager wm;
  pinMode(0, INPUT_PULLUP);                // BOOT button: hold 3 s any time while running to forget Wi-Fi (see loop)
  wm.setConnectTimeout(20);                // if the saved Wi-Fi isn't around, open Lully-setup after 20 s
  WiFiManagerParameter pcode("code", "Pairing code (type this into the app)", code, 32);
  wm.addParameter(&pcode);
  wm.setConfigPortalTimeout(300);
  if (!wm.autoConnect("Lully-setup", "lullylight")) { ESP.restart(); }   // setup network password: lullylight
  strncpy(code, pcode.getValue(), sizeof(code) - 1);
  prefs.putString("code", code);
  snprintf(topic, sizeof(topic), "lully/%s/frame", code);
  Serial.printf("Wi-Fi connected. Pairing code: %s\n", code);

  mqtt.setServer(BROKER, PORT);
  mqtt.setBufferSize(FRAME_BYTES + 128);
  mqtt.setCallback(onMessage);
  mqtt.setKeepAlive(30);
  for (int i = 0; i < GRID * 3; i++) { cur[i] = 0; tgt[i] = 0; }
}

void loop() {
  // hold BOOT for 3 seconds (while running, NOT while tapping EN) to forget Wi-Fi and reopen Lully-setup
  static uint32_t bootDown = 0;
  if (digitalRead(0) == LOW) {
    if (!bootDown) bootDown = millis();
    if (millis() - bootDown > 3000) {
      Serial.println("BOOT held 3 s: forgetting Wi-Fi");
      fill_solid(leds, NUM_LEDS, CRGB(0, 0, 60)); FastLED.show();
      WiFiManager wm; wm.resetSettings(); delay(1500); ESP.restart();
    }
  } else bootDown = 0;
  static uint32_t lastInfo = 0;
  if (millis() - lastInfo > 10000) { lastInfo = millis(); Serial.printf("Pairing code: %s  (relay %s)\n", code, mqtt.connected() ? "connected" : "not connected"); }
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
  for (int i = 0; i < GRID * 3; i++) {
    float target = stale ? ((i % 3 == 0) ? 18 : (i % 3 == 1) ? 8 : 3) : tgt[i];
    cur[i] += (target - cur[i]) * k;
  }
  fill_solid(leds, NUM_LEDS, CRGB::Black);   // corner and spare LEDs stay off
  for (int y = 0; y < ROWS; y++) for (int x = 0; x < COLS; x++) {
    int g = (y * COLS + x) * 3;
    int li = ledIndex(x, y);
    if (spacing > 1 && (li % spacing) != 0) continue;   // spacing preview: the LEDs in between stay off
    leds[li] = CRGB(gammaT[(uint8_t)cur[g]], gammaT[(uint8_t)cur[g + 1]], gammaT[(uint8_t)cur[g + 2]]);
  }
  FastLED.show();
}
