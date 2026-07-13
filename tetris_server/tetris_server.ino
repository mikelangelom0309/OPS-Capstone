// ============================================================
//  SERVER MULTIPLAYER CONTROL
// ============================================================

#include <SPI.h>
#include <Arduino.h>
#include <Wire.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>
#include <RF24.h>
#include "tetris_shared.h"

#define PIN_CE         9
#define PIN_CSN        10
#define PIN_START_BTN  4
#define PIN_BUZZER     8
#define SCR_W          128
#define SCR_H          64
#define T_BTN_DB       240
#define T_START_BCAST  120
#define START_REPEATS  10

#define BPM            120UL
#define MUSIC_UNIT_MS  (60000UL / (BPM * 2UL))
#define POLY_SLICE_MS  36UL

#define _R             0
#define _GS1           52
#define _A1            55
#define _B1            62
#define _C2            65
#define _D2            73
#define _E2            82
#define _A2            110
#define _B2            123
#define _C3            131
#define _D3            147
#define _E3            165
#define _GS3           208
#define _A3            220
#define _B3            247
#define _C4            262
#define _D4            294
#define _E4            330
#define _A4            440
#define _B4            494
#define _C5            523
#define _D5            587
#define _E5            659
#define _F5            698
#define _G5            784
#define _A5            880

Adafruit_SSD1306 oled(SCR_W, SCR_H, &Wire, -1);
RF24 radio(PIN_CE, PIN_CSN);

PlayerPkt lastP1 = {1, 0, 1, 0, 0, GS_WAITING, 0};
PlayerPkt lastP2 = {2, 0, 1, 0, 0, GS_WAITING, 0};

uint32_t tP1Heard = 0;
uint32_t tP2Heard = 0;
uint32_t tHeartbeat = 0;
uint32_t tStartBtn = 0;
uint32_t tStartBroadcast = 0;
uint32_t tMusicSectionStart = 0;
uint32_t tMusicSlice = 0;

bool prevStartBtn = true;
bool oledReady = false;
bool matchActive = false;
uint8_t winnerPid = 0;
uint16_t pendingStartSeed = 0;
uint8_t pendingStartRepeats = 0;
bool musicPlaying = false;
uint8_t leadIndex = 0;
uint8_t bassIndex = 0;
uint8_t leadRemaining = 0;
uint8_t bassRemaining = 0;
bool playLeadSlice = true;

const uint16_t LEAD_NOTES[] PROGMEM = {
  _E5, _B4, _C5, _D5, _C5, _B4, _A4, _A4, _C5, _E5, _D5, _C5, _B4, _B4, _C5, _D5, _E5, _C5, _A4, _A4, _R,
  _D5, _F5, _A5, _G5, _F5, _E5, _C5, _E5, _D5, _C5, _B4, _B4, _C5, _D5, _E5, _C5, _A4, _A4, _R,
  _E4, _C4, _D4, _B3, _C4, _A3, _GS3, _B3,
  _E4, _C4, _D4, _B3, _C4, _E4, _A4, _A4, _GS3, _R
};

const uint8_t LEAD_TIMES[] PROGMEM = {
  2, 1, 1, 2, 1, 1, 2, 1, 1, 2, 1, 1, 2, 1, 1, 2, 2, 2, 2, 2, 2,
  3, 1, 2, 1, 1, 3, 1, 2, 1, 1, 2, 1, 1, 2, 2, 2, 2, 2, 2,
  4, 4, 4, 4, 4, 4, 4, 4,
  4, 4, 4, 4, 2, 2, 2, 2, 6, 2
};

const uint16_t BASS_NOTES[] PROGMEM = {
  _E2, _E3, _E2, _E3, _E2, _E3, _E2, _E3, _A1, _A2, _A1, _A2, _A1, _A2, _A1, _A2,
  _GS1, _GS1, _GS1, _GS1, _GS1, _GS1, _GS1, _GS1, _A1, _A2, _A1, _A2, _A1, _B2, _C3, _E3,
  _D2, _D3, _D2, _D3, _D2, _D3, _D2, _D3, _C2, _C3, _C2, _C3, _C2, _C3, _C2, _C3,
  _B1, _B2, _B1, _B2, _B1, _B2, _B1, _B2, _A1, _A2, _A1, _A2, _A1, _A2, _A1, _A2,
  _A1, _E2, _A1, _E2, _A1, _E2, _A1, _E2, _GS1, _E2, _GS1, _E2, _GS1, _E2, _GS1, _E2,
  _A1, _E2, _A1, _E2, _A1, _E2, _A1, _E2, _GS1, _E2, _GS1, _E2, _GS1, _E2, _GS1, _E2,
  _A1, _E2, _A1, _E2, _A1, _E2, _A1, _E2, _GS1, _E2, _GS1, _E2, _GS1, _E2, _GS1, _E2,
  _A1, _E2, _A1, _E2, _A1, _E2, _A1, _E2, _GS1, _E2, _GS1, _E2, _GS1, _E2, _GS1, _E2
};

const uint8_t BASS_TIMES[] PROGMEM = {
  1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1,
  1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1,
  1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1,
  1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1,
  1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1,
  1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1,
  1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1,
  1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1
};

const uint8_t LEAD_NOTE_COUNT = sizeof(LEAD_NOTES) / sizeof(LEAD_NOTES[0]);
const uint8_t BASS_NOTE_COUNT = sizeof(BASS_NOTES) / sizeof(BASS_NOTES[0]);

const __FlashStringHelper* stateName(uint8_t state) {
  if (state == GS_WAITING) return F("WAITING");
  if (state == GS_PLAYING) return F("PLAYING");
  return F("OVER");
}

void printAddr(const uint8_t* addr) {
  for (uint8_t i = 0; i < 5; i++) Serial.write(addr[i]);
}

bool playerOnline(uint8_t playerId, uint32_t now) {
  uint32_t heard = (playerId == 1) ? tP1Heard : tP2Heard;
  return heard && (now - heard <= 1500);
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
  Serial.print(F(" attack="));
  Serial.println(pkt.attackRows);
}

bool initDisplay() {
  Wire.begin();
  if (oled.begin(SSD1306_SWITCHCAPVCC, 0x3C)) return true;
  if (oled.begin(SSD1306_SWITCHCAPVCC, 0x3D)) return true;
  return false;
}

void sendCommand(uint8_t playerId, uint8_t opcode, uint16_t value) {
  ServerCmd cmd;
  cmd.opcode = opcode;
  cmd.value = value;
  memset(cmd.reserved, 0, sizeof(cmd.reserved));

  radio.stopListening();
  radio.openWritingPipe(playerId == 1 ? ADDR_C1 : ADDR_C2);
  radio.write(&cmd, sizeof(cmd));
  radio.startListening();
}

void resetMusicState() {
  leadIndex = 0;
  bassIndex = 0;
  leadRemaining = pgm_read_byte(&LEAD_TIMES[0]);
  bassRemaining = pgm_read_byte(&BASS_TIMES[0]);
  tMusicSectionStart = 0;
  tMusicSlice = 0;
  playLeadSlice = true;
}

void sendStartCommand() {
  uint32_t now = millis();
  if (!playerOnline(1, now) || !playerOnline(2, now)) {
    Serial.println(F("START ignored: both players must be online"));
    return;
  }

  pendingStartSeed = (uint16_t)random(1, 65535);
  pendingStartRepeats = START_REPEATS;
  tStartBroadcast = 0;
  matchActive = true;
  winnerPid = 0;
  musicPlaying = true;
  resetMusicState();

  Serial.print(F("MATCH START queued seed="));
  Serial.println(pendingStartSeed);
}

void finishMatch(uint8_t loserPid) {
  if (!matchActive) return;

  uint8_t winner = (loserPid == 1) ? 2 : 1;
  sendCommand(loserPid, CMD_LOSE, 0);
  sendCommand(winner, CMD_WIN, 0);
  matchActive = false;
  winnerPid = winner;
  pendingStartRepeats = 0;
  musicPlaying = false;
  noTone(PIN_BUZZER);

  Serial.print(F("MATCH END winner=P"));
  Serial.println(winner);
}

void serviceMusic(uint32_t now) {
  if (!musicPlaying || !matchActive) {
    noTone(PIN_BUZZER);
    return;
  }

  if (leadIndex >= LEAD_NOTE_COUNT || bassIndex >= BASS_NOTE_COUNT) {
    resetMusicState();
  }

  uint8_t sectionUnits = min(leadRemaining, bassRemaining);
  uint32_t sectionDuration = (uint32_t)sectionUnits * MUSIC_UNIT_MS;

  if (!tMusicSectionStart) {
    tMusicSectionStart = now;
    tMusicSlice = 0;
    playLeadSlice = true;
  }

  if (now - tMusicSectionStart >= sectionDuration) {
    leadRemaining -= sectionUnits;
    bassRemaining -= sectionUnits;

    if (leadRemaining == 0) {
      leadIndex++;
      if (leadIndex >= LEAD_NOTE_COUNT) leadIndex = 0;
      leadRemaining = pgm_read_byte(&LEAD_TIMES[leadIndex]);
    }

    if (bassRemaining == 0) {
      bassIndex++;
      if (bassIndex >= BASS_NOTE_COUNT) bassIndex = 0;
      bassRemaining = pgm_read_byte(&BASS_TIMES[bassIndex]);
    }

    tMusicSectionStart = now;
    tMusicSlice = 0;
    playLeadSlice = true;
    sectionUnits = min(leadRemaining, bassRemaining);
    sectionDuration = (uint32_t)sectionUnits * MUSIC_UNIT_MS;
  }

  if (tMusicSlice && (now - tMusicSlice < POLY_SLICE_MS)) return;

  uint16_t leadNote = pgm_read_word(&LEAD_NOTES[leadIndex]);
  uint16_t bassNote = pgm_read_word(&BASS_NOTES[bassIndex]);
  uint16_t note = _R;

  if (leadNote && bassNote) {
    note = playLeadSlice ? leadNote : bassNote;
    playLeadSlice = !playLeadSlice;
  } else if (leadNote) {
    note = leadNote;
  } else if (bassNote) {
    note = bassNote;
  }

  if (note == _R) noTone(PIN_BUZZER);
  else tone(PIN_BUZZER, note);

  tMusicSlice = now;
}

void servicePendingStart(uint32_t now) {
  if (pendingStartRepeats == 0) return;
  if (tStartBroadcast && (now - tStartBroadcast < T_START_BCAST)) return;

  sendCommand(1, CMD_START, pendingStartSeed);
  sendCommand(2, CMD_START, pendingStartSeed);
  pendingStartRepeats--;
  tStartBroadcast = now;

  Serial.print(F("START broadcast seed="));
  Serial.print(pendingStartSeed);
  Serial.print(F(" remaining="));
  Serial.println(pendingStartRepeats);
}

void drawStatus(uint32_t now) {
  if (!oledReady) return;

  bool p1Online = playerOnline(1, now);
  bool p2Online = playerOnline(2, now);

  oled.clearDisplay();
  oled.setTextSize(1);
  oled.setTextColor(WHITE);

  oled.setCursor(18, 0);
  oled.print(F("TETRIS HOST"));

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
  if (winnerPid) {
    oled.print(F("WINNER: P"));
    oled.print(winnerPid);
  } else if (matchActive) {
    oled.print(F("MATCH LIVE"));
  } else if (pendingStartRepeats) {
    oled.print(F("STARTING..."));
  } else if (p1Online && p2Online) {
    oled.print(F("D4 START READY"));
  } else {
    oled.print(F("WAIT FOR BOTH"));
  }

  oled.display();
}

void printHeartbeat(uint32_t now) {
  Serial.print(F("HEARTBEAT P1="));
  Serial.print(playerOnline(1, now) ? F("ON") : F("OFF"));
  Serial.print(F(" P2="));
  Serial.print(playerOnline(2, now) ? F("ON") : F("OFF"));
  Serial.print(F(" live="));
  Serial.print(matchActive ? F("YES") : F("NO"));
  if (winnerPid) {
    Serial.print(F(" winner=P"));
    Serial.print(winnerPid);
  }
  Serial.println();
}

void printHelp() {
  Serial.println(F("Commands:"));
  Serial.println(F("  h = help"));
  Serial.println(F("  s = print current status"));
  Serial.println(F("  d = dump radio config"));
  Serial.println(F("  g = start match"));
}

void handleSerialCommands() {
  while (Serial.available()) {
    char cmd = (char)Serial.read();
    if (cmd == '\r' || cmd == '\n') continue;

    if (cmd == 'h' || cmd == 'H' || cmd == '?') printHelp();
    else if (cmd == 's' || cmd == 'S') printHeartbeat(millis());
    else if (cmd == 'd' || cmd == 'D') printRadioSummary();
    else if (cmd == 'g' || cmd == 'G') sendStartCommand();
    else {
      Serial.print(F("Unknown command: "));
      Serial.println(cmd);
    }
  }
}

void processGameplayEffects(const PlayerPkt& pkt) {
  if (!matchActive) return;

  if (pkt.state == GS_OVER) {
    finishMatch(pkt.pid);
    return;
  }

  uint8_t garbage = pkt.attackRows;
  if (garbage == 0) return;

  uint8_t target = (pkt.pid == 1) ? 2 : 1;
  sendCommand(target, CMD_GARBAGE, garbage);
  Serial.print(F("SEND garbage="));
  Serial.print(garbage);
  Serial.print(F(" -> P"));
  Serial.println(target);
}

void setup() {
  pinMode(PIN_START_BTN, INPUT_PULLUP);
  pinMode(PIN_BUZZER, OUTPUT);
  noTone(PIN_BUZZER);
  Serial.begin(9600);
  Serial.println();
  Serial.println(F("SERVER HOST BOOT"));

  oledReady = initDisplay();
  if (!oledReady) Serial.println(F("OLED not found"));

  if (!radio.begin()) {
    Serial.println(F("NRF24L01 not found - check wiring and power"));
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

  randomSeed((uint32_t)analogRead(A0) ^ ((uint32_t)analogRead(A1) << 8));
  printRadioSummary();
  printHelp();
  drawStatus(millis());
}

void loop() {
  uint32_t now = millis();
  bool startBtn = digitalRead(PIN_START_BTN);
  if (!startBtn && prevStartBtn && (now - tStartBtn >= T_BTN_DB)) {
    Serial.println(F("D4 start button pressed"));
    sendStartCommand();
    tStartBtn = now;
  }
  prevStartBtn = startBtn;

  servicePendingStart(now);
  serviceMusic(now);

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
      continue;
    }

    processGameplayEffects(pkt);
  }

  if (now - tHeartbeat >= 1000) {
    printHeartbeat(now);
    drawStatus(now);
    tHeartbeat = now;
  }
}
