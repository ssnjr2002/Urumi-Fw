#include <stdint.h>

template <typename T, uint8_t Size>
class RingBuffer {
    static_assert((Size & (Size - 1)) == 0, "Size must be a power of 2");

private:
    T buffer[Size];
    volatile uint8_t head = 0;
    volatile uint8_t tail = 0;
    static const uint8_t Mask = Size - 1;

public:
    inline bool isFull()  const { return ((tail + 1) & Mask) == head; }
    inline bool isEmpty() const { return head == tail; }
    inline uint8_t count() const { return (tail - head) & Mask; }

    // Producer: Returns pointer to the next slot to fill manually (e.g. for ISRs)
    inline T* getWritePtr() { 
        return isFull() ? nullptr : &buffer[tail]; 
    }

    // Producer: Finalizes the write
    inline void advanceTail() { 
        __asm__ volatile ("" ::: "memory");
        tail = (tail + 1) & Mask; 
    }

    // Producer: standard push
    inline bool push(const T& item) {
        if (isFull()) return false;
        buffer[tail] = item;
        advanceTail();
        return true;
    }

    // Consumer: Returns pointer to current head without removing
    inline T* peek() {
        return isEmpty() ? nullptr : &buffer[head];
    }

    // Consumer: standard pop
    inline bool pop(T& item) {
        if (isEmpty()) return false;
        item = buffer[head];
        __asm__ volatile ("" ::: "memory");
        head = (head + 1) & Mask;
        return true;
    }

    // Consumer: Discard head
    inline void advanceHead() { head = (head + 1) & Mask; }

    // Force clear (used for Emergency Stop)
    inline void clear() { head = tail = 0; }
};