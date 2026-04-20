// ============================================================
//  tetris_shared.h
//  Shared constants and packet structure.
//  Include this file in BOTH sketches — do not edit unless
//  you update both at the same time.
// ============================================================
#pragma once

// ── NRF24L01 channel & data rate ─────────────────────────────
// Change RADIO_CHANNEL if you have interference (1–125).
#define RADIO_CHANNEL   108

// ── Pipe addresses (exactly 5 chars + null) ───────────────────
// P1 and P2 boards transmit on their own address.
// Server listens on both. Players also listen on a dedicated
// command pipe so the server can start/reset both boards.
const uint8_t ADDR_P1[6] = "PLYRA";
const uint8_t ADDR_P2[6] = "PLYRB";
const uint8_t ADDR_C1[6] = "CMD1A";
const uint8_t ADDR_C2[6] = "CMD2A";

// ── Game states ───────────────────────────────────────────────
#define GS_WAITING  0
#define GS_PLAYING  1
#define GS_OVER     2

// ── Server → player commands ──────────────────────────────────
#define CMD_NONE    0
#define CMD_START   1
#define CMD_RESET   2

// ── Packet sent from each player board → server every 100 ms ──
// Total size: 8 bytes (well within NRF24 32-byte max payload).
struct __attribute__((packed)) PlayerPkt {
  uint8_t  pid;        // 1 or 2
  uint16_t score;      // capped at 65535
  uint8_t  level;
  uint8_t  lines;      // total lines cleared
  uint8_t  nextPiece;  // 0-6, index into SHAPES[]
  uint8_t  state;      // GS_*
  uint8_t  reserved;   // keep a fixed 8-byte NRF payload
};

// ── Packet sent from server → players ─────────────────────────
// Fixed 8-byte payload to match the player packet size.
struct __attribute__((packed)) ServerCmd {
  uint8_t  opcode;     // CMD_*
  uint16_t gameSeed;   // used to synchronise both players on start
  uint8_t  reserved[5];
};
