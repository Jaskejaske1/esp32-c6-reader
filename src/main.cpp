#include <Arduino.h>
#include <Wire.h>
#include <U8g2lib.h>

// --- Hardware ---------------------------------------------------------------
#define I2C_SDA 8
#define I2C_SCL 10
#define OLED_ADDRESS 0x3C
U8G2_SSD1309_128X64_NONAME0_F_HW_I2C display(U8G2_R0, U8X8_PIN_NONE, OLED_ADDRESS);

#define BTN_PLAY_PAUSE 4
#define BTN_SPEED_UP 5
#define BTN_SPEED_DOWN 6

// Replace this with text received from Serial, SD, Wi-Fi, etc. later.
const char words[][20] PROGMEM = {
    "Welcome", "to", "the", "RSVP", "reader", "built", "with", "an",
    "ESP32-C6", "and", "this", "OLED", "screen.", "Let's", "start",
    "reading", "quickly,", "one", "word", "at", "a", "time!"};
constexpr uint16_t TOTAL_WORDS = sizeof(words) / sizeof(words[0]);

// --- Reader state -----------------------------------------------------------
uint16_t currentIndex = 0;
uint16_t wpm = 250;
bool isReading = false;
uint32_t nextWordAt = 0;

struct Button
{
    uint8_t pin;
    bool stable = HIGH;
    bool previousRaw = HIGH;
    uint32_t changedAt = 0;

    // Returns true exactly once per physical press.
    bool pressed()
    {
        const bool raw = digitalRead(pin);
        const uint32_t now = millis();

        if (raw != previousRaw)
        {
            previousRaw = raw;
            changedAt = now;
        }
        if ((now - changedAt) >= 30 && raw != stable)
        {
            stable = raw;
            return stable == LOW;
        }
        return false;
    }
};

Button playButton{BTN_PLAY_PAUSE};
Button upButton{BTN_SPEED_UP};
Button downButton{BTN_SPEED_DOWN};

void wordAt(uint16_t index, char *target, size_t targetSize)
{
    strncpy_P(target, words[index], targetSize - 1);
    target[targetSize - 1] = '\0';
}

// RSVP is easier to follow when punctuation and longer words get slightly more
// time. This is deliberately bounded, so the selected WPM still feels honest.
uint32_t delayFor(const char *word)
{
    float multiplier = 1.0f;
    const size_t length = strlen(word);
    if (length >= 9)
        multiplier += 0.20f;
    else if (length >= 7)
        multiplier += 0.10f;

    const char last = length ? word[length - 1] : '\0';
    if (last == ',' || last == ';' || last == ':')
        multiplier += 0.35f;
    if (last == '.' || last == '!' || last == '?')
        multiplier += 0.85f;

    return (uint32_t)(60000.0f / wpm * multiplier);
}

uint8_t orpIndex(const char *word)
{
    // Spritz-like optimal recognition point: slightly left of centre.
    const uint8_t n = strlen(word);
    if (n <= 1)
        return 0;
    if (n <= 5)
        return 1;
    if (n <= 9)
        return 2;
    return 3;
}

void drawPlayIcon(int x, int y)
{
    display.drawTriangle(x, y, x, y + 8, x + 7, y + 4);
}

void drawPauseIcon(int x, int y)
{
    display.drawBox(x, y, 2, 8);
    display.drawBox(x + 5, y, 2, 8);
}

void drawWord(const char *word)
{
    const uint8_t focus = orpIndex(word);
    char before[20] = {};
    // `focus` is at most 3 (see orpIndex), so it always fits this buffer.
    strncpy(before, word, focus);
    char focusChar[2] = {word[focus], '\0'};

    // Select the largest font whose *actual ORP-positioned bounds* fit. Checking
    // total width alone is not enough: RSVP intentionally centres the focus
    // character, which makes the rest of an uneven word extend to one side.
    const uint8_t *const wordFonts[] = {
        u8g2_font_helvB14_tf,
        u8g2_font_helvB12_tf,
        u8g2_font_helvB10_tf,
        u8g2_font_6x10_tf,
        u8g2_font_5x8_tf,
    };
    for (const uint8_t *font : wordFonts)
    {
        display.setFont(font);
        const int beforeWidth = display.getStrWidth(before);
        const int focusWidth = display.getStrWidth(focusChar);
        const int x = 64 - beforeWidth - focusWidth / 2;
        const int right = x + display.getStrWidth(word);
        if (x >= 2 && right <= 126)
            break; // preserve a 2 px safety margin
    }

    // Centre the ORP, not the complete word. This keeps the eye at a constant
    // x-position while words change length.
    const int beforeWidth = display.getStrWidth(before);
    const int focusWidth = display.getStrWidth(focusChar);
    const int anchor = 64;
    const int desiredX = anchor - beforeWidth - focusWidth / 2;
    const int wordWidth = display.getStrWidth(word);
    // A pathological very long word can still be too asymmetric at the smallest
    // font. In that one case, favour showing the complete word over a perfectly
    // centred ORP.
    const int x = constrain(desiredX, 2, 126 - wordWidth);
    // Keep the word vertically centred in the reading area as its font changes.
    const int y = 34 + (display.getAscent() - display.getDescent()) / 2;

    display.setCursor(x, y);
    display.print(word);

    // The inverse character is the visual fixation point on a monochrome OLED.
    const int focusX = x + beforeWidth;
    display.setDrawColor(1);
    const int focusY = y - display.getAscent() - 1;
    const int focusHeight = display.getAscent() - display.getDescent() + 2;
    display.drawBox(focusX, focusY, focusWidth, focusHeight);
    display.setDrawColor(0);
    display.setCursor(focusX, y);
    display.print(focusChar);
    display.setDrawColor(1);
}

void render()
{
    char word[20];
    wordAt(currentIndex, word, sizeof(word));

    display.clearBuffer();
    display.setFontMode(1);

    // Header
    display.setFont(u8g2_font_6x10_tf);
    display.setCursor(11, 9);
    display.print(isReading ? "READING" : "PAUSED");
    if (isReading)
        drawPauseIcon(1, 1);
    else
        drawPlayIcon(1, 1);

    char speed[12];
    snprintf(speed, sizeof(speed), "%u WPM", wpm);
    display.setCursor(128 - display.getStrWidth(speed), 9);
    display.print(speed);
    display.drawHLine(0, 12, 128);

    drawWord(word);

    // Footer: phrase position and a proper outlined progress bar.
    display.setFont(u8g2_font_5x7_tf);
    char position[16];
    snprintf(position, sizeof(position), "%u/%u", currentIndex + 1, TOTAL_WORDS);
    display.setCursor(0, 58);
    display.print(position);
    display.drawFrame(28, 55, 100, 8);
    const uint8_t fill = ((currentIndex + 1) * 98UL) / TOTAL_WORDS;
    if (fill)
        display.drawBox(29, 56, fill, 6);

    display.sendBuffer();
}

void setReading(bool reading)
{
    isReading = reading;
    if (isReading)
    {
        char word[20];
        wordAt(currentIndex, word, sizeof(word));
        nextWordAt = millis() + delayFor(word);
    }
    render();
}

void setup()
{
    Serial.begin(115200);

    Wire.begin(I2C_SDA, I2C_SCL);
    display.begin();
    display.setPowerSave(0);

    pinMode(BTN_PLAY_PAUSE, INPUT_PULLUP);
    pinMode(BTN_SPEED_UP, INPUT_PULLUP);
    pinMode(BTN_SPEED_DOWN, INPUT_PULLUP);

    render();
    Serial.println("RSVP reader ready: Play/Pause, +WPM, -WPM");
}

void loop()
{
    if (playButton.pressed())
    {
        // At the end, starting again begins a new pass.
        if (!isReading && currentIndex == TOTAL_WORDS - 1)
            currentIndex = 0;
        setReading(!isReading);
    }

    if (upButton.pressed() && wpm < 800)
    {
        wpm = min<uint16_t>(800, wpm + 25);
        render();
    }
    if (downButton.pressed() && wpm > 50)
    {
        wpm = max<uint16_t>(50, wpm - 25);
        render();
    }

    if (isReading && (int32_t)(millis() - nextWordAt) >= 0)
    {
        if (++currentIndex >= TOTAL_WORDS)
        {
            currentIndex = TOTAL_WORDS - 1; // show the last word while paused
            setReading(false);
            Serial.println("Finished reading.");
        }
        else
        {
            char word[20];
            wordAt(currentIndex, word, sizeof(word));
            nextWordAt = millis() + delayFor(word);
            render();
        }
    }
}