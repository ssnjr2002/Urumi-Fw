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
    // 1. PC to RS485
    // Collect bytes from PC into a buffer so we send them in one TX burst
    uint8_t count = 0;
    while (Serial.available() > 0 && count < sizeof(relayBuf)) {
        relayBuf[count++] = Serial.read();
        // Small micro-delay to let the USB buffer fill if a packet is coming
        delayMicroseconds(50); 
    }

    if (count > 0) {
        rs485Send(relayBuf, count);
    }

    // 2. RS485 to PC
    // If the motor replies, send it straight to the PC
    while (Serial2.available() > 0) {
        Serial.write(Serial2.read());
    }
}