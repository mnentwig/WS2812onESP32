#define NEOPIXEL
#include <Arduino.h>
#include <WiFi.h>
#ifdef NEOPIXEL
#include <Adafruit_NeoPixel.h>
Adafruit_NeoPixel pixels(30, 26, NEO_GRB + NEO_KHZ800);
#endif
#define LED 2
const char* ssid = "put SSID here";
const char* password = "put password here";
const static int WS2812_GPIO = 26;
WiFiServer server(/*HTTP port*/ 80);

static uint32_t ticksleep_tStart = 0;
void IRAM_ATTR ticksleep_init() {
    ticksleep_tStart = xthal_get_ccount();
}

void IRAM_ATTR ticksleep_sleep(uint32_t nTicks) {
    while (xthal_get_ccount() - ticksleep_tStart < nTicks) {
    };
    ticksleep_tStart += nTicks;
}

// writes nLEDs RGB values from dataRGB
// note: WS2812 expects "GRB" word order (R: bit 15:8; G: bit 23:16; B: bit 7:0; unused: bit 31:25)
void IRAM_ATTR driveWS2812(uint32_t* dataGRB, uint32_t nLEDs, uint32_t GPIO) {
    // see WS2812B datasheet
    const float t0H_s = 0.4e-6;
    const float t1H_s = 0.8e-6;
    const float t0L_s = 0.85e-6;
    const float t1L_s = 0.45e-6;

    // datasheet requires 50 us for reset but example hardware needed slightly more
    const float tReset_s = 60e-6;

    const float fSys_Hz = F_CPU;
    const uint32_t nTics0H = (uint32_t)(t0H_s * fSys_Hz + 0.5);
    const uint32_t nTics1H = (uint32_t)(t1H_s * fSys_Hz + 0.5);
    const uint32_t nTics0L = (uint32_t)(t0L_s * fSys_Hz + 0.5);
    const uint32_t nTics1L = (uint32_t)(t1L_s * fSys_Hz + 0.5);
    const uint32_t nTicsReset = (uint32_t)(tReset_s * fSys_Hz + 0.5);

    // === write all LEDs ===
    uint32_t* p = dataGRB;
    portDISABLE_INTERRUPTS();
    ticksleep_init();
    for (uint32_t ixLED = nLEDs; ixLED != 0; --ixLED) {
        uint32_t v = *(p++);
        for (uint32_t ixBit = 24; ixBit != 0; --ixBit) {
            // === write one bit ===
            int bitVal = v & 0x00800000;
            digitalWrite(GPIO, 1);
            if (bitVal) {
                ticksleep_sleep(nTics1H);
                digitalWrite(GPIO, 0);
                ticksleep_sleep(nTics1L);
            } else {
                ticksleep_sleep(nTics0H);
                digitalWrite(GPIO, 0);
                ticksleep_sleep(nTics0L);
            }
            v <<= 1;
        }  // for bit
    }      // for ixLED
    portENABLE_INTERRUPTS();

    // === drive reset pulse ===
    // note: not timing-sensitive if minimum duration is achieved.
    // could be removed from interrupts-off section and shared across all GPIOs
    // Note: GPIO state is already low at this point.
    ticksleep_sleep(nTicsReset);
}

static uint32_t grb = 0x000000;
void handleWifi() {
    WiFiClient client = server.available();  // Listen for incoming clients
    if (!client)
        return;

    unsigned long currentTime = millis();
    unsigned long previousTime = currentTime;
    const long timeoutTime = 2000;
    String header;
    Serial.println("New Client.");  // print a message out in the serial port
    int charsInLine = 0;

    // === while dealing with client ===
    while (true) {
        if (!client.connected()) break;

        // === timeout check ===
        currentTime = millis();
        if (currentTime > previousTime + timeoutTime) break;

        // === got data? ===
        if (!client.available()) continue;

        // === read one char ===
        char c = client.read();
        Serial.write(c);

        // === disregard any CR (using only newline for protocol) ===
        if (c == '\r')
            continue;

        // === add non-newline character (expecting more data) ===
        if (c != '\n') {
            header += c;
            ++charsInLine;
            continue;
        }

        // === newline and last row not empty: Expecting more data ===
        if (charsInLine) {
            header += c;
            charsInLine = 0;
            continue;
        }

        // === newline and last row empty: take actions (if requested by header via URL) ===
        if (header.indexOf("GET /LED/full") >= 0) {
            grb = 0xFFFFFF;
        } else if (header.indexOf("GET /LED/dim1") >= 0) {
            grb = 0x444444;
        } else if (header.indexOf("GET /LED/dim2") >= 0) {
            grb = 0x111111;
        } else if (header.indexOf("GET /LED/off") >= 0) {
            grb = 0x000000;
        }

        // === reply HTTP header ===
        client.println("HTTP/1.1 200 OK");
        client.println("Content-type:text/html");
        client.println("Connection: close");
        client.println();

        // === reply page ===
        client.println("<!DOCTYPE html><html>");
        client.println("<head><meta name=\"viewport\" content=\"width=device-width, initial-scale=1\">");
        client.println("<style>html { font-family: Helvetica; display: inline-block; margin: 0px auto; text-align: center;}");
        client.println(".button { background-color: #4CAF50; border: none; color: white; padding: 16px 40px;");
        client.println("text-decoration: none; font-size: 30px; margin: 2px; cursor: pointer;}");
        client.println("</style></head>");

        client.println("<body><h1>LED control</h1>");
        client.println("<p><a href=\"/LED/full\"><button class=\"button\">FULL</button></a>");
        client.println("<a href=\"/LED/dim1\"><button class=\"button\">MID</button></a>");
        client.println("<a href=\"/LED/dim2\"><button class=\"button\">LOW</button></a>");
        client.println("<a href=\"/LED/off\"><button class=\"button\">OFF</button></a></p>");
        client.println("</body></html>");

        // blank line terminates HTTP response
        client.println();
        break;  // done with client
    }           // while dealing with client

    client.stop();
    Serial.println("Client disconnected.");
}

void setup() {
    // put your setup code here, to run once:
    Serial.begin(115200);
    pinMode(LED, OUTPUT);
    pinMode(WS2812_GPIO, OUTPUT);
    WiFi.begin(ssid, password);
#ifdef NEOPIXEL
    pixels.begin();
#endif
}

unsigned long nextUpdate = 0;
static int lastGrb = 0x80000000;  // impossible value for startup
int tick = 0;
void loop() {
    if (WiFi.status() == WL_CONNECTED) {
        server.begin();
        handleWifi();
    }
    unsigned long currentTime = millis();
    if ((lastGrb != grb) || (currentTime > nextUpdate)) {
        Serial.print("tick ");
        Serial.print(tick++);
        Serial.print("\tIP address: ");
        Serial.println(WiFi.localIP());

        lastGrb = grb;
        nextUpdate = currentTime + 1000;

#ifndef NEOPIXEL
        // === storage for LED color info ==
        const uint32_t nLEDs = 30;
        uint32_t ledMem[nLEDs];

        for (size_t ix = 0; ix < nLEDs; ++ix)
            ledMem[ix] = ix < 3 ? grb : 0;
        driveWS2812(ledMem, nLEDs, WS2812_GPIO);
#else
        pixels.clear();  // Set all pixel colors to 'off'
        for (int i = 0; i < 30; i++)
            pixels.setPixelColor(i, grb);
        pixels.show();  // Send the updated pixel colors to the hardware.
#endif
    }
}