// Asteroids for Adafruit Feather RP2040 + 128x64 OLED FeatherWing (SH1107)
//
// Controls:
//   A     = rotate left
//   C     = rotate right
//   B     = thrust
//   BOOT  = fire
//   Any   = start / restart when on title or game over
//
// Libraries: Adafruit SH110x, Adafruit GFX, Adafruit NeoPixel, Adafruit BusIO
// Board: Adafruit Feather RP2040 (Earle Philhower core)

#include <Wire.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SH110X.h>
#include <Adafruit_NeoPixel.h>
#include <math.h>

#define BUTTON_A 9
#define BUTTON_B 8
#define BUTTON_C 7

#ifndef PIN_NEOPIXEL
#define PIN_NEOPIXEL 16
#endif

static const int SCREEN_W = 128;
static const int SCREEN_H = 64;
static const float PI2 = 6.2831853f;
static const float DEG = 0.0174533f;

Adafruit_SH1107 display = Adafruit_SH1107(64, 128, &Wire);
Adafruit_NeoPixel pixel(1, PIN_NEOPIXEL, NEO_GRB + NEO_KHZ800);

enum GameState : uint8_t { TITLE, PLAYING, DEAD };
GameState state = TITLE;

struct Ship {
  float x, y;
  float vx, vy;
  float angle;  // radians
  bool alive;
  unsigned long invulnUntil;
};

struct Bullet {
  float x, y;
  float vx, vy;
  bool active;
  unsigned long expires;
};

struct Rock {
  float x, y;
  float vx, vy;
  float angle;
  float spin;
  float shapePhase;  // fixed per rock so silhouette doesn't morph while spinning
  uint8_t size;  // 3=large, 2=med, 1=small
  bool active;
};

static const uint8_t MAX_BULLETS = 6;
static const uint8_t MAX_ROCKS = 16;

Ship ship;
Bullet bullets[MAX_BULLETS];
Rock rocks[MAX_ROCKS];

uint16_t score = 0;
uint16_t highScore = 0;
uint8_t lives = 3;
uint8_t wave = 1;
bool godMode = false;  // default off; toggle with BOOT on title

unsigned long lastFrame = 0;
unsigned long lastFire = 0;
unsigned long deadAt = 0;
unsigned long lastPixel = 0;
uint16_t hue = 0;

bool aDown = false, bDown = false, cDown = false, bootDown = false;
unsigned long inputLockUntil = 0;

float wrapX(float x) {
  if (x < 0) x += SCREEN_W;
  if (x >= SCREEN_W) x -= SCREEN_W;
  return x;
}

float wrapY(float y) {
  if (y < 0) y += SCREEN_H;
  if (y >= SCREEN_H) y -= SCREEN_H;
  return y;
}

float rockRadius(uint8_t size) {
  if (size == 3) return 10.0f;
  if (size == 2) return 6.0f;
  return 3.5f;
}

int rockPoints(uint8_t size) {
  if (size == 3) return 20;
  if (size == 2) return 50;
  return 100;
}

void setNeo(uint8_t r, uint8_t g, uint8_t b) {
  pixel.setPixelColor(0, pixel.Color(r, g, b));
  pixel.show();
}

void resetBullets() {
  for (uint8_t i = 0; i < MAX_BULLETS; i++) bullets[i].active = false;
}

void clearRocks() {
  for (uint8_t i = 0; i < MAX_ROCKS; i++) rocks[i].active = false;
}

int8_t allocRock() {
  for (uint8_t i = 0; i < MAX_ROCKS; i++) {
    if (!rocks[i].active) return (int8_t)i;
  }
  return -1;
}

void spawnRock(float x, float y, uint8_t size) {
  int8_t i = allocRock();
  if (i < 0) return;
  rocks[i].active = true;
  rocks[i].x = x;
  rocks[i].y = y;
  rocks[i].size = size;
  rocks[i].angle = (rp2040.hwrand32() % 360) * DEG;
  rocks[i].spin = ((int)(rp2040.hwrand32() % 9) - 4) * 0.025f;
  rocks[i].shapePhase = (rp2040.hwrand32() % 628) / 100.0f;

  float speed = 0.35f + (3 - size) * 0.25f + wave * 0.03f;
  float a = (rp2040.hwrand32() % 360) * DEG;
  rocks[i].vx = cosf(a) * speed;
  rocks[i].vy = sinf(a) * speed;
}

void spawnWave() {
  clearRocks();
  uint8_t count = min(4 + wave, 8);
  for (uint8_t n = 0; n < count; n++) {
    float x, y;
    // Keep starting rocks away from ship center
    do {
      x = rp2040.hwrand32() % SCREEN_W;
      y = rp2040.hwrand32() % SCREEN_H;
    } while (hypotf(x - SCREEN_W * 0.5f, y - SCREEN_H * 0.5f) < 28.0f);
    spawnRock(x, y, 3);
  }
}

void respawnShip() {
  ship.x = SCREEN_W * 0.5f;
  ship.y = SCREEN_H * 0.5f;
  ship.vx = 0;
  ship.vy = 0;
  ship.angle = -PI2 * 0.25f;  // nose up
  ship.alive = true;
  ship.invulnUntil = millis() + 2000;
  resetBullets();
}

void startGame() {
  score = 0;
  lives = 3;
  wave = 1;
  state = PLAYING;
  respawnShip();
  spawnWave();
}

uint8_t activeRocks() {
  uint8_t n = 0;
  for (uint8_t i = 0; i < MAX_ROCKS; i++) if (rocks[i].active) n++;
  return n;
}

void fireBullet() {
  if (!ship.alive) return;
  if (millis() - lastFire < 180) return;

  for (uint8_t i = 0; i < MAX_BULLETS; i++) {
    if (bullets[i].active) continue;
    float nose = 6.0f;
    bullets[i].active = true;
    bullets[i].x = ship.x + cosf(ship.angle) * nose;
    bullets[i].y = ship.y + sinf(ship.angle) * nose;
    float spd = 2.8f;
    bullets[i].vx = ship.vx + cosf(ship.angle) * spd;
    bullets[i].vy = ship.vy + sinf(ship.angle) * spd;
    bullets[i].expires = millis() + 900;
    lastFire = millis();
    return;
  }
}

void splitRock(uint8_t idx) {
  uint8_t size = rocks[idx].size;
  float x = rocks[idx].x;
  float y = rocks[idx].y;
  rocks[idx].active = false;
  score += rockPoints(size);
  if (score > highScore) highScore = score;
  if (size > 1) {
    spawnRock(x, y, size - 1);
    spawnRock(x, y, size - 1);
  }
}

bool shipHitRock(const Rock &r) {
  float rad = rockRadius(r.size) + 2.5f;
  float dx = ship.x - r.x;
  float dy = ship.y - r.y;
  // also check wrapped distances for edge cases near borders
  if (dx > SCREEN_W * 0.5f) dx -= SCREEN_W;
  if (dx < -SCREEN_W * 0.5f) dx += SCREEN_W;
  if (dy > SCREEN_H * 0.5f) dy -= SCREEN_H;
  if (dy < -SCREEN_H * 0.5f) dy += SCREEN_H;
  return (dx * dx + dy * dy) < (rad * rad);
}

bool bulletHitRock(const Bullet &b, const Rock &r) {
  float rad = rockRadius(r.size);
  float dx = b.x - r.x;
  float dy = b.y - r.y;
  return (dx * dx + dy * dy) < (rad * rad);
}

void killShip() {
  if (!ship.alive) return;
  if (millis() < ship.invulnUntil) return;
  ship.alive = false;
  lives--;
  deadAt = millis();
  if (lives == 0) {
    state = DEAD;
  }
}

void handleInput(float dt) {
  bool a = digitalRead(BUTTON_A) == LOW;
  bool b = digitalRead(BUTTON_B) == LOW;
  bool c = digitalRead(BUTTON_C) == LOW;
  bool boot = BOOTSEL;

  // Ignore start edges until lockout ends (avoids BOOTSEL/false edges skipping the title)
  bool acceptStart = millis() >= inputLockUntil;

  if (state == TITLE && acceptStart) {
    if (boot && !bootDown) {
      godMode = !godMode;
    } else if ((a && !aDown) || (b && !bDown) || (c && !cDown)) {
      startGame();
    }
  } else if (state == DEAD && acceptStart) {
    if ((a && !aDown) || (b && !bDown) || (c && !cDown)) {
      startGame();
    }
  } else if (state == PLAYING && ship.alive) {
    if (a) ship.angle -= 3.6f * DEG * (dt * 60.0f);
    if (c) ship.angle += 3.6f * DEG * (dt * 60.0f);
    if (b) {
      float thrust = 0.12f * (dt * 60.0f);
      ship.vx += cosf(ship.angle) * thrust;
      ship.vy += sinf(ship.angle) * thrust;
      // soft speed cap
      float spd = hypotf(ship.vx, ship.vy);
      if (spd > 2.2f) {
        ship.vx *= 2.2f / spd;
        ship.vy *= 2.2f / spd;
      }
    }
    if (boot && !bootDown) fireBullet();
  }

  aDown = a;
  bDown = b;
  cDown = c;
  bootDown = boot;
}

void updateGame(float dt) {
  if (state != PLAYING) return;

  // Respawn delay after death
  if (!ship.alive && lives > 0 && millis() - deadAt > 1200) {
    respawnShip();
  }

  if (ship.alive) {
    // drag
    ship.vx *= powf(0.985f, dt * 60.0f);
    ship.vy *= powf(0.985f, dt * 60.0f);
    ship.x = wrapX(ship.x + ship.vx * dt * 60.0f);
    ship.y = wrapY(ship.y + ship.vy * dt * 60.0f);
  }

  for (uint8_t i = 0; i < MAX_BULLETS; i++) {
    if (!bullets[i].active) continue;
    if (millis() > bullets[i].expires) {
      bullets[i].active = false;
      continue;
    }
    bullets[i].x = wrapX(bullets[i].x + bullets[i].vx * dt * 60.0f);
    bullets[i].y = wrapY(bullets[i].y + bullets[i].vy * dt * 60.0f);
  }

  for (uint8_t i = 0; i < MAX_ROCKS; i++) {
    if (!rocks[i].active) continue;
    rocks[i].x = wrapX(rocks[i].x + rocks[i].vx * dt * 60.0f);
    rocks[i].y = wrapY(rocks[i].y + rocks[i].vy * dt * 60.0f);
    rocks[i].angle += rocks[i].spin * dt * 60.0f;
  }

  // Bullets vs rocks
  for (uint8_t bi = 0; bi < MAX_BULLETS; bi++) {
    if (!bullets[bi].active) continue;
    for (uint8_t ri = 0; ri < MAX_ROCKS; ri++) {
      if (!rocks[ri].active) continue;
      if (bulletHitRock(bullets[bi], rocks[ri])) {
        bullets[bi].active = false;
        splitRock(ri);
        break;
      }
    }
  }

  // Ship vs rocks (skipped when god mode is on; default is off)
  if (!godMode && ship.alive) {
    for (uint8_t i = 0; i < MAX_ROCKS; i++) {
      if (rocks[i].active && shipHitRock(rocks[i])) {
        killShip();
        break;
      }
    }
  }

  if (activeRocks() == 0 && ship.alive) {
    wave++;
    spawnWave();
    ship.invulnUntil = millis() + 1500;
  }
}

void drawShipAt(float ox, float oy) {
  float a = ship.angle;
  float c = cosf(a), s = sinf(a);
  float sx = ship.x + ox;
  float sy = ship.y + oy;

  auto tx = [&](float lx, float ly) -> int16_t {
    return (int16_t)(sx + lx * c - ly * s + 0.5f);
  };
  auto ty = [&](float lx, float ly) -> int16_t {
    return (int16_t)(sy + lx * s + ly * c + 0.5f);
  };

  int16_t x0 = tx(5.5f, 0),     y0 = ty(5.5f, 0);
  int16_t x1 = tx(-4.0f, -3.5f), y1 = ty(-4.0f, -3.5f);
  int16_t x2 = tx(-4.0f,  3.5f), y2 = ty(-4.0f,  3.5f);
  display.drawLine(x0, y0, x1, y1, SH110X_WHITE);
  display.drawLine(x1, y1, x2, y2, SH110X_WHITE);
  display.drawLine(x2, y2, x0, y0, SH110X_WHITE);

  if (digitalRead(BUTTON_B) == LOW) {
    int16_t fx = tx(-6.5f, 0), fy = ty(-6.5f, 0);
    display.drawLine(x1, y1, fx, fy, SH110X_WHITE);
    display.drawLine(x2, y2, fx, fy, SH110X_WHITE);
  }
}

// Classic Asteroids trick: redraw near opposite edges so wrap is seamless.
void drawWrapped(float x, float y, float margin, void (*drawAt)(float, float)) {
  drawAt(0, 0);
  bool left = x < margin;
  bool right = x > SCREEN_W - margin;
  bool top = y < margin;
  bool bottom = y > SCREEN_H - margin;
  if (left) drawAt(SCREEN_W, 0);
  if (right) drawAt(-SCREEN_W, 0);
  if (top) drawAt(0, SCREEN_H);
  if (bottom) drawAt(0, -SCREEN_H);
  if (left && top) drawAt(SCREEN_W, SCREEN_H);
  if (right && top) drawAt(-SCREEN_W, SCREEN_H);
  if (left && bottom) drawAt(SCREEN_W, -SCREEN_H);
  if (right && bottom) drawAt(-SCREEN_W, -SCREEN_H);
}

void drawShip() {
  if (!ship.alive) return;
  if (millis() < ship.invulnUntil && ((millis() / 80) % 2)) return;
  drawWrapped(ship.x, ship.y, 8.0f, drawShipAt);
}

void drawRockAtOffset(const Rock &r, float ox, float oy) {
  float rad = rockRadius(r.size);
  const uint8_t verts = 8;
  float ca = cosf(r.angle), sa = sinf(r.angle);
  int16_t px[verts], py[verts];

  for (uint8_t i = 0; i < verts; i++) {
    // Fixed local silhouette, then rigid rotate — no morphing while spinning
    float local = (PI2 * i) / verts;
    float bump = 0.72f + 0.28f * cosf(local * 3.0f + r.shapePhase);
    float lx = cosf(local) * rad * bump;
    float ly = sinf(local) * rad * bump;
    px[i] = (int16_t)(r.x + ox + lx * ca - ly * sa + 0.5f);
    py[i] = (int16_t)(r.y + oy + lx * sa + ly * ca + 0.5f);
  }
  for (uint8_t i = 0; i < verts; i++) {
    uint8_t j = (i + 1) % verts;
    display.drawLine(px[i], py[i], px[j], py[j], SH110X_WHITE);
  }
}

// Need a current-rock pointer because drawWrapped takes a simple callback.
Rock *drawRockCurrent = nullptr;

void drawRockAt(float ox, float oy) {
  if (drawRockCurrent) drawRockAtOffset(*drawRockCurrent, ox, oy);
}

void drawRock(Rock &r) {
  drawRockCurrent = &r;
  drawWrapped(r.x, r.y, rockRadius(r.size) + 1.0f, drawRockAt);
  drawRockCurrent = nullptr;
}

void drawHUD() {
  display.setTextSize(1);
  display.setTextColor(SH110X_WHITE);
  display.setCursor(0, 0);
  display.printf("%u", score);
  display.setCursor(50, 0);
  display.printf("W%u", wave);
  if (godMode) {
    display.setCursor(74, 0);
    display.print(F("G"));
  }
  // Lives as tiny ships
  for (uint8_t i = 0; i < lives; i++) {
    int16_t x = 110 + i * 6;
    display.drawLine(x + 2, 1, x, 5, SH110X_WHITE);
    display.drawLine(x, 5, x + 4, 5, SH110X_WHITE);
    display.drawLine(x + 4, 5, x + 2, 1, SH110X_WHITE);
  }
}

void drawFrame() {
  display.clearDisplay();
  display.setTextColor(SH110X_WHITE);

  if (state == TITLE) {
    display.setTextSize(2);
    display.setCursor(8, 4);
    display.print(F("ASTEROIDS"));
    display.setTextSize(1);
    display.setCursor(8, 28);
    display.print(F("A/C rot  B thrust"));
    display.setCursor(8, 38);
    display.print(F("BOOT=fire / god"));
    display.setCursor(8, 48);
    display.printf("Hi %u  GOD:%s", highScore, godMode ? "ON" : "OFF");
    display.setCursor(8, 56);
    display.print(F("A/B/C to play"));
    display.display();
    return;
  }

  for (uint8_t i = 0; i < MAX_ROCKS; i++) {
    if (rocks[i].active) drawRock(rocks[i]);
  }
  for (uint8_t i = 0; i < MAX_BULLETS; i++) {
    if (!bullets[i].active) continue;
    float bx = bullets[i].x;
    float by = bullets[i].y;
    display.fillRect((int16_t)bx, (int16_t)by, 2, 2, SH110X_WHITE);
    // Soft wrap for bullets too (tiny, but avoids pop-in)
    if (bx < 2) display.fillRect((int16_t)(bx + SCREEN_W), (int16_t)by, 2, 2, SH110X_WHITE);
    if (bx > SCREEN_W - 2) display.fillRect((int16_t)(bx - SCREEN_W), (int16_t)by, 2, 2, SH110X_WHITE);
    if (by < 2) display.fillRect((int16_t)bx, (int16_t)(by + SCREEN_H), 2, 2, SH110X_WHITE);
    if (by > SCREEN_H - 2) display.fillRect((int16_t)bx, (int16_t)(by - SCREEN_H), 2, 2, SH110X_WHITE);
  }
  drawShip();
  drawHUD();

  if (state == DEAD) {
    display.fillRect(18, 20, 92, 28, SH110X_BLACK);
    display.drawRect(18, 20, 92, 28, SH110X_WHITE);
    display.setCursor(30, 26);
    display.print(F("GAME OVER"));
    display.setCursor(22, 38);
    display.printf("Score %u  btn", score);
  }

  display.display();
}

void updateNeoPixel() {
  unsigned long now = millis();
  if (now - lastPixel < 30) return;
  lastPixel = now;

  if (state == TITLE) {
    hue += 180;
    pixel.setPixelColor(0, pixel.gamma32(pixel.ColorHSV(hue, 255, 150)));
    pixel.show();
  } else if (state == DEAD) {
    setNeo(((now / 250) % 2) ? 180 : 30, 0, 0);
  } else if (!ship.alive) {
    setNeo(180, 40, 0);
  } else if (digitalRead(BUTTON_B) == LOW) {
    setNeo(40, 120, 255);  // thrust blue-white
  } else {
    setNeo(20, 180, 40);
  }
}

void setup() {
  Serial.begin(115200);

#if defined(NEOPIXEL_POWER)
  pinMode(NEOPIXEL_POWER, OUTPUT);
  digitalWrite(NEOPIXEL_POWER, HIGH);
#endif
  pixel.begin();
  pixel.setBrightness(45);

  pinMode(BUTTON_A, INPUT_PULLUP);
  pinMode(BUTTON_B, INPUT_PULLUP);
  pinMode(BUTTON_C, INPUT_PULLUP);

  delay(250);
  if (!display.begin(0x3C, true)) {
    display.begin(0x3D, true);
  }
  display.setRotation(1);
  display.clearDisplay();
  display.display();
  delay(50);

  // Treat current levels as already-down so boot/reset can't fake a press edge
  aDown = digitalRead(BUTTON_A) == LOW;
  bDown = digitalRead(BUTTON_B) == LOW;
  cDown = digitalRead(BUTTON_C) == LOW;
  bootDown = true;
  inputLockUntil = millis() + 800;

  state = TITLE;
  lastFrame = millis();
  drawFrame();
  Serial.println(F("Asteroids ready"));
}

void loop() {
  unsigned long now = millis();
  float dt = (now - lastFrame) / 1000.0f;
  if (dt > 0.05f) dt = 0.05f;  // clamp after stalls
  lastFrame = now;

  handleInput(dt);
  updateGame(dt);
  drawFrame();
  updateNeoPixel();
}
