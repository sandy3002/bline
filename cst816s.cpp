#include "cst816s.h"

CST816S::CST816S(int8_t sda, int8_t scl, int8_t rst, int8_t irq)
    : _sda(sda), _scl(scl), _rst(rst), _irq(irq), _initialized(false) {}

bool CST816S::begin() {
    if (_rst >= 0) {
        pinMode(_rst, OUTPUT);
        digitalWrite(_rst, LOW);
        delay(20);
        digitalWrite(_rst, HIGH);
        delay(50);
    }

    if (_irq >= 0) {
        pinMode(_irq, INPUT_PULLUP);
    }

    Wire.begin(_sda, _scl, 400000);

    // Verify I2C acknowledgement
    Wire.beginTransmission(CST816S_I2C_ADDR);
    if (Wire.endTransmission() != 0) {
        // Try once more with a small delay
        delay(10);
        Wire.beginTransmission(CST816S_I2C_ADDR);
        if (Wire.endTransmission() != 0) {
            _initialized = false;
            return false;
        }
    }

    _initialized = true;
    return true;
}

bool CST816S::readTouch(uint8_t &gesture, uint16_t &x, uint16_t &y, bool &isPressed) {
    gesture = GESTURE_NONE;
    isPressed = false;
    if (!_initialized) return false;

    Wire.beginTransmission(CST816S_I2C_ADDR);
    Wire.write(0x01); // Start reading from gesture register
    if (Wire.endTransmission() != 0) {
        return false;
    }

    if (Wire.requestFrom((uint8_t)CST816S_I2C_ADDR, (uint8_t)6) != 6) {
        return false;
    }

    uint8_t gest = Wire.read();      // 0x01
    uint8_t points = Wire.read();    // 0x02
    uint8_t xHigh = Wire.read();     // 0x03
    uint8_t xLow = Wire.read();      // 0x04
    uint8_t yHigh = Wire.read();     // 0x05
    uint8_t yLow = Wire.read();      // 0x06

    gesture = gest;
    isPressed = (points > 0);

    if (isPressed) {
        x = ((uint16_t)(xHigh & 0x0F) << 8) | xLow;
        y = ((uint16_t)(yHigh & 0x0F) << 8) | yLow;
    }

    return (gesture != GESTURE_NONE || isPressed);
}
