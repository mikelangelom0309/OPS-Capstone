# OPS-Capstone (Tetris)
IEEE Capstone Project.\
Contributors: Mikelangelo Mutti, Ayan Singh, Wihl Costelo, Carson Phuong

# Multiplayer Tetris — Arduino Nano

## Hardware
- 3× Arduino Nano
- 3× SSD1306 128×64 OLED (I2C)
- 2× Analog joystick module
- 3× NRF24L01+ transceiver
- 3× 10µF electrolytic capacitor

## Libraries Required
- RF24 by TMRh20
- Adafruit SSD1306
- Adafruit GFX Library

## Folder Structure
tetris_player/ → upload to Player 1 and Player 2 Nanos (change PLAYER_ID)
tetris_server/ → upload to Server Nano

## Upload Order
1. tetris_player.ino with PLAYER_ID 1 → Player 1 Nano
2. tetris_player.ino with PLAYER_ID 2 → Player 2 Nano
3. tetris_server.ino → Server Nano
