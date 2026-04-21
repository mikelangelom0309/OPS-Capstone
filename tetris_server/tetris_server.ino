// Function prototype for sendStartCommand
void sendStartCommand();
// ============================================================
//  SERVER NRF24 DIAGNOSTIC
//  Flash this onto the server Nano, then open Serial Monitor
//  at 9600 baud to confirm whether packets from P1/P2 arrive.
// ============================================================

#include <SPI.h>
#include <Arduino.h>
#include <Wire.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>
#include <RF24.h>
#include "tetris_shared.h"

#define PIN_CE   9
#define PIN_CSN 10
#define PIN_START_BTN 4   // Physical start button (active LOW)
// Debounce state for start button
bool prevStartBtn = true;
uint32_t tStartBtn = 0;
const uint32_t T_BTN_DB = 240;


#define SCR_W   128
#define SCR_H    64

Adafruit_SSD1306 oled(SCR_W, SCR_H, &Wire, -1);
RF24 radio(PIN_CE, PIN_CSN);

PlayerPkt lastP1 = {1, 0, 1, 0, 0, GS_WAITING, 0};
PlayerPkt lastP2 = {2, 0, 1, 0, 0, GS_WAITING, 0};

uint32_t tP1Heard = 0;
uint32_t tP2Heard = 0;
uint32_t tHeartbeat = 0;
bool oledReady = false;

void printHelp();

const __FlashStringHelper* stateName(uint8_t state) {
  if (state == GS_WAITING) return F("WAITING");
  if (state == GS_PLAYING) return F("PLAYING");
  return F("OVER");
}

void printAddr(const uint8_t* addr) {
  for (uint8_t i = 0; i < 5; i++) Serial.write(addr[i]);
}

void printRadioSummary() {
  Serial.println(F("--- SERVER RADIO ---"));
  Serial.print(F("Chip connected: "));
  Serial.println(radio.isChipConnected() ? F("YES") : F("NO"));
  Serial.print(F("Listening pipe 1: "));
  printAddr(ADDR_P1);
  Serial.println();
  Serial.print(F("Listening pipe 2: "));
  printAddr(ADDR_P2);
  Serial.println();
  radio.printDetails();
  Serial.println(F("--------------------"));
}

void printPacket(uint8_t pipe, const PlayerPkt& pkt) {
  Serial.print(F("RX pipe="));
  Serial.print(pipe);
  Serial.print(F(" pid="));
  Serial.print(pkt.pid);
  Serial.print(F(" state="));
  Serial.print(stateName(pkt.state));
  Serial.print(F(" score="));
  Serial.print(pkt.score);
  Serial.print(F(" lines="));
  Serial.print(pkt.lines);
  Serial.print(F(" level="));
  Serial.print(pkt.level);
  Serial.print(F(" next="));
  Serial.println(pkt.nextPiece);

  if (pipe == 2 && pkt.pid != 2) {
    Serial.println(F("WARNING: PID MISMATCH ON PIPE 2 (expected pid=2)"));
  }
  if (pipe == 1 && pkt.pid != 1) {
    Serial.println(F("WARNING: PID MISMATCH ON PIPE 1 (expected pid=1)"));
  }
}

bool initDisplay() {
  Wire.begin();
  if (oled.begin(SSD1306_SWITCHCAPVCC, 0x3C)) return true;
  if (oled.begin(SSD1306_SWITCHCAPVCC, 0x3D)) return true;
  return false;
}

void drawStatus(uint32_t now) {
  if (!oledReady) return;

  bool p1Online = tP1Heard && (now - tP1Heard <= 1500);
  bool p2Online = tP2Heard && (now - tP2Heard <= 1500);

  oled.clearDisplay();
  oled.setTextSize(1);
  oled.setTextColor(WHITE);

  oled.setCursor(12, 0);
  oled.print(F("SERVER DIAG"));

  oled.setCursor(0, 14);
  oled.print(F("NRF: "));
  oled.print(radio.isChipConnected() ? F("OK") : F("FAIL"));

  oled.setCursor(0, 28);
  oled.print(F("P1: "));
  oled.print(p1Online ? F("ON ") : F("OFF"));
  if (p1Online) {
    oled.print(F(" "));
    oled.print(stateName(lastP1.state));
  }

  oled.setCursor(0, 40);
  oled.print(F("P2: "));
  oled.print(p2Online ? F("ON ") : F("OFF"));
  if (p2Online) {
    oled.print(F(" "));
    oled.print(stateName(lastP2.state));
  }

  oled.setCursor(0, 54);
  oled.print(F("CH "));
  oled.print(RADIO_CHANNEL);
  oled.print(F(" 250KBPS"));

  oled.display();
}

void printHeartbeat(uint32_t now) {
  bool p1Online = tP1Heard && (now - tP1Heard <= 1500);
  bool p2Online = tP2Heard && (now - tP2Heard <= 1500);

  Serial.print(F("HEARTBEAT P1="));
  Serial.print(p1Online ? F("ON") : F("OFF"));
  Serial.print(F(" P2="));
  Serial.print(p2Online ? F("ON") : F("OFF"));

  if (p1Online) {
    Serial.print(F(" | P1 state="));
    Serial.print(stateName(lastP1.state));
  }
  if (p2Online) {
    Serial.print(F(" | P2 state="));
    Serial.print(stateName(lastP2.state));
  }
  Serial.println();
}

void printStatusNow() {
  printHeartbeat(millis());
}

void printHelp() {
  Serial.println(F("Commands:"));
  Serial.println(F("  h = help"));
  Serial.println(F("  s = print current P1/P2 status"));
  Serial.println(F("  d = dump radio config"));
}

void handleSerialCommands() {
  while (Serial.available()) {
    char cmd = (char)Serial.read();

    if (cmd == '\r' || cmd == '\n') continue;

    if (cmd == 'h' || cmd == 'H' || cmd == '?') {
      printHelp();
    } else if (cmd == 's' || cmd == 'S') {
      printStatusNow();
    } else if (cmd == 'd' || cmd == 'D') {
      printRadioSummary();
    } else {
      Serial.print(F("Unknown command: "));
      Serial.println(cmd);
      printHelp();
    }
  }
}

void setup() {
    pinMode(PIN_START_BTN, INPUT_PULLUP);
  Serial.begin(9600);
  Serial.println();
  Serial.println(F("SERVER DIAG BOOT"));

  oledReady = initDisplay();
  if (oledReady) {
    oled.clearDisplay();
    oled.setTextSize(1);
    oled.setTextColor(WHITE);
    oled.setCursor(12, 12);
    oled.print(F("SERVER DIAG"));
    oled.setCursor(0, 30);
    oled.print(F("Starting..."));
    oled.display();
  } else {
    Serial.println(F("OLED not found"));
  }

  if (!radio.begin()) {
    Serial.println(F("NRF24L01 not found - check wiring and power"));
    if (oledReady) {
      oled.clearDisplay();
      oled.setCursor(0, 16);
      oled.print(F("NRF24 NOT FOUND"));
      oled.setCursor(0, 32);
      oled.print(F("Check wiring"));
      oled.display();
    }
    for (;;) {}
  }

  radio.setPALevel(RF24_PA_LOW);
  radio.setDataRate(RF24_250KBPS);
  radio.setChannel(RADIO_CHANNEL);
  radio.setAutoAck(false);
  radio.setPayloadSize(sizeof(PlayerPkt));
  radio.openReadingPipe(1, ADDR_P1);
  radio.openReadingPipe(2, ADDR_P2);
  radio.startListening();

  printRadioSummary();
  Serial.println(F("Open Serial Monitor at 9600 baud"));
  printHelp();
  drawStatus(millis());
}

void loop() {
  uint32_t now = millis();
  // Handle physical start button
  bool startBtn = digitalRead(PIN_START_BTN);
  if (!startBtn && prevStartBtn && (now - tStartBtn >= T_BTN_DB)) {
    sendStartCommand();
    tStartBtn = now;
  }
  prevStartBtn = startBtn;

  handleSerialCommands();

  uint8_t pipe = 0;
  while (radio.available(&pipe)) {
    PlayerPkt pkt;
    radio.read(&pkt, sizeof(pkt));
    printPacket(pipe, pkt);

    if (pkt.pid == 1) {
      lastP1 = pkt;
      tP1Heard = now;
    } else if (pkt.pid == 2) {
      lastP2 = pkt;
      tP2Heard = now;
    } else {
      Serial.print(F("Unexpected pid="));
      Serial.println(pkt.pid);
    }
  }

  if (now - tHeartbeat >= 1000) {
    printHeartbeat(now);
    drawStatus(now);
    tHeartbeat = now;
  }
}

void sendStartCommand() {
  ServerCmd cmd;
  cmd.opcode = CMD_START;
  cmd.gameSeed = (uint16_t)random(0, 65535); // random seed for both players
  memset(cmd.reserved, 0, sizeof(cmd.reserved));

  // Send to both command pipes
  radio.stopListening();
  radio.openWritingPipe(ADDR_C1);
  radio.write(&cmd, sizeof(cmd));
  radio.openWritingPipe(ADDR_C2);
  radio.write(&cmd, sizeof(cmd));
  radio.startListening();

  Serial.print(F("START sent, seed="));
  Serial.print(cmd.gameSeed);
  Serial.println(F(" (button press)"));
  if (oledReady) {
    oled.setCursor(0, 54);
    oled.print(F("START sent!        "));
    oled.display();
  }
}
