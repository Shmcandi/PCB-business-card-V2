/**
 * @file main.cpp
 * @brief ESP32-C6 Smart PCB Business Card V2 (Dual NFC + RFID Edition)
 * @author Shmcandi
 *
 * Drives the 96-LED WS2812B-2020 matrix (12 columns x 8 rows) with
 * serpentine addressing, dynamic animation modes, scrolling text,
 * dual 13.56 MHz NFC (NXP NTAG I2C Plus) & 125 kHz RFID (T5577 + modulation),
 * and zero-drain hardware deep sleep via DMG2305UX P-MOSFET load switch.
 */

#include <Arduino.h>
#include <Wire.h>
#include <FastLED.h>
#include <esp_sleep.h>
#include "font5x7.h"

// Hardware Pin Definitions
#define LED_PIN          14   // WS2812B Serial Data Input
#define LED_PWR_PIN      5    // DMG2305UX P-MOSFET Gate (LOW = ON, HIGH = OFF)
#define BTN_USER_PIN     4    // USER tactile switch (active LOW)
#define BTN_BOOT_PIN     9    // BOOT tactile switch (active LOW)
#define I2C_SDA_PIN      6    // NTAG I2C Plus SDA
#define I2C_SCL_PIN      7    // NTAG I2C Plus SCL
#define NFC_FD_PIN       18   // NTAG I2C Field Detect Interrupt (active LOW)
#define RFID_MOD_PIN     1    // 125 kHz Active RFID Load Modulator (Q2 BSS138)

// NXP NTAG I2C Plus Address
#define NTAG_I2C_ADDR    0x55

// Matrix Dimensions
#define MATRIX_WIDTH     12
#define MATRIX_HEIGHT    8
#define NUM_LEDS         (MATRIX_WIDTH * MATRIX_HEIGHT)
#define DEFAULT_BRIGHT   28   // Safe current limit for USB & small LiPo cells

CRGB leds[NUM_LEDS];

enum DisplayMode {
    MODE_RAINBOW = 0,
    MODE_SCROLL_TEXT,
    MODE_NFC_RADAR,
    MODE_PLASMA,
    MODE_SPARKLE,
    MODE_COUNT
};

volatile DisplayMode currentMode = MODE_RAINBOW;
volatile bool nfcTapped = false;
unsigned long lastButtonPress = 0;
unsigned long tapAnimationTime = 0;

void IRAM_ATTR onNfcFieldDetect() {
    nfcTapped = true;
}

/**
 * @brief Serpentine coordinate mapper: (x, y) -> 1D LED index.
 * Even rows (0, 2, 4, 6) flow Right to Left.
 * Odd rows (1, 3, 5, 7) flow Left to Right.
 */
uint16_t XY(uint8_t x, uint8_t y) {
    if (x >= MATRIX_WIDTH || y >= MATRIX_HEIGHT) return 0;
    if (y & 0x01) {
        return (y * MATRIX_WIDTH) + x;
    } else {
        return (y * MATRIX_WIDTH) + (MATRIX_WIDTH - 1 - x);
    }
}

/**
 * @brief Power on the matrix via P-MOSFET.
 */
void enableLEDSupply() {
    pinMode(LED_PWR_PIN, OUTPUT);
    digitalWrite(LED_PWR_PIN, LOW); // Active-LOW: Turn on DMG2305UX
    delay(10);                      // Allow VLED rail to stabilize
}

/**
 * @brief Power down the matrix for 0mA deep-sleep standby.
 */
void disableLEDSupply() {
    FastLED.clear(true);
    digitalWrite(LED_PWR_PIN, HIGH); // Disconnect VLED rail
}

/**
 * @brief Enters ultra-low-power deep sleep. Wakes on button press OR phone tap.
 */
void enterDeepSleep() {
    disableLEDSupply();
    // Wake up when either USER button is pressed OR a phone taps the NFC antenna!
    esp_deep_sleep_enable_gpio_wakeup(1ULL << BTN_USER_PIN, ESP_GPIO_WAKEUP_GPIO_LOW);
    esp_deep_sleep_enable_gpio_wakeup(1ULL << NFC_FD_PIN, ESP_GPIO_WAKEUP_GPIO_LOW);
    esp_deep_sleep_start();
}

/**
 * @brief Transmit 125 kHz EM4100 40-bit ID via Manchester load modulation.
 */
void transmitEM4100(uint64_t cardId) {
    // Standard EM4100 frame: 9 header bits (1s) + 10 nibbles with row parity + 4 column parity + 1 stop bit
    // Modulate on GPIO1 at 2 kBaud (64 clock cycles per bit at 125 kHz -> ~512us per bit period)
    const uint16_t halfBitUs = 256;
    pinMode(RFID_MOD_PIN, OUTPUT);

    auto sendBit = [&](bool bit) {
        // Manchester encoding: 0 = Low-to-High, 1 = High-to-Low
        digitalWrite(RFID_MOD_PIN, bit ? HIGH : LOW);
        delayMicroseconds(halfBitUs);
        digitalWrite(RFID_MOD_PIN, bit ? LOW : HIGH);
        delayMicroseconds(halfBitUs);
    };

    // 9 preamble bits (all 1s)
    for (int i = 0; i < 9; i++) sendBit(1);

    // 10 data nibbles with row even parity
    for (int nibble = 9; nibble >= 0; nibble--) {
        uint8_t val = (cardId >> (nibble * 4)) & 0x0F;
        uint8_t parity = 0;
        for (int b = 3; b >= 0; b--) {
            bool bit = (val >> b) & 1;
            sendBit(bit);
            parity ^= bit;
        }
        sendBit(parity);
    }

    // Stop bit (always 0)
    sendBit(0);
    digitalWrite(RFID_MOD_PIN, LOW);
}

/**
 * @brief Initialize NXP NTAG I2C Plus chip over I2C.
 */
void initNTAG() {
    Wire.begin(I2C_SDA_PIN, I2C_SCL_PIN, 400000);
    Wire.beginTransmission(NTAG_I2C_ADDR);
    if (Wire.endTransmission() == 0) {
        Serial.println("[NFC] NXP NTAG I2C Plus online (Address 0x55)");
    } else {
        Serial.println("[NFC] NTAG I2C Plus not responding (operates in passive mode)");
    }
}

/**
 * @brief Animation 1: Flowing Rainbow Gradient
 */
void renderRainbow(uint8_t step) {
    for (uint8_t y = 0; y < MATRIX_HEIGHT; y++) {
        for (uint8_t x = 0; x < MATRIX_WIDTH; x++) {
            uint8_t hue = step + (x * 14) + (y * 8);
            leds[XY(x, y)] = CHSV(hue, 255, 255);
        }
    }
}

/**
 * @brief Animation 2: NFC / RFID Radar Wave Effect
 */
void renderNfcRadar(uint8_t step) {
    FastLED.clear();
    uint8_t waveRadius = (step / 4) % 10;
    int centerX = 5, centerY = 4;

    for (uint8_t y = 0; y < MATRIX_HEIGHT; y++) {
        for (uint8_t x = 0; x < MATRIX_WIDTH; x++) {
            int dx = x - centerX;
            int dy = y - centerY;
            int dist = (int)sqrt(dx * dx * 2 + dy * dy * 2);
            if (abs(dist - waveRadius) <= 1) {
                leds[XY(x, y)] = CRGB::Cyan;
            } else if (abs(dist - (waveRadius - 2)) <= 1) {
                leds[XY(x, y)] = CRGB::DarkBlue;
            }
        }
    }
}

/**
 * @brief Animation 3: Smooth Plasma Waves
 */
void renderPlasma(uint8_t step) {
    for (uint8_t y = 0; y < MATRIX_HEIGHT; y++) {
        for (uint8_t x = 0; x < MATRIX_WIDTH; x++) {
            uint8_t color = sin8(x * 20 + step) + cos8(y * 24 + step);
            leds[XY(x, y)] = CHSV(color, 240, 255);
        }
    }
}

/**
 * @brief Animation 4: Digital Sparkle / Starfield
 */
void renderSparkle() {
    fadeToBlackBy(leds, NUM_LEDS, 25);
    if (random8() < 90) {
        uint8_t x = random8(MATRIX_WIDTH);
        uint8_t y = random8(MATRIX_HEIGHT);
        leds[XY(x, y)] = CHSV(random8(), random8(128, 255), 255);
    }
}

/**
 * @brief Animation 5: Smooth Scrolling Text
 */
void renderScrollText(const char* text, int offset) {
    FastLED.clear();
    int charWidth = 6;

    for (int col = 0; col < MATRIX_WIDTH; col++) {
        int textX = col + offset;
        if (textX < 0) continue;

        int charIdx = textX / charWidth;
        int colInChar = textX % charWidth;

        if (charIdx >= strlen(text)) continue;

        char c = text[charIdx];
        if (c < 32 || c > 90) continue;

        if (colInChar < 5) {
            uint8_t glyphCol = FONT_5X7[c - 32][colInChar];
            for (uint8_t row = 0; row < 7; row++) {
                if (glyphCol & (1 << row)) {
                    leds[XY(col, row)] = CRGB::Green;
                }
            }
        }
    }
}

/**
 * @brief Celebration animation triggered when a smartphone taps the card.
 */
void renderTapCelebration() {
    fill_rainbow(leds, NUM_LEDS, millis() / 2, 7);
}

void setup() {
    Serial.begin(115200);

    // 1. Initialize power gate and enable LED power rail
    enableLEDSupply();

    // 2. Configure tactile inputs & interrupts
    pinMode(BTN_USER_PIN, INPUT_PULLUP);
    pinMode(BTN_BOOT_PIN, INPUT_PULLUP);
    pinMode(NFC_FD_PIN, INPUT_PULLUP);
    attachInterrupt(digitalPinToInterrupt(NFC_FD_PIN), onNfcFieldDetect, FALLING);

    // 3. Initialize I2C and NTAG dynamic tag
    initNTAG();

    // 4. Initialize FastLED controller
    FastLED.addLeds<WS2812B, LED_PIN, GRB>(leds, NUM_LEDS);
    FastLED.setBrightness(DEFAULT_BRIGHT);
    FastLED.clear(true);
}

void loop() {
    static uint8_t animStep = 0;
    static int scrollOffset = -MATRIX_WIDTH;
    static unsigned long lastFrameTime = 0;
    const char* cardText = "SHMCANDI - DUAL 13.56MHz NFC + 125kHz RFID SMART BUSINESS CARD ";

    // Handle NFC Phone Tap Event
    if (nfcTapped) {
        nfcTapped = false;
        tapAnimationTime = millis() + 1500; // Show celebration for 1.5 seconds
        // Also transmit active 125 kHz credential beacon
        transmitEM4100(0x0102030405ULL);
    }

    // Handle Button Press (Mode Switching & Long-Press Sleep)
    if (digitalRead(BTN_USER_PIN) == LOW) {
        if (millis() - lastButtonPress > 250) {
            lastButtonPress = millis();
            
            // Check for hold to trigger deep sleep
            delay(400);
            if (digitalRead(BTN_USER_PIN) == LOW) {
                enterDeepSleep();
            }

            // Otherwise, cycle animation mode
            currentMode = (DisplayMode)((currentMode + 1) % MODE_COUNT);
            FastLED.clear(true);
            scrollOffset = -MATRIX_WIDTH;
        }
    }

    // Render frame at ~40 FPS
    if (millis() - lastFrameTime >= 25) {
        lastFrameTime = millis();

        if (millis() < tapAnimationTime) {
            renderTapCelebration();
        } else {
            switch (currentMode) {
                case MODE_RAINBOW:
                    renderRainbow(animStep);
                    animStep += 3;
                    break;

                case MODE_NFC_RADAR:
                    renderNfcRadar(animStep);
                    animStep++;
                    break;

                case MODE_PLASMA:
                    renderPlasma(animStep);
                    animStep += 2;
                    break;

                case MODE_SPARKLE:
                    renderSparkle();
                    break;

                case MODE_SCROLL_TEXT:
                    renderScrollText(cardText, scrollOffset);
                    if ((animStep % 3) == 0) {
                        scrollOffset++;
                        if (scrollOffset > (int)(strlen(cardText) * 6)) {
                            scrollOffset = -MATRIX_WIDTH;
                        }
                    }
                    animStep++;
                    break;

                default:
                    break;
            }
        }

        FastLED.show();
    }
}
