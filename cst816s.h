#ifndef CST816S_H
#define CST816S_H

#include <Arduino.h>
#include <Wire.h>

#define CST816S_I2C_ADDR 0x15

// CST816S Hardware Pin Definitions (Waveshare ESP32-S3-Touch-LCD-1.28)
#define CST816S_PIN_SDA 6
#define CST816S_PIN_SCL 7
#define CST816S_PIN_RST 13
#define CST816S_PIN_INT 5

// CST816S Gesture Codes
enum CST816S_Gesture {
    GESTURE_NONE         = 0x00,
    GESTURE_SWIPE_UP     = 0x01,
    GESTURE_SWIPE_DOWN   = 0x02,
    GESTURE_SWIPE_RIGHT  = 0x03, // Decrement screen
    GESTURE_SWIPE_LEFT   = 0x04, // Increment screen
    GESTURE_SINGLE_CLICK = 0x05,
    GESTURE_DOUBLE_CLICK = 0x0B,
    GESTURE_LONG_PRESS   = 0x0C
};

class CST816S {
public:
    CST816S(int8_t sda = CST816S_PIN_SDA, int8_t scl = CST816S_PIN_SCL,
            int8_t rst = CST816S_PIN_RST, int8_t irq = CST816S_PIN_INT);

    bool begin();
    bool readTouch(uint8_t &gesture, uint16_t &x, uint16_t &y, bool &isPressed);

private:
    int8_t _sda;
    int8_t _scl;
    int8_t _rst;
    int8_t _irq;
    bool _initialized;
};

#endif // CST816S_H
