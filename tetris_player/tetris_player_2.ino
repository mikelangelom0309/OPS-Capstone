// ============================================================
//  MULTIPLAYER TETRIS — Player Board Firmware
//  Hardware: Arduino Nano + SSD1306 128x64 OLED + NRF24L01+
//            + Analog Joystick + 2 tactile buttons
//
//  Libraries (Tools > Manage Libraries):
//    ► RF24              by TMRh20
//    ► Adafruit SSD1306  by Adafruit
//    ► Adafruit GFX      by Adafruit
//
//  !! BEFORE UPLOADING: set PLAYER_ID below to 1 or 2 !!
// ============================================================

#include <SPI.h>
#include <Wire.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>
#include <RF24.h>
#include "tetris_shared.h"

// ═══════════════════════════════════════════════════════════════
//  CHANGE THIS BEFORE UPLOADING
// ═══════════════════════════════════════════════════════════════
#define PLAYER_ID   2     // 1 for Player 1 board, 2 for Player 2

// ═══════════════════════════════════════════════════════════════
//  Pin definitions
// ═══════════════════════════════════════════════════════════════
#define PIN_CE      9
#define PIN_CSN     10
#define PIN_JOY_X   A0    // joystick left/right axis
#define PIN_JOY_Y   A1    // joystick up/down axis
#define PIN_JOY_BTN 2     // joystick push (hard drop)
#define PIN_RST_BTN 3     // separate reset button

// ═══════════════════════════════════════════════════════════════
//  Display & board constants
// ═══════════════════════════════════════════════════════════════
#define SCR_W       128
#define SCR_H       64
#define BOARD_W     10    // columns
#define BOARD_H     20    // rows
#define CELL_PX     3     // pixels per cell (3x3)

// ── Computed board pixel bounds (do not change) ───────────────
// Board occupies x=0..31 (BOARD_W*CELL_PX+2=32), y=0..61 (BOARD_H*CELL_PX+2=62)
// UI area starts at x=33
#define UI_X        33

// ═══════════════════════════════════════════════════════════════
//  Joystick thresholds (0–1023 range, 512 = center)
// ═══════════════════════════════════════════════════════════════
#define JOY_LO      380   // below = left / up
#define JOY_HI      640   // above = right / down

// ═══════════════════════════════════════════════════════════════
//  Timing (milliseconds)
// ═══════════════════════════════════════════════════════════════
#define T_SEND      100   // state transmit interval
#define T_MOVE      170   // lateral auto-repeat
#define T_SOFT      75    // soft-drop repeat
#define T_BTN_DB    240   // button debounce
#define LINES_PER_LV 10   // lines needed to advance one level

// ═══════════════════════════════════════════════════════════════
//  Tetromino shape data — stored in PROGMEM (flash, not RAM)
//
//  Each uint16_t encodes a 4×4 bitmask:
//    Row 0 = bits[15:12]   Row 3 = bits[3:0]
//    Within each row, leftmost column = highest bit.
//
//  Piece order: I, O, T, S, Z, J, L
//  Each has 4 rotation states.
// ═══════════════════════════════════════════════════════════════
const uint16_t SHAPES[7][4] PROGMEM = {
  { 0x0F00, 0x4444, 0x0F00, 0x4444 },   // I  — cyan
  { 0x6600, 0x6600, 0x6600, 0x6600 },   // O  — yellow (no rotation)
  { 0x04E0, 0x8C80, 0x0E40, 0x4C40 },   // T  — purple
  { 0x06C0, 0x8C40, 0x06C0, 0x8C40 },   // S  — green
  { 0x0C60, 0x4C80, 0x0C60, 0x4C80 },   // Z  — red
  { 0x08E0, 0xC880, 0x0E20, 0x44C0 },   // J  — blue
  { 0x02E0, 0x88C0, 0x0E80, 0xC440 },   // L  — orange
};

// ═══════════════════════════════════════════════════════════════
//  Hardware objects
// ═══════════════════════════════════════════════════════════════
Adafruit_SSD1306 oled(SCR_W, SCR_H, &Wire, -1);
RF24             radio(PIN_CE, PIN_CSN);

// ═══════════════════════════════════════════════════════════════
//  Game state globals
// ═══════════════════════════════════════════════════════════════
uint16_t board[BOARD_H];      // bit 0 = column 0 (leftmost)
uint8_t  gs;                  // current game state (GS_*)

int8_t   px, py;              // active piece board position
uint8_t  pt, pr;              // piece type (0-6), rotation (0-3)
uint8_t  nextPt;              // next piece type

uint32_t score32;
uint8_t  level;
uint8_t  linesTotal;
uint16_t gameSeed;

// ── Timing bookmarks ─────────────────────────────────────────
uint32_t tDrop, tSend;
uint32_t tLeft, tRight, tSoft;   // auto-repeat timers (0 = idle)
uint32_t tJoyBtn, tRstBtn;        // debounce timestamps

// ── Previous input states for edge detection ─────────────────
bool prevJoyUp  = false;
bool prevJoyBtn = true;   // HIGH = released (INPUT_PULLUP)
bool prevRstBtn = true;
bool gameOverDrawn = false;
uint32_t tDiag = 0;

void enterWaiting();
void initGame();

const __FlashStringHelper* stateName(uint8_t state) {
  if (state == GS_WAITING) return F("WAITING");
  if (state == GS_PLAYING) return F("PLAYING");
  return F("OVER");
}

void printAddr(const byte* addr) {
  for (uint8_t i = 0; i < 5; i++) Serial.write(addr[i]);
}

void printRadioSummary() {
  Serial.println(F("--- PLAYER RADIO ---"));
  Serial.print(F("PLAYER_ID: "));
  Serial.println(PLAYER_ID);
  Serial.print(F("Chip connected: "));
  Serial.println(radio.isChipConnected() ? F("YES") : F("NO"));
  Serial.print(F("TX pipe: "));
  if (PLAYER_ID == 1) printAddr(ADDR_P1);
  else                printAddr(ADDR_P2);
  Serial.println();
  Serial.print(F("RX pipe: "));
  if (PLAYER_ID == 1) printAddr(ADDR_C1);
  else                printAddr(ADDR_C2);
  Serial.println();
  radio.printDetails();
  Serial.println(F("--------------------"));
}

void logTxResult(bool ok) {
  Serial.print(F("TX "));
  Serial.print(ok ? F("OK") : F("FAIL"));
  Serial.print(F(" pid="));
  Serial.print(PLAYER_ID);
  Serial.print(F(" state="));
  Serial.print(stateName(gs));
  Serial.print(F(" score="));
  Serial.println((uint16_t)min(score32, 65535UL));
}

// ═══════════════════════════════════════════════════════════════
//  Piece helper functions
// ═══════════════════════════════════════════════════════════════

// Retrieve piece shape from PROGMEM
inline uint16_t getShape(uint8_t type, uint8_t rot) {
  return pgm_read_word(&SHAPES[type][rot]);
}

// Test whether cell (r, c) in a 4×4 shape is filled
inline bool cellAt(uint16_t shp, uint8_t r, uint8_t c) {
  return (shp >> (15 - (r * 4 + c))) & 1;
}

// Check if piece at (x, y) with given type/rotation is legal
bool isValid(int8_t x, int8_t y, uint8_t type, uint8_t rot) {
  uint16_t shp = getShape(type, rot);
  for (uint8_t r = 0; r < 4; r++) {
    for (uint8_t c = 0; c < 4; c++) {
      if (!cellAt(shp, r, c)) continue;
      int8_t bx = x + c;
      int8_t by = y + r;
      if (bx < 0 || bx >= BOARD_W) return false;   // out of bounds left/right
      if (by >= BOARD_H)            return false;   // out of bounds below
      if (by < 0)                   continue;       // above board top = OK
      if ((board[by] >> bx) & 1)   return false;   // collision with locked cell
    }
  }
  return true;
}

// Attempt rotation with wall-kick offsets
void tryRotate() {
  uint8_t nr = (pr + 1) % 4;
  const int8_t kicks[5][2] = {{0,0},{1,0},{-1,0},{2,0},{-2,0}};
  for (uint8_t i = 0; i < 5; i++) {
    int8_t nx = px + kicks[i][0];
    int8_t ny = py + kicks[i][1];
    if (isValid(nx, ny, pt, nr)) {
      px = nx; py = ny; pr = nr;
      return;
    }
  }
  // All kicks failed — rotation not applied
}

// Stamp active piece into the board array
void lockPiece() {
  uint16_t shp = getShape(pt, pr);
  for (uint8_t r = 0; r < 4; r++) {
    for (uint8_t c = 0; c < 4; c++) {
      if (!cellAt(shp, r, c)) continue;
      int8_t bx = px + c;
      int8_t by = py + r;
      if (by >= 0 && by < BOARD_H && bx >= 0 && bx < BOARD_W)
        board[by] |= (1u << bx);
    }
  }
}

// Scan board bottom-up, remove full rows, shift everything down.
// Returns the number of lines cleared.
uint8_t clearLines() {
  uint8_t n = 0;
  for (int8_t r = BOARD_H - 1; r >= 0; r--) {
    if (board[r] == 0x03FFu) {          // 10 bits set = full row
      for (int8_t i = r; i > 0; i--)
        board[i] = board[i - 1];
      board[0] = 0;
      n++;
      r++;    // re-check same index after row collapsed
    }
  }
  return n;
}

// Spawn the next piece; declare game over if it cannot be placed
void spawnPiece() {
  pt     = nextPt;
  pr     = 0;
  px     = 3;         // horizontally centered in the 10-wide board
  py     = 0;
  nextPt = random(7);
  if (!isValid(px, py, pt, pr))
    gs = GS_OVER;
}

// Drop interval in ms — speeds up from 800 ms at level 1 to ~80 ms at level 10+
uint16_t gravityMs() {
  return (uint16_t)max(80, 800 - (level - 1) * 80);
}

// Lock the active piece, clear lines, update score/level, spawn next piece
void lockAndSpawn() {
  lockPiece();
  uint8_t n = clearLines();
  if (n) {
    // Standard Tetris line-clear scoring
    const uint16_t pts[5] = {0, 100, 300, 500, 800};
    score32    += (uint32_t)pts[n] * level;
    linesTotal += n;
    level       = (uint8_t)(linesTotal / LINES_PER_LV) + 1;
    if (level > 20) level = 20;
  }
  spawnPiece();
  tDrop = millis();
}

// ═══════════════════════════════════════════════════════════════
//  Rendering
// ═══════════════════════════════════════════════════════════════

// Fill a 3×3 block for board cell at (col, row)
inline void drawCell(uint8_t col, uint8_t row) {
  oled.fillRect(1 + col * CELL_PX, 1 + row * CELL_PX, CELL_PX, CELL_PX, WHITE);
}

// Draw all filled cells of a piece at board position (x, y)
void drawPiece(int8_t x, int8_t y, uint8_t type, uint8_t rot) {
  uint16_t shp = getShape(type, rot);
  for (uint8_t r = 0; r < 4; r++) {
    for (uint8_t c = 0; c < 4; c++) {
      if (!cellAt(shp, r, c)) continue;
      int8_t bx = x + c, by = y + r;
      if (bx >= 0 && bx < BOARD_W && by >= 0 && by < BOARD_H)
        drawCell(bx, by);
    }
  }
}

// Draw a ghost (shadow) piece showing where the active piece will land
void drawGhost() {
  int8_t gy = py;
  while (isValid(px, gy + 1, pt, pr)) gy++;
  if (gy == py) return;           // piece already on the floor

  uint16_t shp = getShape(pt, pr);
  for (uint8_t r = 0; r < 4; r++) {
    for (uint8_t c = 0; c < 4; c++) {
      if (!cellAt(shp, r, c)) continue;
      int8_t bx = px + c, by = gy + r;
      if (bx >= 0 && bx < BOARD_W && by >= 0 && by < BOARD_H) {
        // Four corner pixels only — hollow "ghost" look
        int px2 = 1 + bx * CELL_PX;
        int py2 = 1 + by * CELL_PX;
        oled.drawPixel(px2,     py2,     WHITE);
        oled.drawPixel(px2 + 2, py2,     WHITE);
        oled.drawPixel(px2,     py2 + 2, WHITE);
        oled.drawPixel(px2 + 2, py2 + 2, WHITE);
      }
    }
  }
}

// 2-pixel-per-cell next-piece preview in the UI panel
void drawNextPreview() {
  uint16_t shp = getShape(nextPt, 0);
  int ox = 94, oy = 47;
  for (uint8_t r = 0; r < 4; r++)
    for (uint8_t c = 0; c < 4; c++)
      if (cellAt(shp, r, c))
        oled.fillRect(ox + c * 2, oy + r * 2, 2, 2, WHITE);
}

// Full game frame render
void drawGame() {
  oled.clearDisplay();

  // Board border
  oled.drawRect(0, 0, BOARD_W * CELL_PX + 2, BOARD_H * CELL_PX + 2, WHITE);

  // Locked cells
  for (uint8_t r = 0; r < BOARD_H; r++)
    for (uint8_t c = 0; c < BOARD_W; c++)
      if ((board[r] >> c) & 1) drawCell(c, r);

  // Ghost then active piece (so active piece is drawn on top)
  drawGhost();
  drawPiece(px, py, pt, pr);

  // Vertical divider between board and UI
  oled.drawFastVLine(UI_X, 0, SCR_H, WHITE);

  // ── UI panel ─────────────────────────────────────────────────
  oled.setTextSize(1);
  oled.setTextColor(WHITE);

  // Player label
  oled.setCursor(UI_X + 3, 0);
  oled.print(PLAYER_ID == 1 ? F("P1") : F("P2"));

  // Score
  oled.setCursor(UI_X + 3, 11);
  oled.print(F("SC:"));
  oled.setCursor(UI_X + 3, 21);
  oled.print(min(score32, 99999UL));

  // Lines cleared
  oled.setCursor(UI_X + 3, 33);
  oled.print(F("LN:"));
  oled.setCursor(UI_X + 3, 43);
  oled.print(linesTotal);

  // Level (right side of UI)
  oled.setCursor(UI_X + 50, 33);
  oled.print(F("LV:"));
  oled.setCursor(UI_X + 50, 43);
  oled.print(level);

  // Next piece label + 2px preview
  oled.setCursor(UI_X + 3, 55);
  oled.print(F("NX:"));
  drawNextPreview();

  oled.display();
}

// Simple centered-text screen (used for waiting/game-over states)
void drawScreen(const __FlashStringHelper* l1,
                const __FlashStringHelper* l2 = nullptr,
                const __FlashStringHelper* l3 = nullptr) {
  oled.clearDisplay();
  oled.setTextSize(1);
  oled.setTextColor(WHITE);
  if (l1) { oled.setCursor(20, 12); oled.print(l1); }
  if (l2) { oled.setCursor(6,  28); oled.print(l2); }
  if (l3) { oled.setCursor(6,  44); oled.print(l3); }
  oled.display();
}

void drawGameOver() {
  oled.clearDisplay();
  oled.setTextSize(1);
  oled.setTextColor(WHITE);
  oled.setCursor(24, 6);  oled.print(F("GAME OVER"));
  oled.setCursor(6,  22); oled.print(F("Score: "));
  oled.print(min(score32, 99999UL));
  oled.setCursor(6,  36); oled.print(F("Lines: "));
  oled.print(linesTotal);
  oled.setCursor(6,  52); oled.print(F("RST btn to retry"));
  oled.display();
}

void drawWaitingScreen() {
  oled.clearDisplay();
  oled.setTextSize(1);
  oled.setTextColor(WHITE);
  oled.setCursor(20, 12);
  oled.print(PLAYER_ID == 1 ? F("PLAYER 1") : F("PLAYER 2"));
  oled.setCursor(6, 28);
  oled.print(F("Waiting for server"));
  oled.setCursor(18, 44);
  oled.print(F("Start on host"));
  oled.display();
}

// ═══════════════════════════════════════════════════════════════
//  Radio — send current state to server
// ═══════════════════════════════════════════════════════════════
bool sendState() {
  PlayerPkt pkt;
  pkt.pid       = PLAYER_ID;
  pkt.score     = (uint16_t)min(score32, 65535UL);
  pkt.level     = level;
  pkt.lines     = linesTotal;
  pkt.nextPiece = nextPt;
  pkt.state     = gs;
  pkt.reserved  = 0;

  // NRF is half-duplex: stop listening briefly to transmit
  radio.stopListening();
  bool ok = radio.write(&pkt, sizeof(pkt));
  radio.startListening();
  return ok;
}

void handleServerCommand(const ServerCmd& cmd) {
  Serial.print(F("CMD opcode="));
  Serial.print(cmd.opcode);
  Serial.print(F(" seed="));
  Serial.println(cmd.gameSeed);
  if (cmd.opcode == CMD_START) {
    gameSeed = cmd.gameSeed;
    randomSeed(gameSeed);
    initGame();
  } else if (cmd.opcode == CMD_RESET) {
    enterWaiting();
  }
}

void pollServerCommands() {
  while (radio.available()) {
    ServerCmd cmd;
    radio.read(&cmd, sizeof(cmd));
    handleServerCommand(cmd);
  }
}

// ═══════════════════════════════════════════════════════════════
//  Input handling
// ═══════════════════════════════════════════════════════════════
void handleInput() {
  uint32_t now = millis();
  int xv      = analogRead(PIN_JOY_X);
  int yv      = analogRead(PIN_JOY_Y);
  bool joyBtn = digitalRead(PIN_JOY_BTN);  // LOW = pressed (INPUT_PULLUP)
  bool rstBtn = digitalRead(PIN_RST_BTN);
  bool joyUp  = (yv < JOY_LO);

  // ── Move LEFT (auto-repeat while held) ─────────────────────
  if (xv < JOY_LO) {
    if (!tLeft || (now - tLeft >= T_MOVE)) {
      if (isValid(px - 1, py, pt, pr)) px--;
      tLeft = now;
    }
  } else { tLeft = 0; }

  // ── Move RIGHT ──────────────────────────────────────────────
  if (xv > JOY_HI) {
    if (!tRight || (now - tRight >= T_MOVE)) {
      if (isValid(px + 1, py, pt, pr)) px++;
      tRight = now;
    }
  } else { tRight = 0; }

  // ── Soft DROP (push joystick down) ─────────────────────────
  if (yv > JOY_HI) {
    if (!tSoft || (now - tSoft >= T_SOFT)) {
      if (isValid(px, py + 1, pt, pr)) {
        py++;
        score32++;     // 1 point per soft-drop row (standard scoring)
        tDrop = now;   // reset gravity timer so piece doesn't double-drop
      }
      tSoft = now;
    }
  } else { tSoft = 0; }

  // ── ROTATE (joystick up — fires once per push) ──────────────
  if (joyUp && !prevJoyUp) tryRotate();
  prevJoyUp = joyUp;

  // ── HARD DROP + lock (joystick button press) ────────────────
  if (!joyBtn && prevJoyBtn && (now - tJoyBtn >= T_BTN_DB)) {
    while (isValid(px, py + 1, pt, pr)) { py++; score32 += 2; }
    lockAndSpawn();
    tJoyBtn = now;
  }
  prevJoyBtn = joyBtn;

  // ── RESET (dedicated reset button) ─────────────────────────
  if (!rstBtn && prevRstBtn && (now - tRstBtn >= T_BTN_DB)) {
    enterWaiting();
    tRstBtn = now;
  }
  prevRstBtn = rstBtn;
}

// ═══════════════════════════════════════════════════════════════
//  Game initialisation
// ═══════════════════════════════════════════════════════════════
void initGame() {
  memset(board, 0, sizeof(board));
  score32    = 0;
  level      = 1;
  linesTotal = 0;
  nextPt     = (uint8_t)random(7);
  spawnPiece();
  gs    = GS_PLAYING;
  tDrop = millis();
  gameOverDrawn = false;
}

void enterWaiting() {
  gs         = GS_WAITING;
  score32    = 0;
  level      = 1;
  linesTotal = 0;
  nextPt     = 0;
  memset(board, 0, sizeof(board));
  tDrop      = millis();
  tSend      = 0;
  prevJoyUp  = false;
  gameOverDrawn = false;
  drawWaitingScreen();
}

// ═══════════════════════════════════════════════════════════════
//  Setup
// ═══════════════════════════════════════════════════════════════
void setup() {
  Serial.begin(9600);
  Serial.println();
  Serial.println(F("BOOTING P1 FILE"));

  // Input pins
  pinMode(PIN_JOY_BTN, INPUT_PULLUP);
  pinMode(PIN_RST_BTN, INPUT_PULLUP);
  prevJoyBtn = HIGH;
  prevRstBtn = HIGH;

  // ── OLED ─────────────────────────────────────────────────────
  if (!oled.begin(SSD1306_SWITCHCAPVCC, 0x3D)) {
    Serial.println(F("OLED not found — check wiring"));
    for (;;) {}
  }
  oled.clearDisplay();
  oled.display();

  // ── NRF24L01+ ────────────────────────────────────────────────
  if (!radio.begin()) {
    Serial.println(F("NRF24L01 not found — check wiring & 10uF cap"));
    for (;;) {}
  }
  radio.setPALevel(RF24_PA_LOW);        // increase to RF24_PA_HIGH for longer range
  radio.setDataRate(RF24_250KBPS);      // slowest = most reliable at short range
  radio.setChannel(RADIO_CHANNEL);
  radio.setAutoAck(false);              // no hardware ACK needed for this design
  radio.setPayloadSize(sizeof(PlayerPkt));

  // Each player writes to its own unique address
  if (PLAYER_ID == 1) radio.openWritingPipe(ADDR_P1);
  else                radio.openWritingPipe(ADDR_P2);

  // Each player also listens for commands from the server.
  if (PLAYER_ID == 1) radio.openReadingPipe(1, ADDR_C1);
  else                radio.openReadingPipe(1, ADDR_C2);

  // Not expecting any incoming packets on this board, but
  // startListening() is required — we stopListening() briefly to TX.
  radio.startListening();
  printRadioSummary();

  // Seed the RNG from two floating analog pins for unique piece sequences
  randomSeed((uint32_t)analogRead(A2) ^ ((uint32_t)analogRead(A3) << 10));

  // Show waiting screen
  enterWaiting();
  Serial.println(F("EXPECT pid=1 tx=PLYRA rx=CMD1A"));
}

// ═══════════════════════════════════════════════════════════════
//  Main loop
// ═══════════════════════════════════════════════════════════════
void loop() {
  uint32_t now = millis();
  pollServerCommands();

  // ── WAITING: server start button begins the match ───────────
  if (gs == GS_WAITING) {
    bool rst = digitalRead(PIN_RST_BTN);
    if (!rst && prevRstBtn && (now - tRstBtn >= T_BTN_DB)) {
      enterWaiting();
      tRstBtn = now;
    }
    prevRstBtn = rst;
    if (now - tSend >= T_SEND) {
      bool ok = sendState();
      if (now - tDiag >= 1000) {
        logTxResult(ok);
        tDiag = now;
      }
      tSend = now;
    }
    return;
  }

  // ── GAME OVER ───────────────────────────────────────────────
  if (gs == GS_OVER) {
    if (!gameOverDrawn) {
      drawGameOver();
      gameOverDrawn = true;
    }

    bool rst = digitalRead(PIN_RST_BTN);
    if (!rst && prevRstBtn && (now - tRstBtn >= T_BTN_DB)) {
      enterWaiting();
      tRstBtn = now;
    }
    prevRstBtn = rst;

    // Keep notifying server we are done
    if (now - tSend >= T_SEND) {
      bool ok = sendState();
      if (now - tDiag >= 1000) {
        logTxResult(ok);
        tDiag = now;
      }
      tSend = now;
    }
    return;
  }

  // ── PLAYING ─────────────────────────────────────────────────

  handleInput();

  // Gravity: drop one row every gravityMs()
  if (now - tDrop >= gravityMs()) {
    if (isValid(px, py + 1, pt, pr)) py++;
    else lockAndSpawn();
    tDrop = now;
  }

  drawGame();

  // Transmit state to server
  if (now - tSend >= T_SEND) {
    bool ok = sendState();
    if (now - tDiag >= 1000) {
      logTxResult(ok);
      tDiag = now;
    }
    tSend = now;
  }
}
