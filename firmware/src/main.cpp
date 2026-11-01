/**
 * @file main.cpp
 * @brief ESP32-C6 Smart PCB Business Card V2 Firmware
 * @author Shmcandi
 *
 * Drives the 96-LED WS2812B-2020 matrix (12 columns x 8 rows) with
 * serpentine addressing, dynamic animation modes, scrolling text,
 * and zero-drain hardware deep sleep via DMG2305UX P-MOSFET load switch.
 */

#include <Arduino.h>
#include <FastLED.h>
#include <esp_sleep.h>
#include "font5x7.h"

// Hardware Pin Definitions
#define LED_PIN          14   // WS2812B Serial Data Input
#define LED_PWR_PIN      5    // DMG2305UX P-MOSFET Gate (LOW = ON, HIGH = OFF)
#define BTN_USER_PIN     4    // USER tactile switch (active LOW)
#define BTN_BOOT_PIN     9    // BOOT tactile switch (active LOW)

// Matrix Dimensions
#define MATRIX_WIDTH     12
#define MATRIX_HEIGHT    8
#define NUM_LEDS         (MATRIX_WIDTH * MATRIX_HEIGHT)
#define DEFAULT_BRIGHT   28   // Safe current limit for USB & small LiPo cells

CRGB leds[NUM_LEDS];

enum DisplayMode {
    MODE_RAINBOW = 0,
    MODE_SCROLL_TEXT,
    MODE_PLASMA,
    MODE_SPARKLE,
    MODE_COUNT
};

volatile DisplayMode currentMode = MODE_RAINBOW;
unsigned long lastButtonPress = 0;

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
 * @brief Enters ultra-low-power deep sleep. Wakes on USER button press.
 */
void enterDeepSleep() {
    disableLEDSupply();
    esp_deep_sleep_enable_gpio_wakeup(1ULL << BTN_USER_PIN, ESP_GPIO_WAKEUP_GPIO_LOW);
    esp_deep_sleep_start();
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
 * @brief Animation 2: Smooth Plasma Waves
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
 * @brief Animation 3: Digital Sparkle / Starfield
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
 * @brief Animation 4: Smooth Scrolling Text
 */
void renderScrollText(const char* text, int offset) {
    FastLED.clear();
    int charWidth = 6; // 5 font pixels + 1 space

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
                    leds[XY(col, row)] = CRGB::Cyan;
                }
            }
        }
    }
}

void setup() {
    // 1. Initialize power gate and enable LED power rail
    enableLEDSupply();

    // 2. Configure tactile inputs
    pinMode(BTN_USER_PIN, INPUT_PULLUP);
    pinMode(BTN_BOOT_PIN, INPUT_PULLUP);

    // 3. Initialize FastLED controller
    FastLED.addLeds<WS2812B, LED_PIN, GRB>(leds, NUM_LEDS);
    FastLED.setBrightness(DEFAULT_BRIGHT);
    FastLED.clear(true);
}

void loop() {
    static uint8_t animStep = 0;
    static int scrollOffset = -MATRIX_WIDTH;
    static unsigned long lastFrameTime = 0;
    const char* cardText = "SHMCANDI - ESP32-C6 SMART BUSINESS CARD V2 ";

    // Handle Button Press (Mode Switching & Long-Press Sleep)
    if (digitalRead(BTN_USER_PIN) == LOW) {
        if (millis() - lastButtonPress > 250) {
            lastButtonPress = millis();
            
            // Check for hold to trigger deep sleep
            delay(400);
            if (digitalRead(BTN_USER_PIN) == LOW) {
                enterDeepSleep();
            }

            // Otherwise, switch animation mode
            currentMode = (DisplayMode)((currentMode + 1) % MODE_COUNT);
            FastLED.clear(true);
            scrollOffset = -MATRIX_WIDTH;
        }
    }

    // Render frame at ~40 FPS
    if (millis() - lastFrameTime >= 25) {
        lastFrameTime = millis();

        switch (currentMode) {
            case MODE_RAINBOW:
                renderRainbow(animStep);
                animStep += 3;
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

        FastLED.show();
    }
}
