/*
 * Kode Dot PTT (Push-to-Talk) App
 * ------------------------------------------------------------
 * Purpose:
 * - Implements the logic of 'client.py' on Kode Dot hardware.
 * - Uses the 'Base Project' as a template for hardware initialization
 * (Display, LVGL, LED, I/O Expander).
 *
 * Functionality:
 * - Connects to WiFi and authenticates with the server to obtain a token.
 * - Connects to the WebSocket server for PTT.
 * - Uses BUTTON A as a physical "Hold-to-Talk" (PTT) button.
 * - Captures 16kHz/16-bit audio from I2S mic while PTT is pressed.
 * - Sends audio as WebSocket binary messages.
 * - Receives binary audio from WebSocket and plays it on I2S speaker.
 * - Displays status (Connecting, Ready, Talking, Incoming) on LVGL screen.
 * - Uses RGB LED to indicate status (Green=Talking, Orange=Incoming).
 */

// =================================================================
// --- Template Include Headers ---
// =================================================================
#include <Arduino.h>
#include <lvgl.h>
#include <kodedot/display_manager.h>
#include <TCA9555.h>
#include <kodedot/pin_config.h>
#include <Adafruit_NeoPixel.h>

// Fonts are compiled as separate C translation units in src/fonts/
extern "C" {
    extern const lv_font_t Inter_20;
    extern const lv_font_t Inter_30;
    extern const lv_font_t Inter_40;
}

// =================================================================
// --- Includes Added for PTT ---
// =================================================================
#include <WiFi.h>
#include <ArduinoHttpClient.h>
#include <WebSocketsClient.h>
#include <ArduinoJson.h>
#include "driver/i2s.h"
#include <SD_MMC.h>
#include <FS.h>

// =================================================================
// --- Font References (from your project) ---
// =================================================================
// NOTE: Font .c files live in src/fonts/ and are compiled separately by
// the build system. Do NOT #include the .c files here (causes duplicate
// symbol / redefinition errors). Refer to the font objects (e.g. Inter_20)
// directly in code; they will be linked from the separate compilation units.

// =================================================================
// --- Configuration Secrets ---
// =================================================================
// --- WiFi (will be read from SD: Wi-Fi.json) ---
String WIFI_SSID = "";
String WIFI_PASS = "";

// --- Server Credentials ---
// (Device MAC will be used as USERNAME and PASSWORD)
String USERNAME = "";  // Will be filled with MAC
String PASSWORD = "";  // Will be filled with MAC
String FRIENDLY_NAME = "Kode_Dot_PTT"; // Will be read from General/PTT.json

// =================================================================
// --- Server and Client Configuration ---
// (Extracted from client.py)
// =================================================================
// Default endpoint (can be overridden by /General/PTT.json)
String SERVER_ENDPOINT = "http://192.168.178.4:8000";
String server_host_str = "192.168.178.4";
int server_port_int = 8000;
const unsigned long KEEPALIVE_MS = 20000;     // 20s ws keepalive ping
const unsigned long AUDIO_DECAY_MS = 1200;    // how long to keep "incoming" visible

// =================================================================
// --- Audio Configuration (from client.py) ---
// =================================================================
const int SAMPLE_RATE = 16000;
const int BITS_PER_SAMPLE = 16;
// 512 bytes per chunk / 2 bytes per sample = 256 samples
const int AUDIO_BUFFER_SAMPLES = 256;
// Buffer size in bytes
const int I2S_READ_BUFFER_BYTES = AUDIO_BUFFER_SAMPLES * (BITS_PER_SAMPLE / 8);
// Buffer to read from microphone
int16_t i2s_read_buffer[AUDIO_BUFFER_SAMPLES];

// =================================================================
// --- Global State Variables ---
// =================================================================
// --- Network ---
WiFiClient wifiClient;
// httpClient will be initialized at runtime after reading PTT.json endpoint
HttpClient *httpClient = nullptr;
WebSocketsClient webSocket;
String globalToken;    // Authentication token
String globalDeviceId; // ID of this device
bool isWebSocketConnected = false;
unsigned long lastPingTime = 0;

// --- PTT State ---
// 'volatile' is critical because these variables are modified
// by tasks and read by others.
volatile bool isPttActive = false;
volatile bool pttStateChanged = false; // Flag to notify main loop

// --- Incoming Audio State ---
volatile unsigned long lastAudioReceiveTime = 0;
volatile bool isReceivingAudio = false;

// --- Hardware Kode Dot ---
// Create TCA9555 with address from BSP config
TCA9555 io_expander(IOEXP_I2C_ADDR);
// NeoPixel (use BSP defines)
Adafruit_NeoPixel led_strip(NEO_PIXEL_COUNT, NEO_PIXEL_PIN, LED_STRIP_COLOR_ORDER + LED_STRIP_TIMING);

// Display manager instance
DisplayManager displayManager;

// --- UI (LVGL) ---
lv_obj_t *lblStatus;
lv_obj_t *lblPttStatus;
lv_obj_t *lblIncomingStatus;

// =================================================================
// --- LED Helper Functions (from template main.cpp) ---
// =================================================================
void led_setup()
{
    led_strip.begin();
    led_strip.setBrightness(20); // Brillo (0-255)
    led_strip.clear();
    led_strip.show();
}

void led_set_rgb(uint8_t r, uint8_t g, uint8_t b)
{
    led_strip.setPixelColor(0, led_strip.Color(r, g, b));
}

void led_show()
{
    led_strip.show();
}

// =================================================================
// --- SD Card and Configuration Functions ---
// =================================================================

String getDeviceMAC()
{
    Serial.println("Getting device MAC address...");
    uint8_t mac[6];
    WiFi.macAddress(mac);
    
    char macStr[18];
    snprintf(macStr, sizeof(macStr), "%02X%02X%02X%02X%02X%02X",
             mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
    
    String result(macStr);
    Serial.printf("MAC: %s\n", result.c_str());
    return result;
}

bool readOrCreatePTTConfig()
{
    Serial.println("Reading PTT.json from General/...");
    lv_label_set_text(lblStatus, "Reading PTT.json...");
    displayManager.update();

    // Create General folder if it doesn't exist
    if (!SD_MMC.exists("/General"))
    {
        Serial.println("Creating General folder...");
        SD_MMC.mkdir("/General");
    }

    // Read PTT.json if it exists
    if (SD_MMC.exists("/General/PTT.json"))
    {
        File file = SD_MMC.open("/General/PTT.json", FILE_READ);
        if (file)
        {
            String content = "";
            while (file.available())
            {
                content += (char)file.read();
            }
            file.close();

            JsonDocument doc;
            DeserializationError error = deserializeJson(doc, content);
            if (!error)
            {
                if (doc.containsKey("Friendly_Name"))
                {
                    FRIENDLY_NAME = doc["Friendly_Name"].as<String>();
                    Serial.printf("Friendly Name read: %s\n", FRIENDLY_NAME.c_str());
                }
                if (doc.containsKey("Endpoint"))
                {
                    SERVER_ENDPOINT = doc["Endpoint"].as<String>();
                    Serial.printf("Endpoint read: %s\n", SERVER_ENDPOINT.c_str());

                    // Parse host and port from endpoint (simple parser)
                    String s = SERVER_ENDPOINT;
                    if (s.startsWith("http://")) s = s.substring(7);
                    else if (s.startsWith("https://")) s = s.substring(8);
                    int slash = s.indexOf('/');
                    if (slash != -1) s = s.substring(0, slash);
                    int colon = s.indexOf(':');
                    if (colon != -1)
                    {
                        server_host_str = s.substring(0, colon);
                        server_port_int = s.substring(colon + 1).toInt();
                    }
                    else
                    {
                        server_host_str = s;
                        server_port_int = 80;
                    }

                    // Initialize httpClient if not yet created
                    if (httpClient == nullptr)
                    {
                        httpClient = new HttpClient(wifiClient, server_host_str.c_str(), server_port_int);
                        Serial.printf("HttpClient initialized: %s:%d\n", server_host_str.c_str(), server_port_int);
                    }
                }
                return true;
            }
        }
    }

    // If it doesn't exist or there's an error, create with default value
    Serial.println("Creating PTT.json with default value...");
    FRIENDLY_NAME = "Kode_Dot_PTT";
    
    JsonDocument doc;
    doc["Friendly_Name"] = FRIENDLY_NAME;
    doc["Endpoint"] = SERVER_ENDPOINT;
    
    File file = SD_MMC.open("/General/PTT.json", FILE_WRITE);
    if (file)
    {
        serializeJson(doc, file);
        file.close();
        Serial.println("PTT.json created successfully");
        Serial.printf("PTT.json default Endpoint: %s\n", SERVER_ENDPOINT.c_str());
        return true;
    }

    Serial.println("ERROR: Could not create PTT.json");
    return false;
}

// =================================================================
// --- PTT Configuration Functions ---
// =================================================================

void setupI2S()
{
    Serial.println("Configuring I2S...");
    lv_label_set_text(lblStatus, "I2S: Configuring...");
    displayManager.update();

    i2s_config_t i2s_config = {
        .mode = (i2s_mode_t)(I2S_MODE_MASTER | I2S_MODE_TX | I2S_MODE_RX),
        .sample_rate = SAMPLE_RATE,
        .bits_per_sample = I2S_BITS_PER_SAMPLE_16BIT,
        .channel_format = I2S_CHANNEL_FMT_ONLY_RIGHT, // Mono (depends on mic)
        .communication_format = I2S_COMM_FORMAT_STAND_I2S,
        .intr_alloc_flags = ESP_INTR_FLAG_LEVEL1,
        .dma_buf_count = 8,
        .dma_buf_len = 64,
        .use_apll = false,
        .tx_desc_auto_clear = true
    };

    // Use pins defined in kodedot/pin_config.h
    i2s_pin_config_t pin_config = {
        .bck_io_num = MIC_I2S_SCK,
        .ws_io_num = MIC_I2S_WS,
        // Speaker pin not defined in BSP; leave as -1 if unused
        .data_out_num = -1, // Speaker (not defined in BSP)
        .data_in_num = MIC_I2S_DIN     // Microphone
    };

    lv_label_set_text(lblStatus, "I2S: Driver...");
    displayManager.update();
    i2s_driver_install(I2S_NUM_0, &i2s_config, 0, NULL);
    
    lv_label_set_text(lblStatus, "I2S: Pins...");
    displayManager.update();
    i2s_set_pin(I2S_NUM_0, &pin_config);
    
    lv_label_set_text(lblStatus, "I2S: Clock...");
    displayManager.update();
    i2s_set_clk(I2S_NUM_0, SAMPLE_RATE, I2S_BITS_PER_SAMPLE_16BIT, I2S_CHANNEL_MONO);

    Serial.println("I2S configured.");
    lv_label_set_text(lblStatus, "I2S: OK");
    displayManager.update();
    delay(500);
}

void setupWifi()
{
    lv_label_set_text(lblStatus, "WiFi: Reading networks...");
    displayManager.update();
    Serial.println("WiFi: Reading /Wi-Fi.json...");

    // Check if file exists
    if (!SD_MMC.exists("/Wi-Fi.json"))
    {
        Serial.println("ERROR: /Wi-Fi.json not found on SD card");
        lv_label_set_text(lblStatus, "ERROR: No Wi-Fi.json");
        displayManager.update();
        return;
    }

    // Read the JSON file
    File file = SD_MMC.open("/Wi-Fi.json", FILE_READ);
    if (!file)
    {
        Serial.println("ERROR: Cannot open /Wi-Fi.json");
        lv_label_set_text(lblStatus, "ERROR: Cannot open WiFi.json");
        displayManager.update();
        return;
    }

    String content = "";
    while (file.available())
    {
        content += (char)file.read();
    }
    file.close();

    Serial.println("Raw JSON content:");
    Serial.println(content);

    // Parse JSON
    JsonDocument doc;
    DeserializationError error = deserializeJson(doc, content);

    if (error)
    {
        Serial.print("JSON parse error: ");
        Serial.println(error.c_str());
        lv_label_set_text(lblStatus, "ERROR: JSON parse failed");
        displayManager.update();
        return;
    }

    if (!doc.is<JsonArray>())
    {
        Serial.println("ERROR: /Wi-Fi.json is not a JSON array");
        lv_label_set_text(lblStatus, "ERROR: WiFi.json not array");
        displayManager.update();
        return;
    }

    // Get array and count networks
    JsonArray networks = doc.as<JsonArray>();
    int totalNets = networks.size();
    
    if (totalNets == 0)
    {
        Serial.println("ERROR: No networks in /Wi-Fi.json");
        lv_label_set_text(lblStatus, "ERROR: No networks found");
        displayManager.update();
        return;
    }

    Serial.printf("Found %d WiFi networks\n", totalNets);

    // Attempt to connect to each network
    for (int i = 0; i < totalNets; i++)
    {
        JsonObject net = networks[i];
        String ssid = net["ssid"].as<String>();
        String pass = net["pass"].as<String>();

        Serial.printf("[WiFi %d/%d] SSID: '%s', Pass: '%s'\n", i + 1, totalNets, ssid.c_str(), pass.c_str());

        // Display progress: "Connecting... 1/X"
        String status = "Connecting... " + String(i + 1) + "/" + String(totalNets);
        lv_label_set_text(lblStatus, status.c_str());
        displayManager.update();

        Serial.printf("[WiFi %d/%d] Attempting: %s\n", i + 1, totalNets, ssid.c_str());

        WiFi.begin(ssid.c_str(), pass.c_str());

        // Wait up to 15 seconds per attempt
        int attempts = 0;
        while (WiFi.status() != WL_CONNECTED && attempts < 30)
        {
            delay(500);
            Serial.print(".");
            attempts++;
        }

        if (WiFi.status() == WL_CONNECTED)
        {
            Serial.println("\n✓ WiFi connected!");
            Serial.printf("SSID: %s\n", ssid.c_str());
            Serial.printf("IP: %s\n", WiFi.localIP().toString().c_str());

            // Display IP on screen
            String ipStatus = "WiFi: " + WiFi.localIP().toString();
            lv_label_set_text(lblStatus, ipStatus.c_str());
            displayManager.update();
            delay(1000);
            return;
        }

        Serial.println();
        Serial.printf("✗ Could not connect to: %s\n", ssid.c_str());
    }

    // If we get here, failed to connect to any network
    Serial.println("ERROR: Could not connect to any WiFi network");
    lv_label_set_text(lblStatus, "ERROR: WiFi not connected");
    displayManager.update();
}

bool tryRegisterUser()
{
    Serial.println("[REGISTER] Attempting to register new user...");
    lv_label_set_text(lblStatus, "Registering user...");
    displayManager.update();

    JsonDocument doc;
    doc["username"] = USERNAME;
    doc["password"] = PASSWORD;
    doc["friendlyName"] = FRIENDLY_NAME;
    
    String jsonStr;
    serializeJson(doc, jsonStr);
    
    Serial.println("Sending registration:");
    Serial.println(jsonStr);
    displayManager.update();
    
    httpClient->post("/register", "application/json", jsonStr);
    int statusCode = httpClient->responseStatusCode();
    String responseBody = httpClient->responseBody();

    Serial.printf("[REGISTER] Status: %d\n", statusCode);
    Serial.println(responseBody);

    if (statusCode == 200 || statusCode == 201)
    {
        Serial.println("[REGISTER] Registration successful!");
        lv_label_set_text(lblStatus, "Registration successful!");
        displayManager.update();
        delay(1000);
        return true;
    }
    
    lv_label_set_text(lblStatus, "Registration failed");
    displayManager.update();
    delay(1000);
    return false;
}

bool loginAndGetDevice()
{
    lv_label_set_text(lblStatus, "Authentication in progress...");
    displayManager.update();
    Serial.println("1. Authenticating (getting token)...");

    String contentType = "application/x-www-form-urlencoded";
    String postData = "username=" + String(USERNAME) + "&password=" + String(PASSWORD);

    Serial.println("  Sending credentials...");
    lv_label_set_text(lblStatus, "Sending credentials...");
    displayManager.update();
    
    httpClient->post("/token", contentType, postData);
    int statusCode = httpClient->responseStatusCode();
    String responseBody = httpClient->responseBody();

    Serial.printf("  Status code: %d\n", statusCode);

    // If 401, try to register a new user
    if (statusCode == 401)
    {
        Serial.println("[AUTH] 401 Unauthorized. Attempting auto-registration...");
        lv_label_set_text(lblStatus, "401: Registering...");
        displayManager.update();
        delay(1000);

        if (tryRegisterUser())
        {
            // Now try login again
            Serial.println("[AUTH] Registration successful. Retrying login...");
            lv_label_set_text(lblStatus, "Login again...");
            displayManager.update();
            delay(1000);
            
            httpClient->post("/token", contentType, postData);
            statusCode = httpClient->responseStatusCode();
            responseBody = httpClient->responseBody();
            
            Serial.printf("  Status code (retry): %d\n", statusCode);
            
            if (statusCode != 200)
            {
                Serial.printf("[AUTH] Login after registration failed, status: %d\n", statusCode);
                Serial.println(responseBody);
                lv_label_set_text(lblStatus, "Error: Login failed");
                displayManager.update();
                return false;
            }
        }
        else
        {
            Serial.println("[AUTH] Registration failed. Check the server.");
            lv_label_set_text(lblStatus, "Error: Registration failed");
            displayManager.update();
            return false;
        }
    }
    else if (statusCode != 200)
    {
        Serial.printf("[AUTH] Error obtaining token, status: %d\n", statusCode);
        Serial.println(responseBody);
        lv_label_set_text(lblStatus, ("Error " + String(statusCode)).c_str());
        displayManager.update();
        return false;
    }

    lv_label_set_text(lblStatus, "Token obtained!");
    displayManager.update();
    
    JsonDocument doc;
    deserializeJson(doc, responseBody);
    globalToken = doc["access_token"].as<String>();
    Serial.println("[AUTH] Token obtained.");
    Serial.printf("[AUTH] Token: %s\n", globalToken.c_str());
    delay(500);

    // 2. Get Device ID
    lv_label_set_text(lblStatus, "Getting Device ID...");
    displayManager.update();
    Serial.println("2. Getting Device ID...");
    Serial.println("  Sending request...");
    
    httpClient->beginRequest();
    httpClient->get("/devices/me");
    httpClient->sendHeader("Authorization", "Bearer " + globalToken);
    httpClient->endRequest();

    statusCode = httpClient->responseStatusCode();
    responseBody = httpClient->responseBody();

    Serial.printf("  Status code: %d\n", statusCode);

    if (statusCode != 200)
    {
        Serial.printf("[AUTH] Error obtaining device ID, status: %d\n", statusCode);
        Serial.println(responseBody);
        lv_label_set_text(lblStatus, "Error: Device ID");
        displayManager.update();
        return false;
    }

    deserializeJson(doc, responseBody);
    globalDeviceId = doc["deviceId"].as<String>();
    Serial.print("[AUTH] Device ID obtained: ");
    Serial.println(globalDeviceId);
    
    lv_label_set_text(lblStatus, "Authenticated successfully!");
    displayManager.update();
    delay(1000);
    
    return true;
}

void webSocketEvent(WStype_t type, uint8_t *payload, size_t length)
{
    size_t bytes_written = 0;
    switch (type)
    {
    case WStype_DISCONNECTED: {
        Serial.println("[WS] Disconnected.");
        isWebSocketConnected = false;
        if (lblStatus) lv_label_set_text(lblStatus, "Reconnecting...");
        break;
    }

    case WStype_CONNECTED: {
        Serial.println("[WS] Connected.");
        isWebSocketConnected = true;
        if (lblStatus) lv_label_set_text(lblStatus, "Ready");
        break;
    }

    case WStype_TEXT: {
        // client.py uses this for "talk_start" / "talk_stop"
        // (Ignored for now, we use WStype_BIN for 'incoming' indicator)
        Serial.printf("[WS] Text received: %s\n", payload);
        break;
    }

    case WStype_BIN: {
        // Incoming audio!
        lastAudioReceiveTime = millis();
        isReceivingAudio = true;

        // Write audio data directly to I2S speaker
        i2s_write(I2S_NUM_0, payload, length, &bytes_written, portMAX_DELAY);
        if (bytes_written != length) {
            Serial.println("[I2S] Error writing to speaker");
        }
        break;
    }

    default: {
        break;
    }
    }
}

void setupWebSocket()
{
    lv_label_set_text(lblStatus, "WebSocket: Connecting...");
    displayManager.update();
    Serial.println("3. Connecting to WebSocket...");
    String ws_path = "/ws/" + globalDeviceId + "?token=" + globalToken;
    
    Serial.println("  Path: " + ws_path);
    lv_label_set_text(lblStatus, "WS: Starting...");
    displayManager.update();
    
    // Use parsed host/port from SERVER_ENDPOINT (server_host_str, server_port_int)
    webSocket.begin(server_host_str.c_str(), server_port_int, ws_path);
    webSocket.onEvent(webSocketEvent);
    webSocket.setReconnectInterval(5000);
    
    lv_label_set_text(lblStatus, "WS: Waiting for connection...");
    displayManager.update();
    
    Serial.println("  WebSocket configured");
}

// =================================================================
// --- UI (LVGL) ---
// =================================================================
void create_ptt_ui()
{
    lv_obj_t *scr = lv_scr_act();
    lv_obj_clean(scr); // Clear any example UI

    // Set screen background to black
    static lv_style_t style_screen;
    lv_style_init(&style_screen);
    lv_style_set_bg_color(&style_screen, lv_color_hex(0x000000));
    lv_obj_add_style(scr, &style_screen, 0);

    // --- Font Style ---
    static lv_style_t style_status;
    lv_style_init(&style_status);
    lv_style_set_text_font(&style_status, &Inter_20);
    // Make general font color grey
    lv_style_set_text_color(&style_status, lv_color_hex(0x808080));

    static lv_style_t style_ptt;
    lv_style_init(&style_ptt);
    lv_style_set_text_font(&style_ptt, &Inter_40);
    lv_style_set_text_color(&style_ptt, lv_color_hex(0x808080));
    
    static lv_style_t style_incoming;
    lv_style_init(&style_incoming);
    lv_style_set_text_font(&style_incoming, &Inter_30);
    lv_style_set_text_color(&style_incoming, lv_palette_main(LV_PALETTE_ORANGE));

    // --- Status Label (Top) ---
    lblStatus = lv_label_create(scr);
    lv_obj_add_style(lblStatus, &style_status, 0);
    lv_label_set_text(lblStatus, "Initializing...");
    lv_obj_align(lblStatus, LV_ALIGN_TOP_MID, 0, 10);

    // --- PTT Status Label (Center) ---
    lblPttStatus = lv_label_create(scr);
    lv_obj_add_style(lblPttStatus, &style_ptt, 0);
    lv_label_set_text(lblPttStatus, "HOLD TO TALK");
    lv_obj_align(lblPttStatus, LV_ALIGN_CENTER, 0, 0);

    // --- Incoming Status Label (Bottom) ---
    lblIncomingStatus = lv_label_create(scr);
    lv_obj_add_style(lblIncomingStatus, &style_incoming, 0);
    lv_label_set_text(lblIncomingStatus, ""); // Empty at start
    lv_obj_align(lblIncomingStatus, LV_ALIGN_BOTTOM_MID, 0, -30);
}

// =================================================================
// --- FreeRTOS Tasks (Core Logic) ---
// =================================================================

/**
 * Task (Core 0): Reads the PTT button (BTN_A) from the I/O expander.
 * Sets the 'isPttActive' flag and notifies the main loop.
 */
void ptt_button_task(void *pvParameters)
{
    Serial.println("Starting PTT Button Task (Core 0)...");
    
    // The 'Wire' and 'io_expander' are initialized in setup()
    bool lastState = false;

    while (true)
    {
        // The bottom button of the expander is active-low
        bool currentState = !io_expander.read1(EXPANDER_BUTTON_BOTTOM);

        if (currentState != lastState)
        {
            isPttActive = currentState;
            pttStateChanged = true; // Notify main loop to act
            lastState = currentState;
        }
        vTaskDelay(pdMS_TO_TICKS(20)); // Poll every 20ms
    }
}

/**
 * Task (Core 0): Continuously reads from the I2S microphone.
 * If 'isPttActive' is true, sends the data via WebSocket.
 */
void i2s_read_task(void *pvParameters)
{
    Serial.println("Starting I2S Read Task (Core 0)...");
    size_t bytes_read = 0;

    while (true)
    {
        // Read data from I2S microphone
        esp_err_t err = i2s_read(I2S_NUM_0, (void *)i2s_read_buffer, I2S_READ_BUFFER_BYTES, &bytes_read, portMAX_DELAY);

        if (err != ESP_OK) {
            Serial.printf("[I2S Read Task] Read error: %d\n", err);
            continue;
        }

        // Send only if PTT is active and connected
        if (isPttActive && isWebSocketConnected && bytes_read > 0)
        {
            webSocket.sendBIN((uint8_t *)i2s_read_buffer, bytes_read);
        }
    }
}

// =================================================================
// --- Setup and Loop (Main Functions) ---
// =================================================================

void setup()
{
    Serial.begin(115200);
    Serial.println("--- Starting Kode Dot PTT Client ---");

    // --- Hardware Initialization (from template) ---
    // Initialize I2C for I/O expander (using BSP pins)
    Serial.println("I2C: Initializing...");
    Wire.begin(IOEXP_I2C_SDA, IOEXP_I2C_SCL);
    delay(200);

    // Configure I/O Expander
    Serial.println("I/O Expander: Initializing...");
    if (!io_expander.begin())
    {
        Serial.println("ERROR: Could not find TCA9555 I/O expander.");
        while (1) delay(100);
    }
    io_expander.pinMode1(EXPANDER_BUTTON_BOTTOM, INPUT);
    Serial.println("I/O Expander configured");
    delay(200);

    // Initialize LED
    Serial.println("LED: Initializing...");
    led_setup();
    led_set_rgb(0, 0, 20); // Blue during startup
    led_show();
    delay(200);

    // Initialize Display and LVGL via DisplayManager
    Serial.println("Display: Initializing...");
    if (!displayManager.init()) {
        Serial.println("ERROR: Display init failed");
        while (1) delay(100);
    }
    
    // *** IMPORTANT: Create UI BEFORE using it ***
    Serial.println("Creating UI...");
    create_ptt_ui();
    
    // Now we can use lblStatus
    lv_label_set_text(lblStatus, "INITIALIZING...");
    displayManager.update();
    delay(500);
    
    // --- SD Card Initialization (SD_MMC with custom pins) ---
    lv_label_set_text(lblStatus, "SD: Configuring pins...");
    displayManager.update();
    Serial.println("SD: Configuring pins...");
    
    // Configure SD_MMC pins according to Kode Dot board
    if (!SD_MMC.setPins(SD_PIN_CLK, SD_PIN_CMD, SD_PIN_D0))
    {
        Serial.println("ERROR: Could not configure SD_MMC pins");
        lv_label_set_text(lblStatus, "ERROR: SD setPins");
        displayManager.update();
        while (1) delay(100);
    }
    Serial.println("SD_MMC pins configured");
    delay(200);
    
    // Initialize SD_MMC with 1-bit mode
    lv_label_set_text(lblStatus, "SD: Initializing...");
    displayManager.update();
    Serial.println("SD: Initializing in 1-bit mode...");
    if (!SD_MMC.begin(SD_MOUNT_POINT, 1))  // 1 = 1-bit mode
    {
        Serial.println("ERROR: Could not initialize SD card");
        lv_label_set_text(lblStatus, "ERROR: SD failed");
        displayManager.update();
        while (1) delay(100);
    }
    Serial.println("SD initialized successfully");
    delay(200);

    // --- Read configurations from SD ---
    readOrCreatePTTConfig();

    // --- Get device MAC as credentials ---
    USERNAME = getDeviceMAC();
    PASSWORD = USERNAME; // MAC is the password too
    
    Serial.printf("USERNAME (MAC): %s\n", USERNAME.c_str());
    Serial.printf("PASSWORD (MAC): %s\n", PASSWORD.c_str());
    Serial.printf("FRIENDLY_NAME: %s\n", FRIENDLY_NAME.c_str());

    lv_label_set_text(lblStatus, "Credentials OK");
    displayManager.update();
    delay(500);
    
    // --- End of Hardware Initialization ---

    // --- PTT Initialization ---
    setupI2S();
    setupWifi();

    if (loginAndGetDevice())
    {
        setupWebSocket();
        lv_label_set_text(lblStatus, "Ready");
        displayManager.update();
    }
    else
    {
        Serial.println("ERROR: Authentication failed");
        lv_label_set_text(lblStatus, "Auth Failed!");
        displayManager.update();
        while (1) delay(100);
    }
    
    led_set_rgb(0, 0, 0); // Turn off LED
    led_show();

    // --- Start Tasks (on Core 0) ---
    lv_label_set_text(lblStatus, "STARTING TASKS...");
    displayManager.update();
    delay(500);
    
    xTaskCreatePinnedToCore(
        ptt_button_task,
        "PTTButtonTask",
        2048,
        NULL,
        5,
        NULL,
        0
    );

    xTaskCreatePinnedToCore(
        i2s_read_task,
        "I2SReadTask",
        4096,
        NULL,
        5,
        NULL,
        0
    );
    
    Serial.println("--- Configuration Complete ---");
    lv_label_set_text(lblStatus, "Ready");
    displayManager.update();
}

void loop()
{
    // --- Main Loop Tasks (Core 1) ---

    // 1. Handle WebSocket client (very important)
    webSocket.loop();

    // 2. Handle LVGL via DisplayManager (updates ticks and handlers)
    displayManager.update();
    delay(5);

    // 3. Handle PTT state changes (from flag)
    if (pttStateChanged)
    {
        pttStateChanged = false; // Reset the flag
        if (isPttActive)
        {
            // --- PTT PRESSED ---
            Serial.println("PTT: START");
            if (isWebSocketConnected) {
                // Send "talk_start" (as in client.py)
                webSocket.sendTXT("{\"type\":\"talk_start\"}");
            }
            lv_label_set_text(lblPttStatus, "TALKING");
            led_set_rgb(0, 50, 0); // Green
        }
        else
        {
            // --- PTT RELEASED ---
            Serial.println("PTT: STOP");
             if (isWebSocketConnected) {
                // Send "talk_stop" (as in client.py)
                webSocket.sendTXT("{\"type\":\"talk_stop\"}");
            }
            lv_label_set_text(lblPttStatus, "HOLD TO TALK");
            led_set_rgb(0, 0, 0); // Off
        }
        led_show();
    }

    // 4. Handle incoming audio state (LED and UI)
    if (isReceivingAudio)
    {
        // If we're receiving, update the state
        if (!isPttActive) // Don't show "incoming" if we're talking
        { 
            lv_label_set_text(lblIncomingStatus, "INCOMING");
            led_set_rgb(60, 30, 0); // Orange
            led_show();
        }
        isReceivingAudio = false; // Reset (will be set in next packet)
    }
    else if (millis() - lastAudioReceiveTime > AUDIO_DECAY_MS)
    {
        // If time has passed since last packet, clean up
        if (strlen(lv_label_get_text(lblIncomingStatus)) > 0)
        {
            lv_label_set_text(lblIncomingStatus, "");
            if (!isPttActive) {
                led_set_rgb(0, 0, 0); // Turn off
                led_show();
            }
        }
    }

    // 5. Send Keepalive Ping (as in client.py)
    if (isWebSocketConnected && (millis() - lastPingTime > KEEPALIVE_MS))
    {
        webSocket.sendTXT("{\"type\":\"ping\"}");
        lastPingTime = millis();
    }
}