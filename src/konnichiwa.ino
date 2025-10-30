#include <Arduino.h>
#include <U8g2lib.h>
#include <WiFi.h> 
#include <WiFiClientSecure.h>
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
char serverUrl[128] = "https://quotes.muskeg.dev/quote"; // Full URL with protocol

// AP mode settings
const char* AP_SSID = "KonnichiwaSetup";
const char* AP_PASSWORD = "konnichiwa"; // Leave empty for open network or set a password
bool isAPMode = false;
WebServer webServer(80);
DNSServer dnsServer;
const byte DNS_PORT = 53;

unsigned long scrollMillis = 0;
unsigned long scrollInterval = 75; // smaller = faster
unsigned long apiCallInterval = 120000;  // Default: 2 minutes (in milliseconds)

// Preferences for storing configuration
Preferences preferences;

// Make sure MOSI/SCK are wired to your board's hardware SPI pins.
U8G2_MAX7219_32X8_F_4W_SW_SPI u8g2(U8G2_R0, CLK_PIN, DATA_PIN, CS_PIN, U8X8_PIN_NONE);

char textBuf[256] = "これを読めるのはちょっとムズ";
char nextBuf[256] = {0};
bool hasNextQuote = false;
bool invertDisplay = false; // when true: black text on light background
int textWidth = 0;
int xPos = 0;
int yPos = 7;
int displayWidth = 32;
int brightness = 1;
unsigned long lastScrollMs = 0;

// synchronization for safe cross-task updates
static SemaphoreHandle_t textMutex = NULL;
TaskHandle_t apiTaskHandle = NULL;

// Cookie storage
String storedCookies = "";

// Function declarations
bool loadConfiguration();
void saveConfiguration();
void setupAPMode();
void handleRoot();
void handleSave();
void handleNotFound();


// Function to extract and store cookies from response
void handleCookies(HTTPClient& http) {
  // Check if Set-Cookie header exists
  if (http.hasHeader("Set-Cookie")) {
    String cookieHeader = http.header("Set-Cookie");
    Serial.print("Received cookie: ");
    Serial.println(cookieHeader);
    
    // Parse the cookie (basic parsing - you might need more sophisticated parsing for multiple cookies)
    // Format: name=value; attributes...
    int semicolonPos = cookieHeader.indexOf(';');
    String cookieValue = (semicolonPos > 0) ? cookieHeader.substring(0, semicolonPos) : cookieHeader;
    cookieValue.trim();
    
    // Extract cookie name for comparison
    int equalsPos = cookieValue.indexOf('=');
    if (equalsPos > 0) {
      String newCookieName = cookieValue.substring(0, equalsPos);
      
      // Build new cookie string, replacing cookie with same name
      String newStoredCookies = "";
      bool cookieReplaced = false;
      
      if (storedCookies.length() > 0) {
        // Parse existing cookies separated by "; "
        int startPos = 0;
        while (startPos < storedCookies.length()) {
          int endPos = storedCookies.indexOf("; ", startPos);
          String existingCookie;
          
          if (endPos == -1) {
            // Last cookie
            existingCookie = storedCookies.substring(startPos);
            startPos = storedCookies.length();
          } else {
            existingCookie = storedCookies.substring(startPos, endPos);
            startPos = endPos + 2;
          }
          
          // Check if this cookie has the same name
          int existingEqualsPos = existingCookie.indexOf('=');
          if (existingEqualsPos > 0) {
            String existingCookieName = existingCookie.substring(0, existingEqualsPos);
            if (existingCookieName.equals(newCookieName)) {
              // Replace this cookie with the new one
              if (newStoredCookies.length() > 0) newStoredCookies += "; ";
              newStoredCookies += cookieValue;
              cookieReplaced = true;
            } else {
              // Keep this cookie
              if (newStoredCookies.length() > 0) newStoredCookies += "; ";
              newStoredCookies += existingCookie;
            }
          } else {
            // Keep malformed cookie
            if (newStoredCookies.length() > 0) newStoredCookies += "; ";
            newStoredCookies += existingCookie;
          }
        }
      }
      
      // If cookie wasn't replaced, add it as new
      if (!cookieReplaced) {
        if (newStoredCookies.length() > 0) newStoredCookies += "; ";
        newStoredCookies += cookieValue;
      }
      
      storedCookies = newStoredCookies;
    } else {
      // No equals sign, just store as-is (shouldn't happen with valid cookies)
      storedCookies = cookieValue;
    }
    
    // Save to preferences for persistence across reboots
    preferences.begin("konnichiwa", false);
    preferences.putString("cookies", storedCookies);
    preferences.end();
    
    Serial.print("Stored cookies: ");
    Serial.println(storedCookies);
  }
}

// Function to load cookies from preferences
void loadCookies() {
  preferences.begin("konnichiwa", false);
  storedCookies = preferences.getString("cookies", "");
  preferences.end();
  
  if (storedCookies.length() > 0) {
    Serial.print("Loaded cookies: ");
    Serial.println(storedCookies);
  }
}

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
    
    // Load stored cookies from flash
    loadCookies();
    
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
            // Parse the URL to detect protocol
            String url = String(serverUrl);
            bool isHttps = url.startsWith("https://");
            
            HTTPClient http;
            
            if (isHttps) {
              WiFiClientSecure secureClient;
              secureClient.setInsecure(); // Skip certificate validation (use for self-signed certs)
              secureClient.setTimeout(10000);
              
              Serial.print("API task connecting to: ");
              Serial.println(url);
              
              Serial.print("Protocol detected: ");
              Serial.println(isHttps ? "HTTPS" : "HTTP");

              if (http.begin(secureClient, url)) {
                http.setTimeout(10000);
                
                // Add standard headers for good API compatibility
                http.addHeader("User-Agent", "Konnichiwa-ESP32/1.0");
                http.addHeader("Accept", "application/json");
                http.addHeader("Connection", "keep-alive");
                
                // Try setting Host header without port if the URL includes one
                // Some servers might expect Host: hostname instead of Host: hostname:port
                int colonIndex = url.indexOf(":", url.indexOf("//") + 2);
                if (colonIndex > 0 && url.indexOf("/", colonIndex) > colonIndex) {
                  // Extract hostname from URL (between // and :)
                  int start = url.indexOf("//") + 2;
                  String hostname = url.substring(start, colonIndex);
                  http.addHeader("Host", hostname);
                  Serial.print("Setting Host header to: ");
                  Serial.println(hostname);
                }
                
                // Add cookies to the request if we have any
                if (storedCookies.length() > 0) {
                  http.addHeader("Cookie", storedCookies);
                  Serial.print("Sending cookies: ");
                  Serial.println(storedCookies);
                }
                
                // Tell HTTPClient to collect Set-Cookie headers
                const char* headers[] = {"Set-Cookie"};
                http.collectHeaders(headers, 1);
                
                int httpResponseCode = http.GET();
                Serial.print("API task HTTP code: ");
                Serial.println(httpResponseCode);

                if (httpResponseCode > 0) {
                  // Handle cookies from the response
                  handleCookies(http);
                  
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
              WiFiClient insecureClient;
              insecureClient.setTimeout(10000);
              
              Serial.print("API task connecting to: ");
              Serial.println(url);

              if (http.begin(insecureClient, url)) {
                http.setTimeout(10000);
                
                // Add standard headers for good API compatibility
                http.addHeader("User-Agent", "Konnichiwa-ESP32/1.0");
                http.addHeader("Accept", "application/json");
                http.addHeader("Connection", "keep-alive");
                
                // Try setting Host header without port if the URL includes one
                // Some servers might expect Host: hostname instead of Host: hostname:port
                int colonIndex = url.indexOf(":", url.indexOf("//") + 2);
                if (colonIndex > 0 && url.indexOf("/", colonIndex) > colonIndex) {
                  // Extract hostname from URL (between // and :)
                  int start = url.indexOf("//") + 2;
                  String hostname = url.substring(start, colonIndex);
                  http.addHeader("Host", hostname);
                  Serial.print("Setting Host header to: ");
                  Serial.println(hostname);
                }
                
                // Add cookies to the request if we have any
                if (storedCookies.length() > 0) {
                  http.addHeader("Cookie", storedCookies);
                  Serial.print("Sending cookies: ");
                  Serial.println(storedCookies);
                }
                
                // Tell HTTPClient to collect Set-Cookie headers
                const char* headers[] = {"Set-Cookie"};
                http.collectHeaders(headers, 1);
                
                int httpResponseCode = http.GET();
                Serial.print("API task HTTP code: ");
                Serial.println(httpResponseCode);

                if (httpResponseCode > 0) {
                  // Handle cookies from the response
                  handleCookies(http);
                  
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
      8192, // Increased stack size for HTTPS/SSL operations
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
        // check for config command: "/config" to enter AP mode for reconfiguration
        if (s.startsWith("/config")) {
          Serial.println("Entering configuration mode...");
          Serial.println("Stopping API task and restarting in AP mode");
          
          // Delete the API task if it exists
          if (apiTaskHandle != NULL) {
            vTaskDelete(apiTaskHandle);
            apiTaskHandle = NULL;
          }
          
          // Disconnect from WiFi
          WiFi.disconnect();
          
          // Enter AP mode
          setupAPMode();
        }
        // check for invert command: "/invert on", "/invert off", "/invert toggle"
        else if (s.startsWith("/invert")) {
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
        char queued[256];
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

    char localBuf[256];
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
    String storedUrl = preferences.getString("serverUrl", "https://quotes.muskeg.dev/quote");
    invertDisplay = preferences.getBool("invertDisplay", false);
    apiCallInterval = preferences.getULong("apiInterval", 120000); // Default: 2 minutes
    scrollInterval = preferences.getULong("scrollInterval", 75); // Default: 75ms
    
    // Copy values to our variables
    storedSsid.toCharArray(ssid, sizeof(ssid));
    storedPassword.toCharArray(password, sizeof(password));
    storedUrl.toCharArray(serverUrl, sizeof(serverUrl));
    
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
  preferences.putString("serverUrl", serverUrl);
  preferences.putBool("invertDisplay", invertDisplay);
  preferences.putULong("apiInterval", apiCallInterval);
  preferences.putULong("scrollInterval", scrollInterval);
  preferences.putBool("configured", true);
  // Optionally clear cookies when configuration changes
  // preferences.remove("cookies");
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
  String invertChecked = invertDisplay ? "checked" : "";
  // Convert milliseconds to seconds for display
  unsigned long apiIntervalSeconds = apiCallInterval / 1000;

  String html = "<!DOCTYPE html><html><head><title>Konnichiwa Setup</title>"
                "<meta name='viewport' content='width=device-width, initial-scale=1'>"
                "<style>"
                "body{font-family:Arial;margin:20px;background-color:#331111;color:#f3f3f3;} "
                "#config{max-width:600px;margin:auto;margin-top:5%;} "
                "label{display:block;margin:0 0 6px 0;font-weight:600;} "
                "input,label,button{box-sizing:border-box;} "
                "input[type=text],input[type=password],input[type=url],input[type=number]{width:100%;padding:8px;margin:0 0 12px 0;border-radius:6px;border:1px solid #4a2a2a;background:#2a1b1b;color:#f3f3f3;} "
                "input[type=checkbox]{display:inline-block;margin-right:8px;transform:translateY(2px);} "
                ".hint{font-size:0.9em;color:#dbdbdb;margin:6px 0 16px 0;line-height:1.2;} "
                "button{background:#841d00;color:white;font-size:16px;padding:12px 20px;border:none;cursor:pointer;border-radius:25px;display:block;width:100%;} "
                "@media (max-width:420px){#config{margin-top:8%;padding:0 10px;} button{padding:14px 10px;}}"
                "</style></head>"
                "<body><div id='config'><h1>Konnichiwa Setup</h1>"
                "<form action='/save' method='POST'>"
                "<label>WiFi SSID:</label><input type='text' name='ssid' value='" + String(ssid) + "' required>"
                "<label>WiFi Password:</label><input type='password' name='password' value='" + String(password) + "' required>"
                "<label>Server URL:</label><input type='url' name='url' value='" + String(serverUrl) + "' required placeholder='https://quotes.muskeg.dev/quote'>"
                "<div class='hint'>Include protocol (http:// or https://), host, port, and path<br>"
                "<img src='data:image/png;base64,iVBORw0KGgoAAAANSUhEUgAAACAAAAAgCAYAAABzenr0AAAACXBIWXMAAAsTAAALEwEAmpwYAAAAAXNSR0IArs4c6QAAAARnQU1BAACxjwv8YQUAAAHdSURBVHgBvVeLbYNADDVdoHSDG4ENwgjpBMkG6QbtBkknIBtEnaDdADoBbEA6gWuXQ3GR7wOc8qSnSPhvkzuTQSQQMaefLXFDLIiGmFvxldgRG+IX8SPLsiukAAU2xDOxx3mo2BaWgismHnE9jrZ7s6tuMR1ajO0GKRaYNrhMorh35VoSBjwzbxWDCy5Lil/ailgrPnMtAe2F2wn5Hm+J9Nbxp2AvZNKuVPwetdZrKJQuGfCPMI/0baTSOai0EJ4EKlmVC3tYCdRHwOBR5Q8wHK8aOhiO1bVorK8phqPd0/4dJIKnCxULa4dw3vEZTkK7S2oegVH0u2S3mfCpPDOcQNJKPdAK+nsJ7wW10Ixn4xA+pRwDD1x5fOUOdA6bDaQLXjpEHSfw7RC+QDrsHc+b8ZJxYXUS5OPg8b8bj2LfvvcKCxEIzshHRXkavhG3+H8HaK2zMiJoYXXrQPBKGplJsK3tjObk4gl+wXiYqfFJCHublLailZ4EysjgJ814upKdRHc4uYp4gPAIQmjRdc8oFRcwExHBjdTPFAcclOc8Kp5huNN/iI90Or5DIAGHqCM+k30DEVU41/MI26jKo4DDX3JtAidcs1vg7eM0NoEeb98EBlIBAyu50DNzKv4F8Mg9kvfoSM0AAAAASUVORK5CYII=' alt='GitHub' style='height:14px; margin-right:6px; vertical-align:middle;'>"
                "<a href='https://github.com/muskeg/quote-api' target='_blank' rel='noopener' style='color:#ffb8a0; text-decoration:none; display:inline-flex; align-items:center;'>"
                "Spin up your own quotes server</a></div>"
                "<label>API Poll Interval (seconds):</label><input type='number' name='interval' value='" + String(apiIntervalSeconds) + "' min='10' max='86400' required>"
                "<div class='hint'>How often to fetch new quotes (10 seconds to 24 hours)</div>"
                "<label>Scroll Speed (milliseconds):</label><input type='number' name='scrollspeed' value='" + String(scrollInterval) + "' min='10' max='500' required>"
                "<div class='hint'>How fast text scrolls (10ms = fast, 500ms = slow)</div>"
                "<label><input type='checkbox' name='invert' " + invertChecked + "> Invert Display (black text on light background)</label>"
                "<button type='submit'>Save Configuration</button>"
                "</form></div></body></html>";
  webServer.send(200, "text/html", html);
}

// Web server handler for saving configuration
void handleSave() {
  if (webServer.hasArg("ssid") && webServer.hasArg("password") && 
      webServer.hasArg("url") && webServer.hasArg("interval") && webServer.hasArg("scrollspeed")) {
    
    // Get form data
    webServer.arg("ssid").toCharArray(ssid, sizeof(ssid));
    webServer.arg("password").toCharArray(password, sizeof(password));
    webServer.arg("url").toCharArray(serverUrl, sizeof(serverUrl));
    invertDisplay = webServer.hasArg("invert"); // Checkbox is present only if checked
    
    // Parse interval from seconds to milliseconds
    unsigned long intervalSeconds = webServer.arg("interval").toInt();
    if (intervalSeconds < 10) intervalSeconds = 10; // Minimum 10 seconds
    if (intervalSeconds > 86400) intervalSeconds = 86400; // Maximum 24 hours
    apiCallInterval = intervalSeconds * 1000;
    
    // Parse scroll speed
    unsigned long scrollSpeedMs = webServer.arg("scrollspeed").toInt();
    if (scrollSpeedMs < 10) scrollSpeedMs = 10; // Minimum 10ms
    if (scrollSpeedMs > 500) scrollSpeedMs = 500; // Maximum 500ms
    scrollInterval = scrollSpeedMs;
    
    // Save to preferences
    saveConfiguration();
    
    // Show success and restart message
    String html = "<!DOCTYPE html><html><head><title>Configuration Saved</title>"
                  "<meta name='viewport' content='width=device-width, initial-scale=1'>"
                  "<style>body{font-family:Arial;margin:20px;background-color:#331111;color:#f3f3f3;text-align:center;} "
                  "#config{max-width:600px;margin:auto;margin-top:5%;} "
                  ".success{color:#841d00;font-size:24px;}</style></head>"
                  "<body><div id='config'><h1 class='success'>Configuration Saved!</h1>"
                  "<p>The device will restart and attempt to connect to the configured WiFi network.</p>"
                  "</div></body></html>";
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