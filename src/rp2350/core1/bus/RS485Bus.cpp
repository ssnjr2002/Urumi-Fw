#include "RS485Bus.h"

RS485Bus::RS485Bus(PIO pio_inst) {
    _pio = pio_inst;
}

void RS485Bus::begin(uint32_t baud, uint tx_pin, uint rx_pin, uint de_pin) {
    // Load TX program
    uint offset_tx = pio_add_program(_pio, &uart_tx_9bit_program);
    _sm_tx = pio_claim_unused_sm(_pio, true);
    uart_tx_9bit_program_init(_pio, _sm_tx, offset_tx, tx_pin, de_pin, baud);

    // Load RX program
    uint offset_rx = pio_add_program(_pio, &uart_rx_9bit_program);
    _sm_rx = pio_claim_unused_sm(_pio, true);
    uart_rx_9bit_program_init(_pio, _sm_rx, offset_rx, rx_pin, baud);
}

void RS485Bus::writeCommand(uint8_t data) {
    // 9th bit = 1
    pio_sm_put_blocking(_pio, _sm_tx, (uint32_t)data | (1 << 8));
}

void RS485Bus::writeStream(uint8_t data) {
    // 9th bit = 0
    pio_sm_put_blocking(_pio, _sm_tx, (uint32_t)data);
}

void RS485Bus::writeRaw(uint16_t data) {
    pio_sm_put_blocking(_pio, _sm_tx, (uint32_t)data);
}

bool RS485Bus::available() {
    return !pio_sm_is_rx_fifo_empty(_pio, _sm_rx);
}

uint16_t RS485Bus::read() {
    uint32_t raw = pio_sm_get(_pio, _sm_rx);
    return (uint16_t)((raw >> 23) & 0x01FF);
}

void RS485Bus::flushRX() {
    while (!pio_sm_is_rx_fifo_empty(_pio, _sm_rx)) {
        pio_sm_get(_pio, _sm_rx);
    }
}

bool RS485Bus::txEmpty() {
    return pio_sm_is_tx_fifo_empty(_pio, _sm_tx);
}
