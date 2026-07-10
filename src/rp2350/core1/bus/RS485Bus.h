#pragma once

#include <Arduino.h>
#include "hardware/pio.h"
#include "uart_9bit.pio.h"

class RS485Bus {
public:
    RS485Bus(PIO pio_inst = pio0);
    
    // Initialize the bus PIO state machines
    void begin(uint32_t baud, uint tx_pin, uint rx_pin, uint de_pin);
    
    // --- Writers ---
    // Transmit a Command Byte (9th bit = 1)
    void writeCommand(uint8_t data);
    
    // Transmit a Stream Byte (9th bit = 0)
    void writeStream(uint8_t data);
    
    // Transmit a raw 9-bit word
    void writeRaw(uint16_t data);
    
    // --- Readers ---
    // Check if data is available in the RX FIFO
    bool available();
    
    // Read a raw 9-bit word from the RX FIFO
    uint16_t read();
    
    // Flush all pending bytes from the RX FIFO
    void flushRX();
    
    // --- Status ---
    // Check if the TX FIFO is empty
    bool txEmpty();
    
private:
    PIO _pio;
    uint _sm_tx;
    uint _sm_rx;
};
