#include <Arduino.h>
#include <U8g2lib.h>
#include <WiFi.h> 
#include <HTTPClient.h>
#include <ArduinoJson.h>
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include <Preferences.h>
#include <WebServer.h>
#include <DNSServer.h>

#include "misakimincho.h" // Japanese font
#define CLK_PIN 14
#define CS_PIN 12
#define DATA_PIN 13

// Config default values - will be overwritten if stored in preferences
char ssid[32] = "";
char password[64] = "";
char serverHost[64] = "youki.home.muskegg.com";
uint16_t serverPort = 8080;
char serverPath[32] = "/quote";

// AP mode settings
const char* AP_SSID = "KonnichiwaSetup";
const char* AP_PASSWORD = "konnichiwa"; // Leave empty for open network or set a password
bool isAPMode = false;
WebServer webServer(80);
DNSServer dnsServer;
const byte DNS_PORT = 53;

unsigned long scrollMillis = 0;
unsigned long scrollInterval = 75; // smaller = faster
unsigned long apiCallInterval = 120000;  

// Preferences for storing configuration
Preferences preferences;

// Make sure MOSI/SCK are wired to your board's hardware SPI pins.
U8G2_MAX7219_32X8_F_4W_SW_SPI u8g2(U8G2_R0, CLK_PIN, DATA_PIN, CS_PIN, U8X8_PIN_NONE);

char textBuf[128] = "これを読めるのはちょっとムズ";
char nextBuf[128] = {0};
bool hasNextQuote = false;
bool invertDisplay = false; // when true: black text on color background
int textWidth = 0;
int xPos = 0;
int yPos = 7;
int displayWidth = 32;
int brightness = 1;
unsigned long lastScrollMs = 0;

// synchronization for safe cross-task updates
static SemaphoreHandle_t textMutex = NULL;
TaskHandle_t apiTaskHandle = NULL;

// Function declarations
bool loadConfiguration();
void saveConfiguration();
void setupAPMode();
void handleRoot();
void handleSave();
void handleNotFound();

void setup() {
  Serial.begin(115200);
  u8g2.begin();
  // Use 7px/8px Japanese font suitable for 32x8 matrix
  u8g2.setFont(misakimincho);
  u8g2.setContrast(brightness);
  displayWidth = u8g2.getDisplayWidth();
  yPos = u8g2.getDisplayHeight() - 1;
  textWidth = u8g2.getUTF8Width(textBuf);
  xPos = displayWidth; // start off-screen to the right
  Serial.println(F("Send a line over Serial to change text."));

  // Load configuration
  if (loadConfiguration()) {
    Serial.println("Configuration loaded successfully");
    
    // Try to connect to WiFi
    WiFi.begin(ssid, password);
    
    // Show connecting message on display
    sprintf(textBuf, "Connecting to %s...", ssid);
    textWidth = u8g2.getUTF8Width(textBuf);
    int connectingXPos = displayWidth; // Start off-screen
    
    // Wait up to 20 seconds for WiFi connection while scrolling message
    unsigned long startAttemptTime = millis();
    unsigned long lastScrollTime = 0;
    
    while (WiFi.status() != WL_CONNECTED && 
           millis() - startAttemptTime < 20000) {
      
      // Handle scrolling animation
      unsigned long currentTime = millis();
      if (currentTime - lastScrollTime > scrollInterval) {
        lastScrollTime = currentTime;
        
        // Scroll logic
        connectingXPos--;
        if (connectingXPos < -textWidth) {
          connectingXPos = displayWidth;
        }
        
        // Draw scrolling text
        u8g2.clearBuffer();
        u8g2.drawUTF8(connectingXPos, yPos, textBuf);
        u8g2.sendBuffer();
      }
      
      delay(10); // Small delay to prevent watchdog trigger
    }
    
    if (WiFi.status() == WL_CONNECTED) {
      Serial.println("Connected to WiFi");
      
      // Show "Connected" message with scrolling animation
      sprintf(textBuf, "Connected to %s", ssid);
      textWidth = u8g2.getUTF8Width(textBuf);
      connectingXPos = displayWidth;
      
      // Scroll "Connected" message for 2 seconds
      unsigned long connectedStartTime = millis();
      lastScrollTime = 0;
      
      while (millis() - connectedStartTime < 2000) {
        unsigned long currentTime = millis();
        if (currentTime - lastScrollTime > scrollInterval) {
          lastScrollTime = currentTime;
          
          // Scroll logic
          connectingXPos--;
          if (connectingXPos < -textWidth) {
            connectingXPos = displayWidth;
          }
          
          // Draw scrolling text
          u8g2.clearBuffer();
          u8g2.drawUTF8(connectingXPos, yPos, textBuf);
          u8g2.sendBuffer();
        }
        
        delay(10); // Small delay to prevent watchdog trigger
      }
    } else {
      Serial.println("Failed to connect to WiFi");
      setupAPMode();
    }
  } else {
    Serial.println("Failed to load configuration or no configuration exists");
    setupAPMode();
  }
  
  // create mutex
  textMutex = xSemaphoreCreateMutex();

  // Only create API task if not in AP mode
  if (!isAPMode) {
    // create API task on core 1 (adjust stack/priority as needed)
    xTaskCreatePinnedToCore(
      [](void* pvParameters){
        (void) pvParameters; // silence warning
        // API task body
        for (;;) {
          if (WiFi.isConnected()) {
            HTTPClient http;
            WiFiClient client;
            client.setTimeout(10000);
            String url = "http://" + String(serverHost) + ":" + String(serverPort) + serverPath;
            Serial.print("API task connecting to: ");
            Serial.println(url);

            if (http.begin(client, url)) {
              http.setTimeout(10000);
              int httpResponseCode = http.GET();
              Serial.print("API task HTTP code: ");
              Serial.println(httpResponseCode);

              if (httpResponseCode > 0) {
                String payload = http.getString();
                Serial.println("API payload:");
                Serial.println(payload);

                JsonDocument doc;
                DeserializationError err = deserializeJson(doc, payload);
                if (!err) {
                  String quote = doc["quote"].as<String>();
                  quote.trim();
                  if (quote.length() > 0) {
                    // queue the next quote under mutex; loop() will run a transition and swap it in
                    if (textMutex && xSemaphoreTake(textMutex, pdMS_TO_TICKS(2000)) == pdTRUE) {
                      quote.toCharArray(nextBuf, sizeof(nextBuf));
                      hasNextQuote = true;
                      xSemaphoreGive(textMutex);
                    }
                  }
                } else {
                  Serial.print("JSON parse error: ");
                  Serial.println(err.c_str());
                }
              } else {
                Serial.print("API task HTTP Error code: ");
                Serial.println(httpResponseCode);
              }
              http.end();
            } else {
              Serial.println("API task: HTTP begin failed");
            }
          } else {
            Serial.println("API task: WiFi not connected");
          }

          // sleep for configured interval
          vTaskDelay(pdMS_TO_TICKS(apiCallInterval));
        }
        vTaskDelete(NULL);
      },
      "apiTask",
      4096,
      NULL,
      1,
      &apiTaskHandle,
      1
    );
  }
}

void loop() {
  if (isAPMode) {
    // Handle DNS and web server requests
    dnsServer.processNextRequest();
    webServer.handleClient();
  }

  unsigned long loopMillis = millis();
  // Text display loop
  if (loopMillis - scrollMillis > scrollInterval) {
    scrollMillis = loopMillis;

    // Read a line from Serial to change displayed text
    if (Serial.available()) {
      String s = Serial.readStringUntil('\n');
      s.trim();
      if (s.length() > 0) {
        // check for invert command: "/invert on", "/invert off", "/invert toggle"
        if (s.startsWith("/invert")) {
          if (s.indexOf("on") >= 0) invertDisplay = true;
          else if (s.indexOf("off") >= 0) invertDisplay = false;
          else invertDisplay = !invertDisplay;
          Serial.print("invertDisplay = ");
          Serial.println(invertDisplay ? "ON" : "OFF");
        } else {
          // update shared text under mutex
          if (textMutex && xSemaphoreTake(textMutex, pdMS_TO_TICKS(2000)) == pdTRUE) {
            s.toCharArray(textBuf, sizeof(textBuf));
            textWidth = u8g2.getUTF8Width(textBuf);
            xPos = displayWidth;
            xSemaphoreGive(textMutex);
          }
        }
      }
    }

    // Work on scrolling and drawing; copy shared state under mutex to local variables
    // If an API-updated quote is queued, run a quick transition then swap buffers
    if (textMutex && xSemaphoreTake(textMutex, pdMS_TO_TICKS(100)) == pdTRUE) {
      if (hasNextQuote) {
        // copy queued text to local temp and clear the flag while holding the mutex
        char queued[128];
        memcpy(queued, nextBuf, sizeof(queued));
        hasNextQuote = false;
        xSemaphoreGive(textMutex);
        
        // simple box wipe: expand filled box from right->left, then erase from right->left
        int dh = u8g2.getDisplayHeight();
        int stepDelay = 70;

        // Phase 1: grow a filled box from the right edge until it covers full width
        for (int mask = 1; mask <= displayWidth; ++mask) {
          u8g2.clearBuffer();
          int x = displayWidth - mask; // left edge of the growing box
          u8g2.drawBox(x, 0, mask, dh);
          u8g2.sendBuffer();
          delay(stepDelay);
        }

        // brief pause with full coverage
        delay(stepDelay * 3);

        // Phase 2: clear from the right edge towards the left by shrinking the box width
        for (int mask = displayWidth; mask >= 0; --mask) {
          u8g2.clearBuffer();
          // keep box anchored at the left, shrinking its width to simulate clearing from right
          if (mask > 0) u8g2.drawBox(0, 0, mask, dh);
          u8g2.sendBuffer();
          delay(stepDelay);
        }

        // swap in the queued text under mutex
        if (textMutex && xSemaphoreTake(textMutex, pdMS_TO_TICKS(2000)) == pdTRUE) {
          memcpy(textBuf, queued, sizeof(textBuf));
          textWidth = u8g2.getUTF8Width(textBuf);
          xPos = displayWidth;
          xSemaphoreGive(textMutex);
        }
      } else {
        xSemaphoreGive(textMutex);
      }
    }

    char localBuf[128];
    int localTextWidth;
    int localXPos;
    if (textMutex && xSemaphoreTake(textMutex, pdMS_TO_TICKS(100)) == pdTRUE) {
      memcpy(localBuf, textBuf, sizeof(textBuf));
      localTextWidth = textWidth;
      localXPos = xPos;
      // update global xPos as we will write it back after computing new position
      xSemaphoreGive(textMutex);
    } else {
      // fallback: use globals without lock if mutex unavailable
      memcpy(localBuf, textBuf, sizeof(textBuf));
      localTextWidth = textWidth;
      localXPos = xPos;
    }

    // Scroll text if it's wider than the display, otherwise center it
    if (localTextWidth > displayWidth) {
      localXPos--;
      if (localXPos < -localTextWidth) localXPos = displayWidth;
    } else {
      // center short text
      localXPos = (displayWidth - localTextWidth) / 2;
    }

    // write back updated xPos under mutex
    if (textMutex && xSemaphoreTake(textMutex, pdMS_TO_TICKS(100)) == pdTRUE) {
      xPos = localXPos;
      xSemaphoreGive(textMutex);
    } else {
      xPos = localXPos;
    }

    u8g2.clearBuffer();
    // Use the Japanese font when drawing UTF-8
    u8g2.setFont(misakimincho);
    if (invertDisplay) {
      // draw white background then draw text in black
      u8g2.setDrawColor(1); // background ON
      u8g2.drawBox(0, 0, displayWidth, u8g2.getDisplayHeight());
      u8g2.setDrawColor(0); // text OFF on the lit background
      u8g2.drawUTF8(localXPos, yPos, localBuf);
      u8g2.setDrawColor(1); // reset to default
    } else {
      // normal: black background (cleared) and lit text
      u8g2.setDrawColor(1);
      u8g2.drawUTF8(localXPos, yPos, localBuf);
    }
    u8g2.sendBuffer();
  }

  // no blocking HTTP/network code in loop anymore
}

// Load configuration from Preferences
bool loadConfiguration() {
  preferences.begin("konnichiwa", false); // false = read/write mode
  
  // Check if configuration exists
  if (preferences.isKey("configured")) {
    String storedSsid = preferences.getString("ssid", "");
    String storedPassword = preferences.getString("password", "");
    String storedHost = preferences.getString("serverHost", "youki.home.muskegg.com");
    uint16_t storedPort = preferences.getUShort("serverPort", 8080);
    String storedPath = preferences.getString("serverPath", "/quote");
    
    // Copy values to our variables
    storedSsid.toCharArray(ssid, sizeof(ssid));
    storedPassword.toCharArray(password, sizeof(password));
    storedHost.toCharArray(serverHost, sizeof(serverHost));
    serverPort = storedPort;
    storedPath.toCharArray(serverPath, sizeof(serverPath));
    
    preferences.end();
    return storedSsid.length() > 0; // Consider configuration valid if SSID is set
  }
  
  preferences.end();
  return false; // No configuration found
}

// Save configuration to Preferences
void saveConfiguration() {
  preferences.begin("konnichiwa", false);
  preferences.putString("ssid", ssid);
  preferences.putString("password", password);
  preferences.putString("serverHost", serverHost);
  preferences.putUShort("serverPort", serverPort);
  preferences.putString("serverPath", serverPath);
  preferences.putBool("configured", true);
  preferences.end();
}

// Set up AP mode and web server
void setupAPMode() {
  isAPMode = true;
  Serial.println("Starting AP Mode");
  
  // Set up the display with AP mode message
  strcpy(textBuf, "AP Mode: KonnichiwaSetup");
  textWidth = u8g2.getUTF8Width(textBuf);
  xPos = displayWidth;
  u8g2.clearBuffer();
  u8g2.drawUTF8(0, yPos, textBuf);
  u8g2.sendBuffer();
  
  // Start AP
  WiFi.softAP(AP_SSID, AP_PASSWORD);
  IPAddress myIP = WiFi.softAPIP();
  Serial.print("AP IP address: ");
  Serial.println(myIP);
  
  // Set up captive portal
  dnsServer.start(DNS_PORT, "*", myIP);
  
  // Set up web server routes
  webServer.on("/", HTTP_GET, handleRoot);
  webServer.on("/save", HTTP_POST, handleSave);
  webServer.onNotFound(handleNotFound);
  webServer.begin();
  
  Serial.println("HTTP server started");
}

// Web server handler for root page
void handleRoot() {
  String html = "<!DOCTYPE html><html><head><title>Konnichiwa Setup</title>"
                "<meta name='viewport' content='width=device-width, initial-scale=1'>"
                "<style>body{font-family:Arial;margin:20px;} input,label{display:block;margin-bottom:10px;} "
                "input[type=text],input[type=password],input[type=number]{width:100%;padding:8px;box-sizing:border-box;}"
                "button{background:#4CAF50;color:white;padding:10px;border:none;cursor:pointer;}</style></head>"
                "<body><h1>Konnichiwa Setup</h1>"
                "<form action='/save' method='POST'>"
                "<label>WiFi SSID:</label><input type='text' name='ssid' value='" + String(ssid) + "' required>"
                "<label>WiFi Password:</label><input type='password' name='password' value='" + String(password) + "' required>"
                "<label>Server Host:</label><input type='text' name='host' value='" + String(serverHost) + "' required>"
                "<label>Server Port:</label><input type='number' name='port' value='" + String(serverPort) + "' required>"
                "<label>Server Path:</label><input type='text' name='path' value='" + String(serverPath) + "' required>"
                "<button type='submit'>Save Configuration</button>"
                "</form></body></html>";
  webServer.send(200, "text/html", html);
}

// Web server handler for saving configuration
void handleSave() {
  if (webServer.hasArg("ssid") && webServer.hasArg("password") && 
      webServer.hasArg("host") && webServer.hasArg("port") && webServer.hasArg("path")) {
    
    // Get form data
    webServer.arg("ssid").toCharArray(ssid, sizeof(ssid));
    webServer.arg("password").toCharArray(password, sizeof(password));
    webServer.arg("host").toCharArray(serverHost, sizeof(serverHost));
    serverPort = webServer.arg("port").toInt();
    webServer.arg("path").toCharArray(serverPath, sizeof(serverPath));
    
    // Save to preferences
    saveConfiguration();
    
    // Show success and restart message
    String html = "<!DOCTYPE html><html><head><title>Configuration Saved</title>"
                  "<meta name='viewport' content='width=device-width, initial-scale=1'>"
                  "<style>body{font-family:Arial;margin:20px;text-align:center;} "
                  ".success{color:green;}</style></head>"
                  "<body><h1 class='success'>Configuration Saved!</h1>"
                  "<p>The device will restart and attempt to connect to the configured WiFi network.</p>"
                  "</body></html>";
    webServer.send(200, "text/html", html);
    
    // Short delay then restart
    delay(3000);
    ESP.restart();
  } else {
    webServer.send(400, "text/plain", "Missing required fields");
  }
}

// Captive portal - redirect all requests to configuration page
void handleNotFound() {
  webServer.sendHeader("Location", "http://" + WiFi.softAPIP().toString(), true);
  webServer.send(302, "text/plain", "");
}