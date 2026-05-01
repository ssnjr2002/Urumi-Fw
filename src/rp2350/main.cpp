#include <Arduino.h>

// ─── Pins 
#define RS485_TX_PIN  4
#define RS485_RX_PIN  5
#define RS485_EN_PIN  6

// ─── Bus 
#define RS485_BAUD          230400
#define RESPONSE_TIMEOUT_MS    80

// Buffer for PC -> RS485 relaying
uint8_t relayBuf[256];

// ─── RS485 low-level 
static void rs485Send(const uint8_t *d, uint8_t len) {
    digitalWrite(RS485_EN_PIN, HIGH);
    // On RP2350, we don't need a delay before writing; 
    // Serial2.write is non-blocking until the FIFO is full.
    Serial2.write(d, len);
    
    // Crucial: Wait for the hardware shift register to finish
    Serial2.flush(); 
    
    // At 230400 baud, 1 bit is ~4.3us. A tiny safety margin
    // ensures the last stop bit cleared the wire before we drop EN.
    delayMicroseconds(10); 
    
    digitalWrite(RS485_EN_PIN, LOW);
}

void setup() {
    // USB Serial (PC)
    Serial.begin(115200);
    
    // RS485 Enable Pin
    pinMode(RS485_EN_PIN, OUTPUT);
    digitalWrite(RS485_EN_PIN, LOW);

    // RS485 Serial (Motor)
    Serial2.setTX(RS485_TX_PIN);
    Serial2.setRX(RS485_RX_PIN);
    Serial2.begin(RS485_BAUD);
}

void loop() {
    // 2. RS485 to PC (DEBUG HEX VERSION)
    while (Serial2.available() > 0) {
        uint8_t b = Serial2.read();
        if (b < 0x10) Serial.print('0');
        Serial.print(b, HEX);
        Serial.print(" ");
    }

    // 1. PC to RS485 (Relay)
    if (Serial.available()) {
        digitalWrite(RS485_EN_PIN, HIGH);
        while(Serial.available()) {
            Serial2.write(Serial.read());
        }
        Serial2.flush();
        delayMicroseconds(20); // Slightly longer for safety
        digitalWrite(RS485_EN_PIN, LOW);
    }
}