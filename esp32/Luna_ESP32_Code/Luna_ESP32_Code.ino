// ====================================================================
//  Luna ESP32-S3 AI Voice Assistant — v10
// ====================================================================

#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <Wire.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SH110X.h>
#include "ESP_I2S.h"
#include "ESP_SR.h"
#include <Preferences.h>
#include <math.h>

// ================= CREDENTIALS =================
// Copy `secrets.h.example` to `secrets.h` and fill in your own values.
// `secrets.h` is listed in .gitignore, so your Wi-Fi credentials and
// server URL never end up in the repository.
#include "secrets.h"

// ================= WIFI =================
const char* ssid     = WIFI_SSID;
const char* password = WIFI_PASSWORD;

// ================= SERVER =================
const char* chatURL     = SERVER_BASE_URL "/api/chat";
const char* sttURL      = SERVER_BASE_URL "/api/stt";
const char* ttsURL      = SERVER_BASE_URL "/api/tts";
// v3 server fast path: WAV in -> WAV out in a single call
const char* converseURL = SERVER_BASE_URL "/api/converse";

// ================= OLED =================
#define OLED_SDA 8
#define OLED_SCL 9
Adafruit_SH1106G display(128, 64, &Wire, -1);

// ================= SPEAKER (MAX98357A) =================
#define SPK_BCLK 15
#define SPK_LRC  16
#define SPK_DOUT 7

// ================= MIC (INMP441) =================
#define MIC_BCLK 4
#define MIC_LRC  5
#define MIC_DIN  6

// ================= BUTTON =================
#define BUTTON_PIN 21    // was 10; GPIO20 is USB D+ -- do not use it

// ================= AUDIO =================
#define SAMPLE_RATE        16000
// 15 s is now 15 s of ACTUAL SPEECH. Leading silence never reaches this
// buffer any more, so the budget is no longer wasted waiting for you.
#define MAX_RECORD_SECONDS 15
#define MAX_SAMPLES        (SAMPLE_RATE * MAX_RECORD_SECONDS)
#define MAX_WAV_SIZE       (44 + MAX_SAMPLES * 2)


#define MIC_UNITY_SCALE   0.2f      // micGain 1.0 == legacy (>>11 x 1.6)
#define FULL_SCALE        32767.0f

// ---- Fixed frame geometry (not user-tunable) -----------------------
#define FRAME_SAMPLES     128                    // 8 ms at 16 kHz
#define MIN_RECORD_MS     400     // button mode: shortest allowed press
#define MIN_UTTERANCE_MS  350     // shorter than this = noise blip
#define PREROLL_MS        300     // audio kept from BEFORE speech onset
#define TRAIL_KEEP_MS     250     // trailing silence left in the clip
#define PREROLL_SAMPLES   (SAMPLE_RATE * PREROLL_MS / 1000)

// ---- Noise-floor tracker shape (rarely needs changing) -------------
// Tracks the MEAN OF NON-SPEECH FRAMES, not the minimum. A frame well
// above the current estimate is probably speech, so it only creeps.
#define NOISE_ADAPT       0.05f     // adaptation on noise-like frames
#define NOISE_CREEP       0.005f    // slow creep on speech-like frames
#define NOISE_EXCLUDE     2.5f      // above this x estimate = probably speech

// ====================================================================
//  AudioConfig -- single source of truth for all audio tuning.
//  Defaults are the QUIET-ROOM starting point.
// ====================================================================
struct AudioConfig {
  float micGain;        // recording path.  1.0 == v9.9.  Range 0.25 - 8.0
  float wakeGain;       // wake-word path.  10.0 == v9.9's >>7
  float noiseMult;      // speech must exceed floor x this to START
  float contMult;       // fraction of start gate needed to STAY in speech
  float gateFloorMin;   // gate rails, expressed in micGain=1.0 units.
  float gateFloorMax;   //   Scaled by micGain automatically (see below).
  int   minSpeechMs;    // voiced run needed to declare speech started
  int   endSilenceMs;   // silence that ends the utterance
  int   wakeWaitMs;     // how long to wait for you to start talking
  int   calibrationMs;  // length of the noise-floor measurement
  float limitKnee;      // soft-limit above this fraction of full scale
  bool  meterOn;        // live RMS/peak meter printing
  bool  normOn;         // clip normalisation (leave off)
};

// micGain 2.5 recovers most of the 20.8 dB of headroom v9.9 left unused,
// without approaching the limiter. See the tuning guide.
AudioConfig cfg = {
  /* micGain       */ 2.5f,
  /* wakeGain      */ 10.0f,
  /* noiseMult     */ 1.9f,
  /* contMult      */ 0.70f,
  /* gateFloorMin  */ 150.0f,
  /* gateFloorMax  */ 700.0f,
  /* minSpeechMs   */ 90,
  /* endSilenceMs  */ 800,
  /* wakeWaitMs    */ 10000,
  /* calibrationMs */ 500,
  /* limitKnee     */ 0.73f,
  /* meterOn       */ false,
  /* normOn        */ false
};


static inline float gateMinNow()  { return cfg.gateFloorMin * cfg.micGain; }
static inline float gateMaxNow()  { return cfg.gateFloorMax * cfg.micGain; }
static inline float noiseMinNow() { return  40.0f * cfg.micGain; }
static inline float noiseMaxNow() { return 600.0f * cfg.micGain; }
static inline float limitKneeNow(){ return cfg.limitKnee * FULL_SCALE; }

// ---- Clip normalisation: OFF. Kept only for A/B testing ------------
#define NORM_TARGET_PEAK  26000.0f
#define NORM_MAX_GAIN     3.0f
#define NORM_MIN_PEAK     300

// Noise-floor limits, expressed in REC-profile units (v8's pre-gain
// numbers x GAIN_REC). v8's real floor sat around 120-200 pre-gain, so a
// fallback of 300 was far too high and produced a gate of 600 that speech
// could never cross.
#define CAL_FLOOR_MIN      ( 60.0f * cfg.micGain)
#define CAL_FLOOR_MAX      (1200.0f * cfg.micGain)
#define CAL_FLOOR_FALLBACK (140.0f * cfg.micGain)
#define GATE_MULT       1.35f
#define STOP_MIC_DURING_TTS 1
#define TTS_RING_SIZE  (256 * 1024)
#define TTS_PREBUFFER  (64 * 1024)      // ~2.0 s of audio
#define TTS_REBUFFER   (16 * 1024)      // ~0.5 s of audio
#define USE_CONVERSE 1

uint8_t* wavBuffer = nullptr;
uint8_t* ttsRing   = nullptr;   // streaming playback ring (PSRAM)
int16_t* preroll   = nullptr;   // circular pre-speech buffer, PREROLL_SAMPLES
int16_t* pcm       = nullptr;

volatile size_t   ringW = 0, ringR = 0;
volatile bool     ringEof = false, playDone = true;
volatile uint32_t underruns = 0;
volatile int32_t  ttsPeak   = 0;   // loudest |sample| in the current TTS clip

float calibratedNoiseRms = 120.0f;
// runtime-tunable over serial ('e' command) so endpointing can be tuned by ear


struct AudioStats {
  float noiseRms;
  float peakRms;
  float lastGain;
  bool speechDetected;
  bool endedBySilence;
};
AudioStats audioStats = {120.0f, 0.0f, 1.0f, false, false};

// NVS storage for the tuning values (`save` / `load`)
Preferences prefs;

// ================= GLOBAL HTTP =================
WiFiClientSecure tlsClient;
HTTPClient       http;

class GainI2S : public I2SClass {
public:
  using I2SClass::I2SClass;

  volatile float gain    = 1.0f;      // multiples of MIC_UNITY_SCALE
  volatile bool  limiter = true;

  volatile float    liveRms    = 0.0f;   // RMS of the last block
  volatile float    livePeak   = 0.0f;   // |peak| of the last block
  volatile int32_t  rawPeak24  = 0;      // |peak| of the raw 24-bit value
  volatile uint32_t clipCount  = 0;      // hard clips since reset
  volatile uint32_t limitCount = 0;      // soft-limiter engagements

  void resetStats() { clipCount = 0; limitCount = 0; rawPeak24 = 0; }

  // Changing gain rescales the DC estimate rather than resetting it, so
  // there is no settling transient after a change.
  void setGain(float newGain) {
    if (newGain <= 0.0f) return;
    
    gain = newGain;
  }

  void primeDc() {
    size_t got = I2SClass::readBytes((char *)rawbuf, RAW_SAMPLES * sizeof(int32_t));
    int n = got / sizeof(int32_t);
    if (n <= 0) return;
    double sum = 0.0;
    for (int i = 0; i < n; i++) sum += (double)(rawbuf[i] >> 8);   // 24-bit domain
    dc = (float)(sum / n);
  }

  size_t readBytes(char *buffer, size_t size) override {
    const size_t wantSamples = size / sizeof(int16_t);
    int16_t *dst = (int16_t *)buffer;

    size_t done  = 0;
    double sumSq = 0.0;
    float  pk    = 0.0f;
    int32_t rawPk = 0;
    const float g    = gain * MIC_UNITY_SCALE;
    const bool  lim  = limiter;
    const float knee = limitKneeNow();
    const float head = FULL_SCALE - knee;

    while (done < wantSamples) {
      size_t chunk = wantSamples - done;
      if (chunk > RAW_SAMPLES) chunk = RAW_SAMPLES;

      size_t got = I2SClass::readBytes((char *)rawbuf, chunk * sizeof(int32_t));
      size_t n   = got / sizeof(int32_t);
      if (n == 0) break;

      for (size_t i = 0; i < n; i++) {
        // Recover the true 24-bit value FIRST, in full resolution.
        int32_t s24 = rawbuf[i] >> 8;
        int32_t a24 = s24 < 0 ? -s24 : s24;
        if (a24 > rawPk) rawPk = a24;

        float x = (float)s24;
        dc += 0.0015f * (x - dc);          // gentle DC / rumble removal
        float y = (x - dc) * g;            // single float gain

        if (lim) {
          float a = fabsf(y);
          if (a > knee) {                  // soft knee, no hard corner
            float sign = (y < 0.0f) ? -1.0f : 1.0f;
            y = sign * (knee + head * tanhf((a - knee) / head));
            limitCount++;
          }
        }
        if (y >  FULL_SCALE) { y =  FULL_SCALE; clipCount++; }
        if (y < -FULL_SCALE) { y = -FULL_SCALE; clipCount++; }

        dst[done + i] = (int16_t)y;
        sumSq += (double)y * y;
        float ay = fabsf(y);
        if (ay > pk) pk = ay;
      }
      done += n;
    }

    if (done) {
      liveRms  = sqrt(sumSq / done);
      livePeak = pk;
      if (rawPk > rawPeak24) rawPeak24 = rawPk;
    }
    return done * sizeof(int16_t);
  }

private:
  static const size_t RAW_SAMPLES = 1024;
  int32_t rawbuf[RAW_SAMPLES];
  float   dc = 0.0f;      // tracked in the 24-bit pre-gain domain
};

GainI2S  mic(I2S_NUM_1);     // INMP441
I2SClass spk(I2S_NUM_0);     // MAX98357A

// ================= WAKE WORD =================

enum { SR_CMD_HELLO, SR_CMD_STOP };
static const sr_cmd_t sr_commands[] = {
  {SR_CMD_HELLO, "Hello Luna"},
  {SR_CMD_STOP,  "Stop"},
};
#define SR_INPUT_FORMAT     "M"
#define SR_INPUT_CHANNELS   SR_CHANNELS_MONO
#define I2S_OUTPUT_CHANNELS I2S_SLOT_MODE_MONO

volatile bool     wakeFired = false;
volatile uint32_t wakeCount = 0;
volatile bool     srRunning = false;


volatile bool uiEnabled = true;      // 'v' toggles the OLED animation

#define EYE_TICK_MS 30               // was 25; fewer full-frame I2C writes

// ================= EYE EMOTIONS =================
enum EyeEmotion {
  EYE_IDLE,
  EYE_HAPPY,
  EYE_CURIOUS,
  EYE_SLEEPY,
  EYE_LISTENING,
  EYE_THINKING,
  EYE_SPEAKING,
  EYE_ERROR
};
void setEyeMood(int mood);

// ================= DISPLAY MODE =================
enum DisplayMode { MODE_EYES, MODE_TEXT };
volatile int  displayMode = MODE_EYES;
volatile bool textDirty   = false;
String        pendingText = "";

void showStatus(const String& t) {
  pendingText = t;
  textDirty   = true;
  displayMode = MODE_TEXT;
}
void backToEyes() { setEyeMood(EYE_IDLE); displayMode = MODE_EYES; }

// ================= TYPING ANIMATION =================
const int TYPE_DELAY_MS = 40;
const int READ_DELAY_MS = 2000;

// ================= EYES =================
// Fast, expressive OLED eyes for SH1106 I2C.
// Key idea: fewer display.display() calls = faster animation.

struct Eye {
  int ox = 0, oy = 0;             // look offset
  int blink = 0;                  // 0=open, bigger=closed
  int mood = EYE_IDLE;
  unsigned long nextBlink = 0;
  unsigned long nextMove  = 0;
  unsigned long nextMood  = 0;
  unsigned long speakBeat = 0;
  bool mouthOpen = false;
} eye;

const int EYE_W = 30, EYE_H = 36, EYE_R = 8;
const int CX = 64, CY = 31, GAP = 18;

void setEyeMood(int mood) {
  eye.mood = mood;
  eye.blink = 0;
}

void drawEyes() {
  display.clearDisplay();

  int w = EYE_W;
  int h = max(4, EYE_H - eye.blink);
  int r = EYE_R;
  int yPad = (EYE_H - h) / 2;
  int lx = CX - GAP/2 - w + eye.ox;
  int rx = CX + GAP/2 + eye.ox;
  int ty = CY - EYE_H/2 + yPad + eye.oy;

  // Emotion shape tweaks
  if (eye.mood == EYE_SLEEPY) { h = max(5, h / 2); ty += 8; r = 5; }
  if (eye.mood == EYE_LISTENING) { h += 2; ty -= 1; }
  if (eye.mood == EYE_THINKING) { lx -= 2; rx += 2; }
  if (eye.mood == EYE_ERROR) { h = 12; r = 3; }

  // Main eyes
  display.fillRoundRect(lx, ty, w, h, r, SH110X_WHITE);
  display.fillRoundRect(rx, ty, w, h, r, SH110X_WHITE);

  // Happy cheeks / smiling eyes: cut top corners slightly for cute curve
  if (eye.mood == EYE_HAPPY) {
    display.fillTriangle(lx, ty, lx + w, ty, lx + w/2, ty + 10, SH110X_BLACK);
    display.fillTriangle(rx, ty, rx + w, ty, rx + w/2, ty + 10, SH110X_BLACK);
    display.drawPixel(38, 54, SH110X_WHITE);
    display.drawPixel(90, 54, SH110X_WHITE);
  }

  // Curious: one eye taller than the other
  if (eye.mood == EYE_CURIOUS) {
    display.fillRect(rx, ty, w, 7, SH110X_BLACK);
  }

  // Thinking: small dots under eyes
  if (eye.mood == EYE_THINKING) {
    display.fillCircle(54, 55, 1, SH110X_WHITE);
    display.fillCircle(64, 55, 1, SH110X_WHITE);
    display.fillCircle(74, 55, 1, SH110X_WHITE);
  }

  // Speaking: tiny animated mouth
  if (eye.mood == EYE_SPEAKING) {
    if (eye.mouthOpen) display.fillRoundRect(55, 52, 18, 7, 3, SH110X_WHITE);
    else display.drawFastHLine(55, 55, 18, SH110X_WHITE);
  }

  // Error: angry slashes
  if (eye.mood == EYE_ERROR) {
    display.drawLine(lx, ty - 4, lx + w, ty + 4, SH110X_WHITE);
    display.drawLine(rx, ty + 4, rx + w, ty - 4, SH110X_WHITE);
  }

  display.display();
}

// Fast natural blink: only 4 screen updates, not 14.
void doBlink() {
  eye.blink = 18; drawEyes(); vTaskDelay(5 / portTICK_PERIOD_MS);
  eye.blink = 34; drawEyes(); vTaskDelay(28 / portTICK_PERIOD_MS);
  eye.blink = 18; drawEyes(); vTaskDelay(5 / portTICK_PERIOD_MS);
  eye.blink = 0;  drawEyes();
}

void winkLeft() {
  int oldMood = eye.mood;
  display.clearDisplay();
  int lx = CX - GAP/2 - EYE_W + eye.ox;
  int rx = CX + GAP/2 + eye.ox;
  int ty = CY - EYE_H/2 + eye.oy;
  display.drawFastHLine(lx, CY, EYE_W, SH110X_WHITE);
  display.fillRoundRect(rx, ty, EYE_W, EYE_H, EYE_R, SH110X_WHITE);
  display.display();
  vTaskDelay(80 / portTICK_PERIOD_MS);
  eye.mood = oldMood;
  drawEyes();
}

void smoothLook(int tx, int ty) {
  int sx = eye.ox, sy = eye.oy;
  // Fewer steps = less slow-motion effect on I2C OLED
  for (int s = 1; s <= 5; s++) {
    eye.ox = sx + (tx - sx) * s / 5;
    eye.oy = sy + (ty - sy) * s / 5;
    drawEyes();
    vTaskDelay(12 / portTICK_PERIOD_MS);
  }
}

void tinyBounce() {
  int oldY = eye.oy;
  eye.oy = oldY - 2; drawEyes(); vTaskDelay(35 / portTICK_PERIOD_MS);
  eye.oy = oldY + 1; drawEyes(); vTaskDelay(35 / portTICK_PERIOD_MS);
  eye.oy = oldY;     drawEyes();
}

void eyesTick() {
  unsigned long now = millis();

  // Speaking mouth animation
  if (eye.mood == EYE_SPEAKING && now > eye.speakBeat) {
    eye.mouthOpen = !eye.mouthOpen;
    eye.speakBeat = now + 120;
    drawEyes();
    return;
  }

  // Random mood shifts while idle so it does not look robotic
  if (eye.mood == EYE_IDLE && now > eye.nextMood) {
    int r = random(0, 100);
    if (r < 12) { setEyeMood(EYE_HAPPY); tinyBounce(); setEyeMood(EYE_IDLE); }
    else if (r < 24) { setEyeMood(EYE_CURIOUS); drawEyes(); vTaskDelay(300 / portTICK_PERIOD_MS); setEyeMood(EYE_IDLE); }
    else if (r < 30) { setEyeMood(EYE_SLEEPY); drawEyes(); vTaskDelay(350 / portTICK_PERIOD_MS); setEyeMood(EYE_IDLE); }
    else if (r < 36) { winkLeft(); }
    eye.nextMood = now + 3000 + random(0, 5000);
  }

  if (now > eye.nextBlink) {
    doBlink();
    // Sometimes double blink, very human/cute
    if (random(0, 100) < 18) {
      vTaskDelay(90 / portTICK_PERIOD_MS);
      doBlink();
    }
    eye.nextBlink = now + 1800 + random(0, 3500);
  }

  if (now > eye.nextMove) {
    int tx = random(-13, 14);
    int ty = random(-5, 6);
    smoothLook(tx, ty);
    vTaskDelay(random(250, 800) / portTICK_PERIOD_MS);
    smoothLook(0, 0);
    eye.nextMove = now + 2500 + random(0, 5000);
  } else {
    drawEyes();
  }
}

// ================= TEXT WITH TYPING =================

void showTyped(const String& text) {
  display.clearDisplay();
  display.setTextSize(1);
  display.setTextColor(SH110X_WHITE);

  const int maxChars = 21;
  const int maxLines = 8;

  String lines[50];
  int lineIndex = 0;
  String line = "";
  String word = "";

  for (int i = 0; i < (int)text.length(); i++) {
    char c = text[i];
    if (c == ' ' || c == '\n') {
      if ((int)line.length() + (int)word.length() + 1 > maxChars) {
        lines[lineIndex++] = line;
        line = word;
      } else {
        if (line.length()) line += " ";
        line += word;
      }
      word = "";
    } else {
      word += c;
    }
  }
  if (word.length()) {
    if ((int)line.length() + (int)word.length() + 1 > maxChars) {
      lines[lineIndex++] = line;
      line = word;
    } else {
      if (line.length()) line += " ";
      line += word;
    }
  }
  if (line.length()) lines[lineIndex++] = line;

  if (lineIndex <= maxLines) {
    display.clearDisplay();
    int y = 0;
    for (int i = 0; i < lineIndex; i++) {
      display.setCursor(0, y);
      String cur = lines[i];
      for (int j = 0; j < (int)cur.length(); j++) {
        display.print(cur[j]);
        display.display();
        delay(TYPE_DELAY_MS);
      }
      y += 8;
    }
    display.display();
  } else {
    // Scroll page-by-page if longer than screen
    int startLine = 0;
    while (startLine < lineIndex) {
      display.clearDisplay();
      int endLine = min(startLine + maxLines, lineIndex);
      int y = 0;
      for (int i = startLine; i < endLine; i++) {
        display.setCursor(0, y);
        String cur = lines[i];
        for (int j = 0; j < (int)cur.length(); j++) {
          display.print(cur[j]);
          display.display();
          delay(TYPE_DELAY_MS);
        }
        y += 8;
      }
      delay(2500);
      startLine += maxLines;
    }
  }
}

// ================= DISPLAY TASK =================
void displayTask(void *p) {
  bool blanked = false;
  while (true) {
    // 'v' blanks the OLED so you can measure wake-word hit rate with the
    // display's I2C traffic removed from Core 0.
    if (!uiEnabled) {
      if (!blanked) { display.clearDisplay(); display.display(); blanked = true; }
      vTaskDelay(200 / portTICK_PERIOD_MS);
      continue;
    }
    blanked = false;

    if (displayMode == MODE_EYES) {
      eyesTick();
      vTaskDelay(EYE_TICK_MS / portTICK_PERIOD_MS);
    } else {
      if (textDirty) {
        textDirty = false;
        showTyped(pendingText);
      }
      vTaskDelay(50 / portTICK_PERIOD_MS);
    }
  }
}

// ================= HELPERS =================
String cleanText(String s) {
  s.replace("\r", " ");
  s.replace("\n", " ");
  s.replace("\\", "");
  s.trim();
  while (s.indexOf("  ") >= 0) s.replace("  ", " ");
  return s;
}

String jsonEscape(String s) {
  String out;
  for (int i = 0; i < (int)s.length(); i++) {
    char c = s[i];
    if (c == '\\') out += "\\\\";
    else if (c == '\"') out += "\\\"";
    else if (c == '\n' || c == '\r') out += " ";
    else out += c;
  }
  return out;
}


// Set false by sendSTT()/chat() when the HTTP call itself failed, so the UI
// can say "Network error" instead of blaming your voice with "No speech".
volatile bool netOk = true;

void netReset() {
  http.end();
  tlsClient.stop();
  delay(120);
}

bool audioInit() {
  mic.setTimeout(1000);
  mic.setPins(MIC_BCLK, MIC_LRC, -1 /* no DOUT */, MIC_DIN);
  // Raw 32-bit + SLOT_LEFT. Confirmed by the slot diagnostic: LEFT reads
  // the INMP441 (99% nonzero), RIGHT is dead. Matches v8's ONLY_LEFT.
  if (!mic.begin(I2S_MODE_STD, SAMPLE_RATE, I2S_DATA_BIT_WIDTH_32BIT,
                 I2S_OUTPUT_CHANNELS, I2S_STD_SLOT_LEFT)) {
    Serial.println("[err] mic.begin() failed");
    return false;
  }
  mic.setGain(cfg.wakeGain);
  mic.primeDc();                 // no settling transient on the very first read

  spk.setPins(SPK_BCLK, SPK_LRC, SPK_DOUT, -1 /* no DIN */);
  if (!spk.begin(I2S_MODE_STD, SAMPLE_RATE, I2S_DATA_BIT_WIDTH_16BIT,
                 I2S_SLOT_MODE_MONO, I2S_STD_SLOT_LEFT)) {
    Serial.println("[err] spk.begin() failed");
    return false;
  }
  {
    uint8_t quiet[512];
    memset(quiet, 0, sizeof(quiet));
    for (int i = 0; i < 8; i++) spk.write(quiet, sizeof(quiet));
  }

  Serial.println("[audio] mic (I2S1) + speaker (I2S0) both live");
  return true;
}

// Discard a few frames so the DC estimate matches the new scale.
void micFlush(int frames) {
  int16_t tmp[256];
  for (int i = 0; i < frames; i++) mic.readBytes((char*)tmp, sizeof(tmp));
}

// Reopen the mic bus after TTS tore it down. Profile (shift/gain) lives in
// the object, so it survives end()/begin(); only the DC seed needs redoing.
bool micReopen() {
  mic.setPins(MIC_BCLK, MIC_LRC, -1 /* no DOUT */, MIC_DIN);
  if (!mic.begin(I2S_MODE_STD, SAMPLE_RATE, I2S_DATA_BIT_WIDTH_32BIT,
                 I2S_OUTPUT_CHANNELS, I2S_STD_SLOT_LEFT)) return false;
  mic.primeDc();
  return true;
}

// ================= LOCAL AUDIO FRONT END =================
// GainI2S already applied shift + DC removal + gain + limiting, so these
// routines see finished 16-bit PCM. Thresholds are therefore in REC-profile
// units (v8's pre-gain numbers scaled by GAIN_REC).
// Measure the 12th-percentile frame RMS over ~0.4 s of audio.
static float measureNoiseFloor() {
  int16_t raw[FRAME_SAMPLES];
  float frameRms[96];
  // cfg.calibrationMs of audio, at 8 ms per frame, capped to the array.
  int want = constrain(cfg.calibrationMs / 8, 24, 96);
  int collected = 0;

  while (collected < want) {
    size_t got = mic.readBytes((char*)raw, sizeof(raw));
    int n = got / sizeof(int16_t);
    if (n <= 0) continue;
    double sumSq = 0.0;
    for (int i = 0; i < n; ++i) sumSq += (double)raw[i] * raw[i];
    frameRms[collected++] = sqrt(sumSq / n);
  }
  for (int i = 1; i < collected; ++i) {           // insertion sort
    float key = frameRms[i];
    int j = i - 1;
    while (j >= 0 && frameRms[j] > key) { frameRms[j + 1] = frameRms[j]; --j; }
    frameRms[j + 1] = key;
  }
  Serial.printf("[audio]   p12=%.1f median=%.1f max=%.1f\n",
                frameRms[collected / 8], frameRms[collected / 2], frameRms[collected - 1]);
  return frameRms[collected / 8];
}

void calibrateMicNoise() {
  // ORDER MATTERS. v9.0 called primeDc() BEFORE flushing, so it seeded the
  // DC estimate from I2S startup garbage. That saturated the calibration on
  // every single boot (p12=5109, median=15594, max=32732) and forced the
  // fallback, which produced a VAD gate of 600 that speech never crossed.
  micFlush(24);        // 1. dump the startup transient
  mic.primeDc();       // 2. seed DC from real streaming audio
  micFlush(8);         // 3. let the high-pass settle at that DC

  float p12 = measureNoiseFloor();

  if (p12 >= CAL_FLOOR_MAX) {              // one retry before giving up
    Serial.println("[audio] first pass hot, re-seeding and retrying");
    micFlush(16);
    mic.primeDc();
    micFlush(8);
    p12 = measureNoiseFloor();
  }

  if (p12 >= CAL_FLOOR_MAX) {
    calibratedNoiseRms = CAL_FLOOR_FALLBACK;
    Serial.printf("[audio] WARNING still saturated -> fallback floor %.1f\n",
                  calibratedNoiseRms);
  } else {
    calibratedNoiseRms = constrain(p12, CAL_FLOOR_MIN, CAL_FLOOR_MAX);
  }
  audioStats.noiseRms = calibratedNoiseRms;
  Serial.printf("[audio] boot floor=%.1f (SEED ONLY -- the real gate is tracked live)\n",
                calibratedNoiseRms);
}

// force = true  -> wake-word mode: wait for speech, record the utterance,
//                  stop on end-of-speech silence.
// force = false -> button mode: record while the button is held (v8 behaviour).
//
// In wake mode nothing is written to the main buffer until you actually
// start talking, so MAX_RECORD_SECONDS applies to speech only.
int recordAudio(bool force = false) {
  int16_t raw[128];
  int count = 0;

  // Live noise tracking. calibratedNoiseRms is only a SEED now -- it has
  // been wrong three revisions running, so it is clamped hard and then
  // corrected from the actual room within ~100 ms of waiting.
  float noiseEst  = constrain(calibratedNoiseRms, noiseMinNow(), 350.0f * cfg.micGain);
  float startGate = constrain(noiseEst * cfg.noiseMult, gateMinNow(), gateMaxNow());
  float contGate  = startGate * cfg.contMult;

  const int minSpeechFrames  = max(1, (cfg.minSpeechMs * SAMPLE_RATE / 1000) / 128);
  const int endSilenceFrames = max(1, (cfg.endSilenceMs * SAMPLE_RATE / 1000) / 128);
  const int minRecordSamples = SAMPLE_RATE * MIN_RECORD_MS   / 1000;
  const int minUttSamples    = SAMPLE_RATE * MIN_UTTERANCE_MS / 1000;

  Serial.printf("[audio] seed noise=%.0f gate=%.0f endSil=%dms force=%d\n",
                noiseEst, startGate, cfg.endSilenceMs, (int)force);

  int  voicedFrames = 0, silentFrames = 0, lowFrames = 0, endedBy = 0;
  bool speechStarted = force ? false : true;   // button mode records immediately
  unsigned long waitStart = millis();

  // circular pre-roll (wake mode only)
  int prW = 0, prCount = 0;

  audioStats = {calibratedNoiseRms, 0.0f, cfg.micGain, false, false};

  while (true) {
    if (!force && digitalRead(BUTTON_PIN) != LOW && count >= minRecordSamples) break;

    size_t got = mic.readBytes((char*)raw, sizeof(raw));
    int n = got / sizeof(int16_t);
    if (n <= 0) continue;

    double sumSq = 0.0;
    for (int i = 0; i < n; ++i) sumSq += (double)raw[i] * raw[i];
    float frameRms = sqrt(sumSq / n);
    if (frameRms > audioStats.peakRms) audioStats.peakRms = frameRms;

    // ---------- PHASE 1: waiting for you to start ----------
    if (!speechStarted) {
      // Track the MEAN of non-speech frames, not the minimum. A frame well
      // above the current estimate is probably speech, so it only creeps --
      // that is what stops the estimate diving to 73 in a 250-floor room.
      if (frameRms < noiseEst * NOISE_EXCLUDE)
           noiseEst += NOISE_ADAPT * (frameRms - noiseEst);
      else noiseEst += NOISE_CREEP * (frameRms - noiseEst);
      noiseEst  = constrain(noiseEst, noiseMinNow(), noiseMaxNow());
      startGate = constrain(noiseEst * cfg.noiseMult, gateMinNow(), gateMaxNow());

      for (int i = 0; i < n; ++i) {          // keep the last PREROLL_MS
        preroll[prW] = raw[i];
        prW = (prW + 1) % PREROLL_SAMPLES;
      }
      prCount = min(PREROLL_SAMPLES, prCount + n);

      if (frameRms > startGate) {
        voicedFrames++;
        if (voicedFrames >= minSpeechFrames) {
          speechStarted = true;
          silentFrames  = 0;
          contGate = startGate * cfg.contMult;     // freeze for this utterance
          // Flush the pre-roll so the first consonant is not clipped.
          int start = (prW - prCount + PREROLL_SAMPLES) % PREROLL_SAMPLES;
          for (int i = 0; i < prCount && count < MAX_SAMPLES; ++i)
            pcm[count++] = preroll[(start + i) % PREROLL_SAMPLES];
          Serial.printf("[audio] speech start rms=%.0f noise=%.0f gate=%.0f cont=%.0f preroll=%dms\n",
                        frameRms, noiseEst, startGate, contGate,
                        (prCount * 1000) / SAMPLE_RATE);
        }
      } else {
        voicedFrames = 0;
        if (millis() - waitStart > (unsigned long)cfg.wakeWaitMs) {
          Serial.printf("[audio] nobody spoke (noise=%.0f gate=%.0f peak=%.0f)\n",
                        noiseEst, startGate, audioStats.peakRms);
          break;
        }
      }
      continue;                              // nothing written to pcm yet
    }

    // ---------- PHASE 2: recording the utterance ----------
    for (int i = 0; i < n && count < MAX_SAMPLES; ++i) pcm[count++] = raw[i];
    if (count >= MAX_SAMPLES) {
      Serial.println("[audio] hit max length");
      break;
    }

    if (frameRms > contGate)  silentFrames = 0; else silentFrames++;
    if (frameRms > startGate) lowFrames    = 0; else lowFrames++;

    if (force && count >= minUttSamples) {
      if (silentFrames >= endSilenceFrames) {
        audioStats.endedBySilence = true;
        endedBy = silentFrames;
        break;
      }
      if (lowFrames >= (endSilenceFrames * 3) / 2) {
        audioStats.endedBySilence = true;
        endedBy = lowFrames;
        Serial.println("[audio] endpoint via SAFETY gate (cont gate too low for this room)");
        break;
      }
    }
  }

  // ---------- PHASE 3: trim the trailing silence ----------
  if (audioStats.endedBySilence) {
    int keep = SAMPLE_RATE * TRAIL_KEEP_MS / 1000;
    int trim = (endedBy * 128) - keep;
    if (trim > 0 && (count - trim) >= minUttSamples) {
      count -= trim;
      Serial.printf("[audio] trimmed %dms of trailing silence\n",
                    (trim * 1000) / SAMPLE_RATE);
    }
  }

  audioStats.speechDetected = speechStarted;
  Serial.printf("[audio] samples=%d seconds=%.2f speech=%d silenceStop=%d peakRMS=%.0f\n",
                count, count / (float)SAMPLE_RATE, speechStarted,
                audioStats.endedBySilence, audioStats.peakRms);
  return speechStarted ? count : 0;
}

// Bring the finished utterance up to a consistent level before upload.
// Uniform gain over the whole clip: safe for ASR, unlike per-frame AGC.
void normalizeClip(int16_t *buf, int count) {
  if (!cfg.normOn) return;                 // default: off, see tuning guide
  if (count <= 0) return;

  int32_t peak = 0;
  for (int i = 0; i < count; i++) {
    int32_t a = buf[i] < 0 ? -(int32_t)buf[i] : (int32_t)buf[i];
    if (a > peak) peak = a;
  }
  if (peak < NORM_MIN_PEAK) {
    Serial.printf("[audio] too quiet to normalise (peak=%ld)\n", (long)peak);
    return;
  }

  float g = NORM_TARGET_PEAK / (float)peak;
  if (g < 1.0f) g = 1.0f;                  // never attenuate
  if (g > NORM_MAX_GAIN) g = NORM_MAX_GAIN;
  if (g <= 1.05f) {
    Serial.printf("[audio] already hot, peak=%ld, no normalise\n", (long)peak);
    return;
  }

  for (int i = 0; i < count; i++) {
    float v = (float)buf[i] * g;
    if (v >  32767.0f) v =  32767.0f;
    if (v < -32768.0f) v = -32768.0f;
    buf[i] = (int16_t)v;
  }
  Serial.printf("[audio] normalised peak %ld -> %ld (x%.2f)\n",
                (long)peak, (long)(peak * g), g);
}

// ================= WAV =================
void createWav(uint8_t* wav, int samples) {
  int dataSize = samples * 2;
  memcpy(wav, "RIFF", 4);
  *(int*)(wav + 4)    = 36 + dataSize;
  memcpy(wav + 8, "WAVEfmt ", 8);
  *(int*)(wav + 16)   = 16;
  *(short*)(wav + 20) = 1;
  *(short*)(wav + 22) = 1;
  *(int*)(wav + 24)   = SAMPLE_RATE;
  *(int*)(wav + 28)   = SAMPLE_RATE * 2;
  *(short*)(wav + 32) = 2;
  *(short*)(wav + 34) = 16;
  memcpy(wav + 36, "data", 4);
  *(int*)(wav + 40)   = dataSize;
}

// ================= STT =================
String sendSTT(uint8_t* wav, int size) {
  netOk = false;
  // Two attempts: a -7 is almost always a stale/starved TLS session rather
  // than a real server problem, and a clean reconnect fixes it.
  for (int attempt = 1; attempt <= 2; attempt++) {
    tlsClient.setInsecure();
    http.setReuse(false);          // do NOT hold the session between turns
    http.setTimeout(30000);

    if (!http.begin(tlsClient, sttURL)) { netReset(); continue; }
    http.addHeader("Content-Type", "audio/wav");

    int code = http.POST(wav, size);
    Serial.printf("[stt] try%d code=%d heap=%u\n", attempt, code, ESP.getFreeHeap());

    if (code == 200) {
      String res = http.getString();
      netReset();
      netOk = true;
      return cleanText(res);
    }
    netReset();
    if (attempt == 1) {
      Serial.println("[stt] retrying after clean reconnect");
      delay(400);
    }
  }
  return "";
}

// ================= CHAT =================
String chat(String msg) {
  netOk = false;
  String body = "{\"message\":\"" + jsonEscape(msg) + "\",\"max_tokens\":60}";

  for (int attempt = 1; attempt <= 2; attempt++) {
    tlsClient.setInsecure();
    http.setReuse(false);
    http.setTimeout(30000);

    if (!http.begin(tlsClient, chatURL)) { netReset(); continue; }
    http.addHeader("Content-Type", "application/json");

    int code = http.POST(body);
    Serial.printf("[chat] try%d code=%d heap=%u\n", attempt, code, ESP.getFreeHeap());

    if (code == 200) {
      String res = http.getString();
      netReset();
      netOk = true;
      return cleanText(res);
    }
    netReset();
    if (attempt == 1) delay(400);
  }
  return "Server error";
}


// ================= STREAMING PLAYBACK =================
// Consumer: drains the ring into I2S. Runs on Core 0 at priority 6 so
// neither the display task (1) nor the paused SR tasks (5) can stall it.
void ttsPlayTask(void *p) {
  uint8_t chunk[1024];
  uint8_t quiet[512];
  memset(quiet, 0, sizeof(quiet));
  bool rebuffering = false;

  while (true) {
    size_t avail = ringW - ringR;

    if (avail == 0 && ringEof) break;          // clean end of clip

    if (avail == 0) {
      if (!rebuffering) { underruns++; rebuffering = true; }
      spk.write(quiet, sizeof(quiet));
      continue;
    }

    // Hysteresis: after a dry spell, rebuild a cushion before resuming
    // instead of restarting on the first byte and stuttering again.
    if (rebuffering) {
      if (avail < TTS_REBUFFER && !ringEof) {
        spk.write(quiet, sizeof(quiet));
        continue;
      }
      rebuffering = false;
    }

    size_t n     = avail < sizeof(chunk) ? avail : sizeof(chunk);
    size_t off   = ringR % TTS_RING_SIZE;
    size_t first = (TTS_RING_SIZE - off) < n ? (TTS_RING_SIZE - off) : n;
    memcpy(chunk, ttsRing + off, first);
    if (n > first) memcpy(chunk + first, ttsRing, n - first);

   
    {
      const int16_t *sm = (const int16_t *)chunk;
      size_t ns = n / 2;
      for (size_t i = 0; i < ns; i++) {
        int32_t a = sm[i] < 0 ? -(int32_t)sm[i] : (int32_t)sm[i];
        if (a > ttsPeak) ttsPeak = a;
      }
    }

    spk.write(chunk, n);
    ringR += n;
  }

  uint8_t silence[256];
  memset(silence, 0, sizeof(silence));
  spk.write(silence, sizeof(silence));     // clean tail, no pop
  playDone = true;
  vTaskDelete(NULL);
}

// Producer: pumps an HTTP body into the ring and starts the player as soon
// as TTS_PREBUFFER is available. skipBytes drops the 44-byte WAV header.
void streamPlayback(WiFiClient *stream, size_t skipBytes) {
  ringW = ringR = 0;
  ringEof = false;
  playDone = false;
  underruns = 0;
  ttsPeak   = 0;

  unsigned long t0 = millis();
  size_t skipped = 0;
  while (skipped < skipBytes && millis() - t0 < 3000) {
    if (stream->available()) { stream->read(); skipped++; }
    else delay(1);
  }

#if STOP_MIC_DURING_TTS
  mic.end();                      // free the RX path, as v8 did
#endif

  TaskHandle_t th = nullptr;
  bool started = false;
  uint8_t tmp[2048];
  unsigned long lastData = millis();

  while (true) {
    size_t space = TTS_RING_SIZE - (ringW - ringR);
    if (space <= 1024) {          // ring full: let the player drain it.
      delay(2);                   // NOTE: does not count toward the idle
      continue;                   // timeout, or long clips would abort.
    }

    int avail = stream->available();
    if (avail <= 0) {
      if (millis() - lastData > 3000) break;   // end of body
      delay(1);
      continue;
    }

    size_t want = space < sizeof(tmp) ? space : sizeof(tmp);
    if ((size_t)avail < want) want = (size_t)avail;
    int n = stream->readBytes(tmp, want);
    if (n > 0) {
      size_t off   = ringW % TTS_RING_SIZE;
      size_t first = (TTS_RING_SIZE - off) < (size_t)n ? (TTS_RING_SIZE - off) : (size_t)n;
      memcpy(ttsRing + off, tmp, first);
      if ((size_t)n > first) memcpy(ttsRing, tmp + first, (size_t)n - first);
      ringW += (size_t)n;
      lastData = millis();
    }

    if (!started && (ringW - ringR) >= TTS_PREBUFFER) {
      started = true;
      xTaskCreatePinnedToCore(ttsPlayTask, "TTSPlay", 4096, NULL, 6, &th, 0);
      Serial.printf("[tts] audio starts at %lums (%uKB buffered)\n",
                    (unsigned long)(millis() - t0),
                    (unsigned)((ringW - ringR) / 1024));
    }
  }

  ringEof = true;
  if (!started) {                 // clip shorter than the pre-buffer
    xTaskCreatePinnedToCore(ttsPlayTask, "TTSPlay", 4096, NULL, 6, &th, 0);
  }
  while (!playDone) delay(5);

  Serial.printf("[tts] done %uKB in %lums underruns=%lu peak=%ld (%d%% of full scale)\n",
                (unsigned)(ringW / 1024),
                (unsigned long)(millis() - t0),
                (unsigned long)underruns,
                (long)ttsPeak,
                (int)((100L * ttsPeak) / 32767L));

#if STOP_MIC_DURING_TTS
  if (!micReopen()) Serial.println("[err] mic reopen failed after TTS");
#endif
}

// Server sends X-Reply / X-Transcript through encodeURIComponent.
String urlDecode(const String &s) {
  String out;
  out.reserve(s.length());
  for (int i = 0; i < (int)s.length(); i++) {
    char c = s[i];
    if (c == '%' && i + 2 < (int)s.length()) {
      char h[3] = { s[i + 1], s[i + 2], 0 };
      out += (char)strtol(h, nullptr, 16);
      i += 2;
    } else if (c == '+') out += ' ';
    else out += c;
  }
  return out;
}

// ================= SPEAK (legacy TTS path) =================
void speak(String text) {
  text = cleanText(text);
  if (text.length() == 0) return;

  tlsClient.setInsecure();
  http.setReuse(false);
  http.setTimeout(30000);

  if (!http.begin(tlsClient, ttsURL)) return;
  http.addHeader("Content-Type", "application/json");

  String body = "{\"text\":\"" + jsonEscape(text) + "\"}";
  int code = http.POST(body);
  Serial.printf("[tts] code=%d heap=%u\n", code, ESP.getFreeHeap());
  if (code != 200) { netReset(); return; }

  WiFiClient *stream = http.getStreamPtr();
  if (stream) streamPlayback(stream, 44);
  netReset();
}

// ================= CONVERSE (fast path) =================

bool converse(uint8_t *wav, int size, String &replyOut, bool &memoryMode) {
  netOk = false;
  memoryMode = false;
  const char *hdrs[] = { "X-Reply", "X-Transcript" };

  for (int attempt = 1; attempt <= 2; attempt++) {
    tlsClient.setInsecure();
    http.setReuse(false);
    http.setTimeout(30000);

    if (!http.begin(tlsClient, converseURL)) { netReset(); continue; }
    http.collectHeaders(hdrs, 2);
    http.addHeader("Content-Type", "audio/wav");

    int code = http.POST(wav, size);
    Serial.printf("[converse] try%d code=%d heap=%u\n", attempt, code, ESP.getFreeHeap());

    if (code == 204) {                       // server wants memory mode
      memoryMode = true;
      netOk = true;
      netReset();
      return true;
    }

    if (code == 200) {
      replyOut = urlDecode(http.header("X-Reply"));
      String heard = urlDecode(http.header("X-Transcript"));
      Serial.printf("[converse] heard=\"%s\"\n", heard.c_str());
      Serial.printf("[converse] reply=\"%s\"\n", replyOut.c_str());

      showStatus(replyOut);                  // OLED types while audio plays
      setEyeMood(EYE_SPEAKING);

      WiFiClient *stream = http.getStreamPtr();
      if (stream) streamPlayback(stream, 44);
      netReset();
      netOk = true;
      return true;
    }

    netReset();
    if (attempt == 1) { Serial.println("[converse] retrying"); delay(400); }
  }
  return false;
}

// ================= ONE CONVERSATION TURN =================
void memoryModeFlow(bool fromWake) {
  setEyeMood(EYE_HAPPY);
  showStatus("What should I remember?");
  setEyeMood(EYE_SPEAKING);

  if (fromWake) {
    speak("What should I remember? Tell me now.");
    setEyeMood(EYE_LISTENING);
    showStatus("Listening...");
  } else {
    speak("What should I remember? Hold the button and tell me.");
    setEyeMood(EYE_LISTENING);
    showStatus("Hold button & speak");
    unsigned long waitStart = millis();
    while (digitalRead(BUTTON_PIN) != LOW) {
      if (millis() - waitStart > 8000) {
        setEyeMood(EYE_ERROR); showStatus("Cancelled");
        delay(700); backToEyes(); return;
      }
      delay(10);
    }
    delay(50);
    showStatus("Listening...");
  }

  int samples2 = recordAudio(fromWake);
  if (samples2 < 400) {
    setEyeMood(EYE_ERROR); showStatus("No speech");
    delay(700); backToEyes(); return;
  }

  normalizeClip(pcm, samples2);
  createWav(wavBuffer, samples2);
  setEyeMood(EYE_THINKING);
  showStatus("Saving...");
  String fact = sendSTT(wavBuffer, 44 + samples2 * 2);
  if (!netOk) {
    setEyeMood(EYE_ERROR); showStatus("Network error");
    delay(900); backToEyes(); return;
  }
  if (fact == "" || fact == "Thank you.") {
    setEyeMood(EYE_ERROR); showStatus("Didn't catch that");
    delay(700); backToEyes(); return;
  }

  String finalReply = chat("remember " + fact);
  showStatus(finalReply);
  setEyeMood(EYE_SPEAKING);
  speak(finalReply);
  delay(READ_DELAY_MS);
  backToEyes();
}

void conversationTurn(bool fromWake) {
  setEyeMood(EYE_LISTENING);
  showStatus(fromWake ? "Listening..." : "Speak...");

  int samples = recordAudio(fromWake);
  if (samples < 400) {
    setEyeMood(EYE_ERROR);
    showStatus(fromWake ? "Didn't hear you" : "No speech");
    delay(700); backToEyes(); return;
  }

  int wavSize = 44 + samples * 2;
  if (wavSize > MAX_WAV_SIZE) {
    setEyeMood(EYE_ERROR); showStatus("Too long");
    delay(700); backToEyes(); return;
  }

  normalizeClip(pcm, samples);
  createWav(wavBuffer, samples);
  setEyeMood(EYE_THINKING);
  showStatus("Thinking...");

#if USE_CONVERSE
  // ---------- FAST PATH: one round trip ----------
 
  {
    String reply;
    bool   memMode = false;
    unsigned long t0 = millis();

    if (converse(wavBuffer, wavSize, reply, memMode)) {
      Serial.printf("[turn] converse path took %lums\n",
                    (unsigned long)(millis() - t0));
      if (memMode) { memoryModeFlow(fromWake); return; }
      delay(READ_DELAY_MS);
      backToEyes();
      return;
    }
    Serial.println("[turn] converse failed -> falling back to legacy pipeline");
  }
#endif

  // ---------- LEGACY PATH: STT -> chat -> TTS ----------
  String text = sendSTT(wavBuffer, wavSize);
  if (!netOk) {
    setEyeMood(EYE_ERROR); showStatus("Network error");
    delay(900); backToEyes(); return;
  }
  if (text == "" || text == "Thank you.") {
    setEyeMood(EYE_ERROR); showStatus("Didn't catch that");
    delay(700); backToEyes(); return;
  }

  String reply = chat(text);
  if (reply == "MEMORY_MODE") { memoryModeFlow(fromWake); return; }

  showStatus(reply);
  setEyeMood(EYE_SPEAKING);
  speak(reply);
  delay(READ_DELAY_MS);
  backToEyes();
}

void handleConversation(bool fromWake) {
  if (srRunning) ESP_SR.pause();
  mic.setGain(cfg.micGain);
  mic.primeDc();                 // safe: SR is paused, we own the bus
  micFlush(4);

  conversationTurn(fromWake);

  // Drop the TLS session before going back to idle. Holding it left only
  // ~73 KB free, and the first request of the NEXT turn then failed with
  // error(-7). Every -7 in the v9.5 log followed a round that ended at 73 KB.
  netReset();
  Serial.printf("[net] session released, heap=%u\n", ESP.getFreeHeap());

  mic.setGain(cfg.wakeGain);
  mic.primeDc();
  micFlush(4);
  if (srRunning) ESP_SR.resume();
  Serial.printf("[round end] heap=%u wake=%lu\n",
                ESP.getFreeHeap(), (unsigned long)wakeCount);
}

// ================= SR EVENTS =================
void onSrEvent(sr_event_t event, int command_id, int phrase_id) {
  switch (event) {
    case SR_EVENT_WAKEWORD:
    case SR_EVENT_WAKEWORD_CHANNEL:
      wakeCount++;
      wakeFired = true;
      Serial.printf("[wake] #%lu rms=%.0f\n",
                    (unsigned long)wakeCount, mic.liveRms);
      break;
    case SR_EVENT_TIMEOUT:
      ESP_SR.setMode(SR_MODE_WAKEWORD);
      break;
    default: break;
  }
}

// ================= SERIAL TUNING =================
// ====================================================================
//                  AUDIO DIAGNOSTICS  +  TUNING CONSOLE
// ====================================================================

// Convert an int16 level to dBFS for readable diagnostics.
static float dbfs(float level) {
  if (level < 1.0f) return -99.0f;
  return 20.0f * log10f(level / FULL_SCALE);
}

void printAudioStatus() {
  float startGate = constrain(calibratedNoiseRms * cfg.noiseMult,
                              gateMinNow(), gateMaxNow());
  float contGate  = startGate * cfg.contMult;
  float snr = (calibratedNoiseRms > 1.0f)
                ? 20.0f * log10f(max(1.0f, (float)mic.livePeak) / calibratedNoiseRms)
                : 0.0f;

  Serial.println();
  Serial.println(F("===== LUNA AUDIO STATUS ====="));
  Serial.printf("Sample Rate    : %d Hz\n", SAMPLE_RATE);
  Serial.printf("Bits           : 16 (from 24-bit I2S, float gain path)\n");
  Serial.printf("Mic Gain       : %.2f   (1.00 == legacy v9.9)\n", cfg.micGain);
  Serial.printf("Wake Gain      : %.2f\n", cfg.wakeGain);
  Serial.println(F("-----------------------------"));
  Serial.printf("Noise RMS      : %.0f   (%.1f dBFS)\n",
                calibratedNoiseRms, dbfs(calibratedNoiseRms));
  Serial.printf("VAD Start      : %.0f   (noise x %.2f)\n", startGate, cfg.noiseMult);
  Serial.printf("VAD End        : %.0f   (start x %.2f)\n", contGate, cfg.contMult);
  Serial.printf("Gate rails     : %.0f .. %.0f  (auto-scaled by micGain)\n",
                gateMinNow(), gateMaxNow());
  Serial.println(F("-----------------------------"));
  Serial.printf("Min Speech     : %d ms\n",  cfg.minSpeechMs);
  Serial.printf("End Silence    : %d ms\n",  cfg.endSilenceMs);
  Serial.printf("Wake Wait      : %d ms\n",  cfg.wakeWaitMs);
  Serial.printf("Calibration    : %d ms\n",  cfg.calibrationMs);
  Serial.println(F("-----------------------------"));
  Serial.printf("Live RMS       : %.0f   (%.1f dBFS)\n", mic.liveRms, dbfs(mic.liveRms));
  Serial.printf("Live Peak      : %.0f   (%.1f dBFS)\n", mic.livePeak, dbfs(mic.livePeak));
  Serial.printf("Raw peak (24b) : %ld  of 8388608\n", (long)mic.rawPeak24);
  Serial.printf("Est. SNR       : %.1f dB\n", snr);
  Serial.printf("Limiter hits   : %lu\n", (unsigned long)mic.limitCount);
  Serial.printf("Clipping       : %lu %s\n", (unsigned long)mic.clipCount,
                mic.clipCount ? "  <-- REDUCE gain" : "");
  Serial.println(F("-----------------------------"));
  Serial.printf("PSRAM buffer   : %s  (free %u KB)\n",
                (wavBuffer && ttsRing && preroll) ? "OK" : "FAIL",
                ESP.getFreePsram() / 1024);
  Serial.printf("Heap free      : %u\n", ESP.getFreeHeap());
  Serial.printf("Wake detects   : %lu\n", (unsigned long)wakeCount);
  Serial.printf("Normalisation  : %s\n", cfg.normOn ? "ON" : "off");
  Serial.println(F("============================="));
  Serial.println();
}

void printHelp() {
  Serial.println();
  Serial.println(F("===== LUNA TUNING COMMANDS ====="));
  Serial.println(F("  status            show the full audio status block"));
  Serial.println(F("  meter             toggle live RMS/peak meter (for distance tests)"));
  Serial.println(F("  calibrate         re-measure the room noise floor (stay quiet)"));
  Serial.println(F("  test              guided 20cm / 50cm / 1m measurement"));
  Serial.println(F("---- tuning (value shown is current) ----"));
  Serial.printf ("  gain <0.25-8.0>   mic gain, recording path   [%.2f]\n", cfg.micGain);
  Serial.printf ("  wakegain <1-20>   mic gain, wake-word path   [%.2f]\n", cfg.wakeGain);
  Serial.printf ("  noise <1.2-4.0>   speech must beat floor x   [%.2f]\n", cfg.noiseMult);
  Serial.printf ("  cont <0.4-0.95>   stay-in-speech fraction    [%.2f]\n", cfg.contMult);
  Serial.printf ("  silence <300-3000> end-of-speech silence ms  [%d]\n",  cfg.endSilenceMs);
  Serial.printf ("  minspeech <40-400> voiced run to start ms    [%d]\n",  cfg.minSpeechMs);
  Serial.printf ("  wait <2000-30000> wait-for-speech ms         [%d]\n",  cfg.wakeWaitMs);
  Serial.printf ("  caltime <200-2000> calibration length ms     [%d]\n",  cfg.calibrationMs);
  Serial.printf ("  knee <0.5-1.0>    soft limiter threshold     [%.2f]\n", cfg.limitKnee);
  Serial.println(F("---- persistence ----"));
  Serial.println(F("  save              write settings to NVS (survives reboot)"));
  Serial.println(F("  load              reload saved settings"));
  Serial.println(F("  defaults          restore built-in defaults (does not save)"));
  Serial.println(F("---- other ----"));
  Serial.println(F("  norm              toggle clip normalisation (leave OFF)"));
  Serial.println(F("  oled              toggle OLED animation (wake-rate A/B test)"));
  Serial.println(F("  reset             zero the wake counter and audio stats"));
  Serial.println(F("================================"));
  Serial.println();
}

// ---- NVS persistence -----------------------------------------------
void saveConfig() {
  prefs.begin("luna", false);
  prefs.putFloat("micGain",   cfg.micGain);
  prefs.putFloat("wakeGain",  cfg.wakeGain);
  prefs.putFloat("noiseMult", cfg.noiseMult);
  prefs.putFloat("contMult",  cfg.contMult);
  prefs.putInt  ("minSpeech", cfg.minSpeechMs);
  prefs.putInt  ("endSil",    cfg.endSilenceMs);
  prefs.putInt  ("wakeWait",  cfg.wakeWaitMs);
  prefs.putInt  ("calMs",     cfg.calibrationMs);
  prefs.putFloat("knee",      cfg.limitKnee);
  prefs.putBool ("norm",      cfg.normOn);
  prefs.end();
  Serial.println("[cfg] saved to NVS -- these values now survive reboot");
}

void loadConfig() {
  prefs.begin("luna", true);
  cfg.micGain       = prefs.getFloat("micGain",   cfg.micGain);
  cfg.wakeGain      = prefs.getFloat("wakeGain",  cfg.wakeGain);
  cfg.noiseMult     = prefs.getFloat("noiseMult", cfg.noiseMult);
  cfg.contMult      = prefs.getFloat("contMult",  cfg.contMult);
  cfg.minSpeechMs   = prefs.getInt  ("minSpeech", cfg.minSpeechMs);
  cfg.endSilenceMs  = prefs.getInt  ("endSil",    cfg.endSilenceMs);
  cfg.wakeWaitMs    = prefs.getInt  ("wakeWait",  cfg.wakeWaitMs);
  cfg.calibrationMs = prefs.getInt  ("calMs",     cfg.calibrationMs);
  cfg.limitKnee     = prefs.getFloat("knee",      cfg.limitKnee);
  cfg.normOn        = prefs.getBool ("norm",      cfg.normOn);
  prefs.end();
}

// Re-measure the noise floor safely (pauses SR, switches to the REC gain).
void runCalibration() {
  Serial.println("\n[cal] Calibrating -- keep the room QUIET...");
  if (srRunning) ESP_SR.pause();
  mic.setGain(cfg.micGain);
  calibrateMicNoise();
  mic.setGain(cfg.wakeGain);
  mic.primeDc();
  if (srRunning) ESP_SR.resume();
  Serial.printf("[cal] done. noise floor = %.0f  -> VAD start = %.0f\n\n",
                calibratedNoiseRms,
                constrain(calibratedNoiseRms * cfg.noiseMult, gateMinNow(), gateMaxNow()));
}

// Guided distance measurement, exactly the A/B/C/D procedure in the brief.
void runDistanceTest() {
  const char *steps[] = { "QUIET ROOM - say nothing",
                          "SPEAK at 20 cm",
                          "SPEAK at 50 cm",
                          "SPEAK at 1 metre" };
  Serial.println("\n===== MICROPHONE DISTANCE TEST =====");
  Serial.printf("micGain = %.2f. Each step measures for 5 seconds.\n", cfg.micGain);

  if (srRunning) ESP_SR.pause();
  mic.setGain(cfg.micGain);
  mic.primeDc();
  micFlush(8);

  float results[4] = {0,0,0,0};
  for (int s = 0; s < 4; s++) {
    Serial.printf("\n[%d/4] %s ... starting in 2 s\n", s+1, steps[s]);
    delay(2000);
    Serial.println("      measuring...");
    int16_t buf[FRAME_SAMPLES];
    float   peakRms = 0; int32_t peakSamp = 0; uint32_t clips = 0;
    double  sumSq = 0; long n = 0;
    unsigned long t0 = millis();
    while (millis() - t0 < 5000) {
      size_t got = mic.readBytes((char*)buf, sizeof(buf));
      int m = got / sizeof(int16_t);
      if (m <= 0) continue;
      double fs = 0;
      for (int i = 0; i < m; i++) {
        int32_t a = buf[i] < 0 ? -buf[i] : buf[i];
        if (a > peakSamp) peakSamp = a;
        if (a >= 32760) clips++;
        fs += (double)buf[i] * buf[i];
        sumSq += (double)buf[i] * buf[i];
        n++;
      }
      float fr = sqrt(fs / m);
      if (fr > peakRms) peakRms = fr;
    }
    float avgRms = n ? sqrt(sumSq / n) : 0;
    results[s] = peakRms;
    Serial.printf("      avgRMS=%-6.0f peakRMS=%-6.0f peak=%-6ld (%.1f dBFS) clips=%lu\n",
                  avgRms, peakRms, (long)peakSamp, dbfs(peakSamp), (unsigned long)clips);
  }

  Serial.println("\n----- RESULT -----");
  float noise = results[0];
  for (int s = 1; s < 4; s++) {
    float snr = (noise > 1) ? 20.0f*log10f(max(1.0f,results[s])/noise) : 0;
    Serial.printf("  %-14s peakRMS %-7.0f SNR %5.1f dB   %s\n",
                  steps[s]+6, results[s], snr,
                  snr >= 18 ? "excellent" : snr >= 12 ? "usable" : "TOO NOISY");
  }
  float need = 6000.0f / max(1.0f, results[3]);       // aim ~6000 peakRMS at 1 m
  Serial.printf("\n  Suggested gain for good 1 m pickup: %.2f  (currently %.2f)\n",
                constrain(cfg.micGain * need, 0.25f, 8.0f), cfg.micGain);
  Serial.println("  If clips > 0 at 20 cm, reduce gain instead.");
  Serial.println("==================================\n");

  mic.setGain(cfg.wakeGain);
  mic.primeDc();
  if (srRunning) ESP_SR.resume();
}

// ---- command parser -------------------------------------------------
static bool argFloat(const String &line, const char *cmd, float lo, float hi, float &out) {
  String v = line.substring(strlen(cmd));
  v.trim();
  if (!v.length()) return false;
  float f = v.toFloat();
  if (f < lo || f > hi) {
    Serial.printf("[cfg] value must be %.2f .. %.2f\n", lo, hi);
    return false;
  }
  out = f;
  return true;
}
static bool argInt(const String &line, const char *cmd, int lo, int hi, int &out) {
  String v = line.substring(strlen(cmd));
  v.trim();
  if (!v.length()) return false;
  int n = v.toInt();
  if (n < lo || n > hi) {
    Serial.printf("[cfg] value must be %d .. %d\n", lo, hi);
    return false;
  }
  out = n;
  return true;
}

void handleSerial() {
  static String line = "";
  while (Serial.available()) {
    char c = Serial.read();
    if (c != '\n' && c != '\r') { line += c; continue; }

    line.trim();
    line.toLowerCase();
    if (!line.length()) { line = ""; continue; }

    float f; int n;
    if      (line == "help" || line == "?")   printHelp();
    else if (line == "status")                printAudioStatus();
    else if (line == "calibrate")             runCalibration();
    else if (line == "test")                  runDistanceTest();
    else if (line == "meter") {
      cfg.meterOn = !cfg.meterOn;
      Serial.printf("[cfg] live meter %s\n", cfg.meterOn ? "ON" : "off");
    }
    else if (line.startsWith("gain") && argFloat(line, "gain", 0.25f, 8.0f, f)) {
      cfg.micGain = f;
      Serial.printf("[cfg] micGain = %.2f  (VAD rails auto-scaled: %.0f..%.0f)\n",
                    cfg.micGain, gateMinNow(), gateMaxNow());
    }
    else if (line.startsWith("wakegain") && argFloat(line, "wakegain", 1.0f, 20.0f, f)) {
      cfg.wakeGain = f;
      mic.setGain(cfg.wakeGain);
      Serial.printf("[cfg] wakeGain = %.2f\n", cfg.wakeGain);
    }
    else if (line.startsWith("noise") && argFloat(line, "noise", 1.2f, 4.0f, f)) {
      cfg.noiseMult = f;
      Serial.printf("[cfg] noiseMult = %.2f  -> VAD start %.0f\n", cfg.noiseMult,
                    constrain(calibratedNoiseRms*cfg.noiseMult, gateMinNow(), gateMaxNow()));
    }
    else if (line.startsWith("cont") && argFloat(line, "cont", 0.40f, 0.95f, f)) {
      cfg.contMult = f;  Serial.printf("[cfg] contMult = %.2f\n", cfg.contMult);
    }
    else if (line.startsWith("silence") && argInt(line, "silence", 300, 3000, n)) {
      cfg.endSilenceMs = n;  Serial.printf("[cfg] endSilenceMs = %d\n", n);
    }
    else if (line.startsWith("minspeech") && argInt(line, "minspeech", 40, 400, n)) {
      cfg.minSpeechMs = n;   Serial.printf("[cfg] minSpeechMs = %d\n", n);
    }
    else if (line.startsWith("wait") && argInt(line, "wait", 2000, 30000, n)) {
      cfg.wakeWaitMs = n;    Serial.printf("[cfg] wakeWaitMs = %d\n", n);
    }
    else if (line.startsWith("caltime") && argInt(line, "caltime", 200, 2000, n)) {
      cfg.calibrationMs = n; Serial.printf("[cfg] calibrationMs = %d\n", n);
    }
    else if (line.startsWith("knee") && argFloat(line, "knee", 0.5f, 1.0f, f)) {
      cfg.limitKnee = f;     Serial.printf("[cfg] limitKnee = %.2f (%.0f)\n", f, limitKneeNow());
    }
    else if (line == "save")     saveConfig();
    else if (line == "load")   { loadConfig(); Serial.println("[cfg] reloaded from NVS"); printAudioStatus(); }
    else if (line == "defaults") {
      cfg = AudioConfig{2.5f,10.0f,1.9f,0.70f,150.0f,700.0f,90,800,10000,500,0.73f,false,false};
      Serial.println("[cfg] defaults restored (type `save` to keep them)");
    }
    else if (line == "norm") {
      cfg.normOn = !cfg.normOn;
      Serial.printf("[cfg] clip normalisation %s\n", cfg.normOn ? "ON" : "off");
    }
    else if (line == "oled") {
      uiEnabled = !uiEnabled;
      Serial.printf("[cfg] OLED animation %s\n", uiEnabled ? "ON" : "off");
    }
    else if (line == "reset") {
      wakeCount = 0; mic.resetStats();
      Serial.println("[cfg] counters cleared");
    }
    else Serial.printf("[cfg] unknown command '%s' -- type `help`\n", line.c_str());

    line = "";
  }
}

// ================= SETUP =================
void setup() {
  Serial.begin(115200);
  delay(300);
  Serial.printf("[boot] Luna v9  heap=%u core=%d\n",
                ESP.getFreeHeap(), xPortGetCoreID());

  wavBuffer = (uint8_t*)ps_malloc(MAX_WAV_SIZE);
  if (!wavBuffer) wavBuffer = (uint8_t*)malloc(MAX_WAV_SIZE);
  if (!wavBuffer) { Serial.println("FATAL: audio buffer alloc failed"); while (1) delay(1000); }
  pcm = (int16_t*)(wavBuffer + 44);

  // Playback buffer: the whole TTS clip lands here before a single sample
  // reaches the speaker, so network jitter cannot cause I2S underruns.
  ttsRing = (uint8_t*)ps_malloc(TTS_RING_SIZE);
  if (!ttsRing) { Serial.println("FATAL: tts ring alloc failed"); while (1) delay(1000); }

  // Circular pre-speech buffer. Holds the last PREROLL_MS while we wait for
  // you to start, so the first consonant survives.
  preroll = (int16_t*)ps_malloc(PREROLL_SAMPLES * sizeof(int16_t));
  if (!preroll) { Serial.println("FATAL: preroll alloc failed"); while (1) delay(1000); }

  Serial.printf("[boot] PSRAM total=%u free=%u  rec=%ds ring=%dKB pre=%dms path=%s\n",
                ESP.getPsramSize(), ESP.getFreePsram(),
                MAX_RECORD_SECONDS, TTS_RING_SIZE / 1024, PREROLL_MS,
                USE_CONVERSE ? "converse" : "legacy");

  pinMode(BUTTON_PIN, INPUT_PULLUP);
  randomSeed(esp_random());

  // Restore any tuning previously stored with `save`.
  loadConfig();
  Serial.printf("[cfg] micGain=%.2f wakeGain=%.2f noiseMult=%.2f endSil=%dms\n",
                cfg.micGain, cfg.wakeGain, cfg.noiseMult, cfg.endSilenceMs);

  Wire.begin(OLED_SDA, OLED_SCL);
  display.begin(0x3C, true);
  // A 128x64 frame is ~1 KB. At 100 kHz that is ~88 ms of blocking I2C per
  // display() call, on the same core as esp-sr's Feed Task. 400 kHz cuts
  // that to ~22 ms. The SH1106 handles 400 kHz fine.
  Wire.setClock(400000);
  display.clearDisplay();
  display.setTextSize(1);
  display.setTextColor(SH110X_WHITE);
  display.setCursor(0, 0);
  display.print("Connecting...");
  display.display();

  WiFi.begin(ssid, password);
  while (WiFi.status() != WL_CONNECTED) delay(500);
  // Modem sleep wakes the radio in DTIM bursts that preempt the SR Feed
  // Task (Wi-Fi runs at priority ~23 on Core 0, the feed task at 5).
  // Disabling it trades a little power for a steadier audio feed.
  WiFi.setSleep(false);
  Serial.printf("[wifi] connected  heap=%u\n", ESP.getFreeHeap());

  if (!audioInit()) {
    display.clearDisplay(); display.setCursor(0, 0);
    display.print("Audio init failed"); display.display();
    while (1) delay(1000);
  }

  // Calibrate on the REC profile, because that is the scale the VAD uses.
  display.clearDisplay(); display.setCursor(0, 0);
  display.print("Calibrating mic..."); display.display();
  mic.setGain(cfg.micGain);
  calibrateMicNoise();
  mic.setGain(cfg.wakeGain);

  // Wake word last: it owns the mic feed from here on.
  display.clearDisplay(); display.setCursor(0, 0);
  display.print("Loading wake word..."); display.display();
  ESP_SR.onEvent(onSrEvent);
  srRunning = ESP_SR.begin(mic, sr_commands,
                           sizeof(sr_commands) / sizeof(sr_cmd_t),
                           SR_INPUT_CHANNELS, SR_MODE_WAKEWORD, SR_INPUT_FORMAT);
  if (!srRunning) {
    Serial.println("[err] ESP_SR.begin() failed -> Partition Scheme must be 'ESP SR 16M'");
    display.clearDisplay(); display.setCursor(0, 0);
    display.print("Wake word FAILED");
    display.setCursor(0, 12);
    display.print("Button still works");
    display.display();
    delay(2500);
  } else {
    Serial.printf("[boot] wake word ready  heap=%u psram=%u\n",
                  ESP.getFreeHeap(), ESP.getFreePsram());
  }

  display.clearDisplay(); display.setCursor(0, 0);
  display.print(srRunning ? "Say: Hi ESP" : "Ready (button)");
  display.display();
  delay(700);

  displayMode = MODE_EYES;
  xTaskCreatePinnedToCore(displayTask, "DisplayTask", 4096, NULL, 1, NULL, 0);
  Serial.println("[boot] running. Say \"Hi ESP\" or hold the button.");
}

// ================= LOOP =================
void loop() {
  handleSerial();

  // Live meter for distance testing (`meter`). Shows what the mic is
  // hearing right now, in the same units the VAD uses.
  if (cfg.meterOn) {
    static unsigned long nextMeter = 0;
    if (millis() > nextMeter) {
      nextMeter = millis() + 300;
      float g = constrain(calibratedNoiseRms * cfg.noiseMult, gateMinNow(), gateMaxNow());
      const char *state = (mic.liveRms > g) ? "SPEECH" : "  --  ";
      int bars = (int)(24.0f * mic.liveRms / 8000.0f);
      if (bars > 24) bars = 24;
      if (bars < 0)  bars = 0;
      Serial.printf("rms=%6.0f peak=%6.0f gate=%5.0f %s [",
                    mic.liveRms, mic.livePeak, g, state);
      for (int i = 0; i < 24; i++) Serial.print(i < bars ? '#' : '.');
      Serial.printf("] clip=%lu\n", (unsigned long)mic.clipCount);
    }
  }

  // Wake word trigger
  if (wakeFired) {
    wakeFired = false;
    handleConversation(true);
    wakeFired = false;               // drop anything queued during the turn
    return;
  }

  // Button trigger (always available, even if the wake word failed to load)
  if (digitalRead(BUTTON_PIN) == LOW) {
    delay(50);
    handleConversation(false);
    wakeFired = false;
    delay(300);
    return;
  }

  delay(10);
}
