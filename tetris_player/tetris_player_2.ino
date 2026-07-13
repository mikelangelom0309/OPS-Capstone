// ============================================================
//  MULTIPLAYER TETRIS - Player Board Firmware
// ============================================================

#include <SPI.h>
#include <Wire.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>
#include <RF24.h>
#include "tetris_shared.h"

#define PLAYER_ID      2

#define PIN_CE         9
#define PIN_CSN        10
#define PIN_JOY_X      A0
#define PIN_JOY_Y      A1
#define PIN_JOY_BTN    2
#define PIN_RST_BTN    3
#define PIN_HOLD_BTN   4

#define OLED_W         128
#define OLED_H         64
#define SCR_W          64
#define SCR_H          128
#define BOARD_W        10
#define BOARD_H        20
#define CELL_PX        4
#define UI_X           44

#define JOY_LO         380
#define JOY_HI         640

#define T_SEND         100
#define T_MOVE         170
#define T_SOFT         75
#define T_BTN_DB       240
#define LINES_PER_LV   10

#define MATCH_NONE     0
#define MATCH_WIN_RES  1
#define MATCH_LOSE_RES 2
#define PIECE_NONE     255

const uint16_t SHAPES[7][4] PROGMEM = {
  { 0x0F00, 0x4444, 0x0F00, 0x4444 },
  { 0x6600, 0x6600, 0x6600, 0x6600 },
  { 0x04E0, 0x8C80, 0x0E40, 0x4C40 },
  { 0x06C0, 0x8C40, 0x06C0, 0x8C40 },
  { 0x0C60, 0x4C80, 0x0C60, 0x4C80 },
  { 0x08E0, 0xC880, 0x0E20, 0x44C0 },
  { 0x02E0, 0x88C0, 0x0E80, 0xC440 },
};

Adafruit_SSD1306 oled(OLED_W, OLED_H, &Wire, -1);
RF24 radio(PIN_CE, PIN_CSN);

uint16_t board[BOARD_H];
uint8_t gs;

int8_t px, py;
uint8_t pt, pr;
uint8_t nextPt;
uint8_t heldPt;

uint32_t score32;
uint8_t level;
uint8_t linesTotal;
uint16_t gameSeed;

uint32_t tDrop, tSend;
uint32_t tLeft, tRight, tSoft;
uint32_t tJoyBtn, tRstBtn, tHoldBtn;

bool prevJoyUp = false;
bool prevJoyBtn = true;
bool prevRstBtn = true;
bool prevHoldBtn = true;
bool holdUsed = false;
bool gameOverDrawn = false;

uint8_t pendingGarbage = 0;
uint8_t lastAttackTx = 0;
uint8_t matchResult = MATCH_NONE;
uint32_t tDiag = 0;

void enterWaiting();
void initGame();

const __FlashStringHelper* stateName(uint8_t state) {
  if (state == GS_WAITING) return F("WAITING");
  if (state == GS_PLAYING) return F("PLAYING");
  return F("OVER");
}

const __FlashStringHelper* resultName() {
  if (matchResult == MATCH_WIN_RES) return F("WIN");
  if (matchResult == MATCH_LOSE_RES) return F("LOSE");
  return F("NONE");
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
  printAddr(ADDR_P2);
  Serial.println();
  Serial.print(F("RX pipe: "));
  printAddr(ADDR_C2);
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
  Serial.print(F(" result="));
  Serial.print(resultName());
  Serial.print(F(" atk="));
  Serial.println(lastAttackTx);
}

inline uint16_t getShape(uint8_t type, uint8_t rot) {
  return pgm_read_word(&SHAPES[type][rot]);
}

inline bool cellAt(uint16_t shp, uint8_t row, uint8_t col) {
  return (shp >> (15 - (row * 4 + col))) & 1;
}

bool isValid(int8_t x, int8_t y, uint8_t type, uint8_t rot) {
  uint16_t shp = getShape(type, rot);
  for (uint8_t row = 0; row < 4; row++) {
    for (uint8_t col = 0; col < 4; col++) {
      if (!cellAt(shp, row, col)) continue;
      int8_t bx = x + col;
      int8_t by = y + row;
      if (bx < 0 || bx >= BOARD_W) return false;
      if (by >= BOARD_H) return false;
      if (by < 0) continue;
      if ((board[by] >> bx) & 1) return false;
    }
  }
  return true;
}

void markLose() {
  gs = GS_OVER;
  matchResult = MATCH_LOSE_RES;
  gameOverDrawn = false;
}

void setActivePiece(uint8_t type) {
  pt = type;
  pr = 0;
  px = 3;
  py = 0;
  if (!isValid(px, py, pt, pr)) markLose();
}

void spawnPiece() {
  holdUsed = false;
  uint8_t type = nextPt;
  nextPt = (uint8_t)random(7);
  setActivePiece(type);
}

void tryRotate() {
  uint8_t nr = (pr + 1) % 4;
  const int8_t kicks[5] = {0, 1, -1, 2, -2};
  for (uint8_t i = 0; i < 5; i++) {
    int8_t nx = px + kicks[i];
    if (isValid(nx, py, pt, nr)) {
      px = nx;
      pr = nr;
      return;
    }
  }
}

void lockPiece() {
  uint16_t shp = getShape(pt, pr);
  for (uint8_t row = 0; row < 4; row++) {
    for (uint8_t col = 0; col < 4; col++) {
      if (!cellAt(shp, row, col)) continue;
      int8_t bx = px + col;
      int8_t by = py + row;
      if (by >= 0 && by < BOARD_H && bx >= 0 && bx < BOARD_W)
        board[by] |= (1u << bx);
    }
  }
}

uint8_t clearLines() {
  uint8_t cleared = 0;
  for (int8_t row = BOARD_H - 1; row >= 0; row--) {
    if (board[row] == 0x03FFu) { // Check if the row is full
      cleared++;
      for (int8_t i = row; i > 0; i--) {
        board[i] = board[i - 1]; // Shift rows down
      }
      board[0] = 0; // Clear the top row
      row++; // Recheck the current row after shifting
    }
  }
  return cleared;
}

void addGarbageLine() {
  uint8_t hole = (uint8_t)random(BOARD_W);
  for (uint8_t row = 0; row < BOARD_H - 1; row++) board[row] = board[row + 1];
  board[BOARD_H - 1] = (uint16_t)(0x03FFu & ~(1u << hole));
}

void applyPendingGarbage() {
  while (pendingGarbage > 0) {
    addGarbageLine();
    pendingGarbage--;
  }
}

uint8_t garbageRowsForClear(uint8_t cleared) {
  if (cleared == 1) return 1; // Single now sends 1 garbage line
  if (cleared == 2) return 2; // Double sends 2 garbage lines
  if (cleared == 3) return 4; // Triple sends 4 garbage lines
  if (cleared >= 4) return 6; // Tetris sends 6 garbage lines
  return 0;
}

uint8_t resolveAttackRows(uint8_t cleared) {
  uint8_t outgoing = garbageRowsForClear(cleared);
  if (outgoing == 0) return 0;

  if (pendingGarbage >= outgoing) {
    pendingGarbage -= outgoing;
    return 0;
  }

  outgoing -= pendingGarbage;
  pendingGarbage = 0;
  return outgoing;
}

uint16_t gravityMs() {
  return (uint16_t)max(80, 800 - (level - 1) * 80);
}

void tryHoldPiece() {
  if (holdUsed || gs != GS_PLAYING) return;

  uint8_t current = pt;
  if (heldPt == PIECE_NONE) {
    heldPt = current;
    spawnPiece();
  } else {
    uint8_t swap = heldPt;
    heldPt = current;
    setActivePiece(swap);
  }
  holdUsed = true;
  tDrop = millis();
}

void lockAndSpawn() {
  lockPiece();
  uint8_t cleared = clearLines();
  lastAttackTx = resolveAttackRows(cleared);
  if (cleared) {
    const uint16_t pts[5] = {0, 100, 300, 500, 800};
    score32 += (uint32_t)pts[cleared] * level;
    linesTotal += cleared;
    level = (uint8_t)(linesTotal / LINES_PER_LV) + 1;
    if (level > 20) level = 20;
  }

  if (cleared == 0 && pendingGarbage) applyPendingGarbage();
  spawnPiece();
  tDrop = millis();
}

inline void drawCell(uint8_t col, uint8_t row) {
  oled.fillRect(1 + col * CELL_PX, 1 + row * CELL_PX, CELL_PX - 1, CELL_PX - 1, WHITE);
}

void drawPiece(int8_t x, int8_t y, uint8_t type, uint8_t rot) {
  uint16_t shp = getShape(type, rot);
  for (uint8_t row = 0; row < 4; row++) {
    for (uint8_t col = 0; col < 4; col++) {
      if (!cellAt(shp, row, col)) continue;
      int8_t bx = x + col;
      int8_t by = y + row;
      if (bx >= 0 && bx < BOARD_W && by >= 0 && by < BOARD_H) drawCell(bx, by);
    }
  }
}

void drawGhost() {
  int8_t gy = py;
  while (isValid(px, gy + 1, pt, pr)) gy++;
  if (gy == py) return;

  uint16_t shp = getShape(pt, pr);
  for (uint8_t row = 0; row < 4; row++) {
    for (uint8_t col = 0; col < 4; col++) {
      if (!cellAt(shp, row, col)) continue;
      int8_t bx = px + col;
      int8_t by = gy + row;
      if (bx >= 0 && bx < BOARD_W && by >= 0 && by < BOARD_H) {
        int x0 = 1 + bx * CELL_PX;
        int y0 = 1 + by * CELL_PX;
        oled.drawRect(x0, y0, CELL_PX - 1, CELL_PX - 1, WHITE);
      }
    }
  }
}

void drawMiniPiece(uint8_t type, int ox, int oy, uint8_t cellPx) {
  if (type == PIECE_NONE) return;
  uint16_t shp = getShape(type, 0);
  for (uint8_t row = 0; row < 4; row++) {
    for (uint8_t col = 0; col < 4; col++) {
      if (cellAt(shp, row, col)) oled.fillRect(ox + col * cellPx, oy + row * cellPx, cellPx, cellPx, WHITE);
    }
  }
}

void drawGame() {
  oled.clearDisplay();
  oled.drawRect(0, 0, BOARD_W * CELL_PX + 2, BOARD_H * CELL_PX + 2, WHITE);

  for (uint8_t row = 0; row < BOARD_H; row++) {
    for (uint8_t col = 0; col < BOARD_W; col++) {
      if ((board[row] >> col) & 1u) drawCell(col, row);
    }
  }

  drawGhost();
  drawPiece(px, py, pt, pr);

  oled.drawFastVLine(UI_X - 2, 0, SCR_H, WHITE);
  oled.setTextSize(1);
  oled.setTextColor(WHITE);

  oled.setCursor(UI_X, 2);
  oled.print(F("P2"));

  oled.setCursor(UI_X, 14);
  oled.print(F("SC"));
  oled.setCursor(UI_X, 24);
  oled.print(min(score32, 99999UL));

  oled.setCursor(UI_X, 40);
  oled.print(F("LN"));
  oled.setCursor(UI_X, 50);
  oled.print(linesTotal);

  oled.setCursor(UI_X, 66);
  oled.print(F("LV"));
  oled.setCursor(UI_X, 76);
  oled.print(level);

  oled.setCursor(UI_X, 90);
  oled.print(F("NX"));
  drawMiniPiece(nextPt, UI_X, 98, 2);

  oled.setCursor(UI_X, 112);
  oled.print(F("HD"));
  drawMiniPiece(heldPt, UI_X, 120, 2);

  oled.display();
}

void drawResultScreen() {
  oled.clearDisplay();
  oled.setTextSize(1);
  oled.setTextColor(WHITE);
  if (matchResult == MATCH_WIN_RES) {
    oled.setCursor(12, 18);
    oled.print(F("YOU WIN"));
    oled.setCursor(6, 40);
    oled.print(F("Other player out"));
  } else if (matchResult == MATCH_LOSE_RES) {
    oled.setCursor(10, 18);
    oled.print(F("YOU LOSE"));
    oled.setCursor(6, 40);
    oled.print(F("RST to retry"));
  } else {
    oled.setCursor(10, 18);
    oled.print(F("GAME OVER"));
    oled.setCursor(6, 40);
    oled.print(F("RST to retry"));
  }
  oled.setCursor(6, 58);
  oled.print(F("Score: "));
  oled.print(min(score32, 99999UL));
  oled.setCursor(6, 72);
  oled.print(F("Lines: "));
  oled.print(linesTotal);
  oled.display();
}

void drawWaitingScreen() {
  oled.clearDisplay();
  oled.setTextSize(1);
  oled.setTextColor(WHITE);
  oled.setCursor(10, 20);
  oled.print(F("PLAYER 2"));
  oled.setCursor(6, 42);
  oled.print(F("Waiting for host"));
  oled.setCursor(8, 64);
  oled.print(F("Start on server"));
  oled.setCursor(10, 86);
  oled.print(F("Hold btn: D4"));
  oled.display();
}

bool sendState() {
  PlayerPkt pkt;
  uint8_t attackToSend = lastAttackTx;
  pkt.pid = PLAYER_ID;
  pkt.score = (uint16_t)min(score32, 65535UL);
  pkt.level = level;
  pkt.lines = linesTotal;
  pkt.nextPiece = nextPt;
  pkt.state = gs;
  pkt.attackRows = attackToSend;

  radio.stopListening();
  bool ok = radio.write(&pkt, sizeof(pkt));
  radio.startListening();

  if (attackToSend) lastAttackTx = 0;
  return ok;
}

void handleServerCommand(const ServerCmd& cmd) {
  Serial.print(F("CMD opcode="));
  Serial.print(cmd.opcode);
  Serial.print(F(" value="));
  Serial.println(cmd.value);

  if (cmd.opcode == CMD_START) {
    gameSeed = cmd.value;
    randomSeed(gameSeed);
    initGame();
  } else if (cmd.opcode == CMD_RESET) {
    enterWaiting();
  } else if (cmd.opcode == CMD_GARBAGE) {
    uint8_t add = (cmd.value > BOARD_H) ? BOARD_H : (uint8_t)cmd.value;
    if ((uint16_t)pendingGarbage + add > BOARD_H) pendingGarbage = BOARD_H;
    else pendingGarbage += add;
  } else if (cmd.opcode == CMD_WIN) {
    gs = GS_OVER;
    matchResult = MATCH_WIN_RES;
    gameOverDrawn = false;
  } else if (cmd.opcode == CMD_LOSE) {
    gs = GS_OVER;
    matchResult = MATCH_LOSE_RES;
    gameOverDrawn = false;
  }
}

void pollServerCommands() {
  while (radio.available()) {
    ServerCmd cmd;
    radio.read(&cmd, sizeof(cmd));
    handleServerCommand(cmd);
  }
}

void handleInput() {
  uint32_t now = millis();
  int xv = analogRead(PIN_JOY_X);
  int yv = analogRead(PIN_JOY_Y);
  bool joyBtn = digitalRead(PIN_JOY_BTN);
  bool holdBtn = digitalRead(PIN_HOLD_BTN);
  bool rstBtn = digitalRead(PIN_RST_BTN);
  bool joyUp = (yv < JOY_LO);

  if (xv < JOY_LO) {
    if (!tLeft || (now - tLeft >= T_MOVE)) {
      if (isValid(px - 1, py, pt, pr)) px--;
      tLeft = now;
    }
  } else {
    tLeft = 0;
  }

  if (xv > JOY_HI) {
    if (!tRight || (now - tRight >= T_MOVE)) {
      if (isValid(px + 1, py, pt, pr)) px++;
      tRight = now;
    }
  } else {
    tRight = 0;
  }

  if (yv > JOY_HI) {
    if (!tSoft || (now - tSoft >= T_SOFT)) {
      if (isValid(px, py + 1, pt, pr)) {
        py++;
        score32++;
        tDrop = now;
      }
      tSoft = now;
    }
  } else {
    tSoft = 0;
  }

  if (joyUp && !prevJoyUp) tryRotate();
  prevJoyUp = joyUp;

  if (!holdBtn && prevHoldBtn && (now - tHoldBtn >= T_BTN_DB)) {
    tryHoldPiece();
    tHoldBtn = now;
  }
  prevHoldBtn = holdBtn;

  if (!joyBtn && prevJoyBtn && (now - tJoyBtn >= T_BTN_DB)) {
    while (isValid(px, py + 1, pt, pr)) {
      py++;
      score32 += 2;
    }
    lockAndSpawn();
    tJoyBtn = now;
  }
  prevJoyBtn = joyBtn;

  if (!rstBtn && prevRstBtn && (now - tRstBtn >= T_BTN_DB)) {
    enterWaiting();
    tRstBtn = now;
  }
  prevRstBtn = rstBtn;
}

void initGame() {
  memset(board, 0, sizeof(board));
  score32 = 0;
  level = 1;
  linesTotal = 0;
  nextPt = (uint8_t)random(7);
  heldPt = PIECE_NONE;
  pendingGarbage = 0;
  lastAttackTx = 0;
  matchResult = MATCH_NONE;
  holdUsed = false;
  spawnPiece();
  gs = GS_PLAYING;
  tDrop = millis();
  gameOverDrawn = false;
}

void enterWaiting() {
  gs = GS_WAITING;
  score32 = 0;
  level = 1;
  linesTotal = 0;
  nextPt = 0;
  heldPt = PIECE_NONE;
  pendingGarbage = 0;
  lastAttackTx = 0;
  matchResult = MATCH_NONE;
  memset(board, 0, sizeof(board));
  tDrop = millis();
  tSend = 0;
  prevJoyUp = false;
  prevHoldBtn = true;
  holdUsed = false;
  gameOverDrawn = false;
  drawWaitingScreen();
}

void setup() {
  Serial.begin(9600);
  Serial.println();
  Serial.println(F("BOOTING P2 FILE"));

  pinMode(PIN_JOY_BTN, INPUT_PULLUP);
  pinMode(PIN_RST_BTN, INPUT_PULLUP);
  pinMode(PIN_HOLD_BTN, INPUT_PULLUP);
  prevJoyBtn = HIGH;
  prevRstBtn = HIGH;
  prevHoldBtn = HIGH;

  if (!oled.begin(SSD1306_SWITCHCAPVCC, 0x3C)) {
    Serial.println(F("OLED not found - check wiring"));
    for (;;) {}
  }
  oled.setRotation(1);
  oled.clearDisplay();
  oled.display();

  if (!radio.begin()) {
    Serial.println(F("NRF24L01 not found - check wiring & 10uF cap"));
    for (;;) {}
  }
  radio.setPALevel(RF24_PA_LOW);
  radio.setDataRate(RF24_250KBPS);
  radio.setChannel(RADIO_CHANNEL);
  radio.setAutoAck(false);
  radio.setPayloadSize(sizeof(PlayerPkt));

  radio.openWritingPipe(ADDR_P2);
  radio.openReadingPipe(1, ADDR_C2);
  radio.startListening();
  printRadioSummary();

  randomSeed((uint32_t)analogRead(A2) ^ ((uint32_t)analogRead(A3) << 10));
  enterWaiting();
}

void loop() {
  uint32_t now = millis();
  pollServerCommands();

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

  if (gs == GS_OVER) {
    if (!gameOverDrawn) {
      drawResultScreen();
      gameOverDrawn = true;
    }

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

  handleInput();

  if (now - tDrop >= gravityMs()) {
    if (isValid(px, py + 1, pt, pr)) py++;
    else lockAndSpawn();
    tDrop = now;
  }

  drawGame();

  if (now - tSend >= T_SEND) {
    bool ok = sendState();
    if (now - tDiag >= 1000) {
      logTxResult(ok);
      tDiag = now;
    }
    tSend = now;
  }
}
