// ============================================================
//  MULTIPLAYER TETRIS — Server / Host Board Firmware
//  Hardware: Arduino Nano + SSD1306 128x64 OLED + NRF24L01+
//
//  Libraries (Tools > Manage Libraries):
//    ► RF24              by TMRh20
//    ► Adafruit SSD1306  by Adafruit
//    ► Adafruit GFX      by Adafruit
//
//  This board has NO joystick. It receives state packets from
//  both player boards and displays scores, next pieces, and the
//  winner on its OLED.
//
//  Upload as-is — no configuration needed.
// ============================================================

#include <SPI.h>
#include <Wire.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>
#include <RF24.h>
#include "tetris_shared.h"

// ═══════════════════════════════════════════════════════════════
//  Pin definitions
// ═══════════════════════════════════════════════════════════════
#define PIN_CE      9
#define PIN_CSN     10

// ═══════════════════════════════════════════════════════════════
//  Display
// ═══════════════════════════════════════════════════════════════
#define SCR_W       128
#define SCR_H       64

// ── Two-panel layout ─────────────────────────────────────────
// P1 panel: x = 0 .. 61 (62 px wide)
// Divider:  x = 62
// P2 panel: x = 63 .. 127 (65 px wide)
#define DIVIDER_X   62
#define P2_X        63

// ═══════════════════════════════════════════════════════════════
//  Piece shape data (mirrors player firmware — same PROGMEM table)
//  Needed here only for drawing the mini next-piece preview.
// ═══════════════════════════════════════════════════════════════
const uint16_t SHAPES[7][4] PROGMEM = {
  { 0x0F00, 0x4444, 0x0F00, 0x4444 },   // I
  { 0x6600, 0x6600, 0x6600, 0x6600 },   // O
  { 0x04E0, 0x8C80, 0x0E40, 0x4C40 },   // T
  { 0x06C0, 0x8C40, 0x06C0, 0x8C40 },   // S
  { 0x0C60, 0x4C80, 0x0C60, 0x4C80 },   // Z
  { 0x08E0, 0xC880, 0x0E20, 0x44C0 },   // J
  { 0x02E0, 0x88C0, 0x0E80, 0xC440 },   // L
};

inline uint16_t getShape(uint8_t t) {
  return pgm_read_word(&SHAPES[t][0]);
}

inline bool cellAt(uint16_t s, uint8_t r, uint8_t c) {
  return (s >> (15 - (r * 4 + c))) & 1;
}

// ═══════════════════════════════════════════════════════════════
//  Hardware objects
// ═══════════════════════════════════════════════════════════════
Adafruit_SSD1306 oled(SCR_W, SCR_H, &Wire, -1);
RF24             radio(PIN_CE, PIN_CSN);

// ═══════════════════════════════════════════════════════════════
//  Server state
// ═══════════════════════════════════════════════════════════════
PlayerPkt p1 = {1, 0, 1, 0, 0, GS_WAITING};
PlayerPkt p2 = {2, 0, 1, 0, 0, GS_WAITING};

uint8_t  winnerPid     = 0;     // 0=no winner, 1=P1, 2=P2, 3=draw
uint32_t tLastDraw     = 0;
uint32_t tLastReceive  = 0;     // watchdog: track when we last heard from each player
uint32_t tP1Heard      = 0;
uint32_t tP2Heard      = 0;

// ═══════════════════════════════════════════════════════════════
//  Rendering helpers
// ═══════════════════════════════════════════════════════════════

// Draw a 2px-per-cell mini piece (4×4 = up to 8×8px rendered area)
// ox, oy = top-left pixel origin
void drawMiniPiece(uint8_t type, uint8_t ox, uint8_t oy) {
  uint16_t shp = getShape(type);
  for (uint8_t r = 0; r < 4; r++)
    for (uint8_t c = 0; c < 4; c++)
      if (cellAt(shp, r, c))
        oled.fillRect(ox + c * 2, oy + r * 2, 2, 2, WHITE);
}

// Draw one player's info panel.
// xOff = starting x pixel; panelW = panel width in pixels.
void drawPanel(const PlayerPkt& p, uint8_t xOff, uint8_t panelW) {
  oled.setTextSize(1);
  oled.setTextColor(WHITE);

  // ── Header line ──────────────────────────────────────────────
  oled.setCursor(xOff + 1, 0);
  oled.print(p.pid == 1 ? F("P1") : F("P2"));

  // Connection status — dim warning if no packet recently
  uint32_t& tHeard = (p.pid == 1) ? tP1Heard : tP2Heard;
  const char* statStr;
  if (p.state == GS_WAITING)       statStr = "WAIT";
  else if (p.state == GS_PLAYING)  statStr = "PLAY";
  else                              statStr = "DONE";

  // Right-align state string within the panel
  // Each char = 6px wide; 4-char string = 24px
  oled.setCursor(xOff + panelW - 24, 0);
  oled.print(statStr);

  // ── Score ────────────────────────────────────────────────────
  oled.setCursor(xOff + 1, 11);
  oled.print(F("SC:"));
  oled.setCursor(xOff + 1, 20);
  oled.print(p.score);

  // ── Lines ────────────────────────────────────────────────────
  oled.setCursor(xOff + 1, 30);
  oled.print(F("LN:"));
  oled.print(p.lines);

  // ── Level ────────────────────────────────────────────────────
  oled.setCursor(xOff + 1, 39);
  oled.print(F("LV:"));
  oled.print(p.level);

  // ── Next piece preview ───────────────────────────────────────
  oled.setCursor(xOff + 1, 49);
  oled.print(F("NX:"));
  // Mini piece drawn to the right of the "NX:" label
  drawMiniPiece(p.nextPiece, xOff + 21, 48);
}

// Winner announcement screen
void drawWinner() {
  oled.clearDisplay();
  oled.setTextSize(1);
  oled.setTextColor(WHITE);

  if (winnerPid == 3) {
    oled.setCursor(30, 4);
    oled.print(F("IT'S A DRAW!"));
  } else {
    oled.setCursor(18, 4);
    oled.print(F("PLAYER "));
    oled.print(winnerPid);
    oled.setCursor(34, 16);
    oled.print(F("WINS!"));
  }

  // Show final scores
  oled.setCursor(10, 32);
  oled.print(F("P1 score: "));
  oled.print(p1.score);

  oled.setCursor(10, 44);
  oled.print(F("P2 score: "));
  oled.print(p2.score);

  oled.setCursor(4, 56);
  oled.print(F("Rst both boards to replay"));

  oled.display();
}

// Two-panel live display
void drawLive() {
  oled.clearDisplay();

  drawPanel(p1, 0,        DIVIDER_X);
  oled.drawFastVLine(DIVIDER_X, 0, SCR_H, WHITE);
  drawPanel(p2, P2_X,     SCR_W - P2_X);

  oled.display();
}

// ═══════════════════════════════════════════════════════════════
//  Win detection
//  Called every time a new packet arrives.
// ═══════════════════════════════════════════════════════════════
void checkWin() {
  if (winnerPid != 0) return;    // already decided

  bool p1done = (p1.state == GS_OVER);
  bool p2done = (p2.state == GS_OVER);

  if (p1done && p2done)  winnerPid = 3;      // simultaneous top-out = draw
  else if (p1done)       winnerPid = 2;      // P1 topped out → P2 wins
  else if (p2done)       winnerPid = 1;      // P2 topped out → P1 wins
}

// ═══════════════════════════════════════════════════════════════
//  Setup
// ═══════════════════════════════════════════════════════════════
void setup() {
  Serial.begin(9600);

  // ── OLED ─────────────────────────────────────────────────────
  if (!oled.begin(SSD1306_SWITCHCAPVCC, 0x3C)) {
    Serial.println(F("OLED not found"));
    for (;;) {}
  }
  oled.clearDisplay();
  oled.setTextSize(1);
  oled.setTextColor(WHITE);
  oled.setCursor(16, 16); oled.print(F("SERVER READY"));
  oled.setCursor(4,  32); oled.print(F("Waiting for players"));
  oled.setCursor(4,  48); oled.print(F("Both boards must be on"));
  oled.display();

  // ── NRF24L01+ ────────────────────────────────────────────────
  if (!radio.begin()) {
    Serial.println(F("NRF24L01 not found"));
    for (;;) {}
  }
  radio.setPALevel(RF24_PA_LOW);
  radio.setDataRate(RF24_250KBPS);
  radio.setChannel(RADIO_CHANNEL);
  radio.setAutoAck(false);
  radio.setPayloadSize(sizeof(PlayerPkt));

  // Server listens on two dedicated pipes — one per player board
  radio.openReadingPipe(1, ADDR_P1);
  radio.openReadingPipe(2, ADDR_P2);
  radio.startListening();

  Serial.println(F("Server listening on pipes 1 (P1) and 2 (P2)"));
}

// ═══════════════════════════════════════════════════════════════
//  Main loop
// ═══════════════════════════════════════════════════════════════
void loop() {
  uint32_t now = millis();

  // ── Drain all pending NRF packets ───────────────────────────
  uint8_t pipe;
  while (radio.available(&pipe)) {
    PlayerPkt pkt;
    radio.read(&pkt, sizeof(pkt));

    if (pkt.pid == 1) {
      p1        = pkt;
      tP1Heard  = now;
      Serial.print(F("P1: sc=")); Serial.print(pkt.score);
      Serial.print(F(" lv="));    Serial.print(pkt.level);
      Serial.print(F(" st="));    Serial.println(pkt.state);
    } else if (pkt.pid == 2) {
      p2        = pkt;
      tP2Heard  = now;
      Serial.print(F("P2: sc=")); Serial.print(pkt.score);
      Serial.print(F(" lv="));    Serial.print(pkt.level);
      Serial.print(F(" st="));    Serial.println(pkt.state);
    }

    checkWin();
  }

  // ── Update display at ~10 fps ────────────────────────────────
  if (now - tLastDraw >= 100) {
    if (winnerPid != 0) drawWinner();
    else                drawLive();
    tLastDraw = now;
  }

  // ── Reset win state if both players restart ───────────────────
  // Once both players go back to WAITING, clear the winner so the
  // display returns to live mode for the next game.
  if (winnerPid != 0 &&
      p1.state == GS_WAITING &&
      p2.state == GS_WAITING) {
    winnerPid = 0;
    Serial.println(F("Both players reset — clearing winner"));
  }
}
