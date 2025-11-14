/*
 * Kode Dot PTT (Push-to-Talk) App
 * ------------------------------------------------------------
 * Propósito:
 * - Implementa la lógica de 'client.py' en el hardware de Kode Dot.
 * - Utiliza el 'Base Project' como plantilla para la inicialización
 * del hardware (Display, LVGL, LED, Expansor de E/S).
 *
 * Funcionalidad:
 * - Se conecta a WiFi y se autentica en el servidor para obtener un token.
 * - Se conecta al servidor WebSocket para PTT.
 * - Usa el BOTÓN A como un botón físico de "Hold-to-Talk" (PTT).
 * - Captura audio 16kHz/16-bit del mic I2S mientras PTT está presionado.
 * - Envía audio como mensajes binarios de WebSocket.
 * - Recibe audio binario de WebSocket y lo reproduce en el altavoz I2S.
 * - Muestra el estado (Conectando, Listo, Hablando, Entrante) en la pantalla LVGL.
 * - Usa el LED RGB para indicar el estado (Verde=Hablando, Naranja=Entrante).
 */

// =================================================================
// --- Includes del Template de Kode Dot ---
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
// --- Includes AÑADIDOS para PTT ---
// =================================================================
#include <WiFi.h>
#include <ArduinoHttpClient.h>
#include <WebSocketsClient.h>
#include <ArduinoJson.h>
#include "driver/i2s.h"

// =================================================================
// --- Includes de Fuentes (desde tu proyecto) ---
// =================================================================
// NOTE: Font .c files live in src/fonts/ and are compiled separately by
// the build system. Do NOT #include the .c files here (causes duplicate
// symbol / redefinition errors). Refer to the font objects (e.g. Inter_20)
// directly in code; they will be linked from the separate compilation units.

// =================================================================
// --- TUS_SECRETOS_AQUI ---
// =================================================================
// --- Wi-Fi ---
const char *WIFI_SSID = "Lago Sommer IoT";
const char *WIFI_PASS = "Jose1999";

// --- Credenciales del Servidor ---
// (Las mismas que usas en el cliente Python)
// IMPORTANTE: Actualiza estos valores con las credenciales de tu servidor
const char *USERNAME = "kode_dot";      // Cambiar a tu usuario
const char *PASSWORD = "kode_dot_pass"; // Cambiar a tu contraseña

// =================================================================
// --- Configuración del Servidor y Cliente ---
// (Extraído de tu client.py)
// =================================================================
const char *SERVER_HOST = "192.168.178.4";
const int SERVER_PORT = 8000;
const unsigned long KEEPALIVE_MS = 20000;     // 20s ws keepalive ping
const unsigned long AUDIO_DECAY_MS = 1200;    // cómo se mantiene visible "incoming"

// =================================================================
// --- Configuración de Audio (de client.py) ---
// =================================================================
const int SAMPLE_RATE = 16000;
const int BITS_PER_SAMPLE = 16;
// 512 bytes por chunk / 2 bytes por muestra = 256 muestras
const int AUDIO_BUFFER_SAMPLES = 256;
// Tamaño del buffer en bytes
const int I2S_READ_BUFFER_BYTES = AUDIO_BUFFER_SAMPLES * (BITS_PER_SAMPLE / 8);
// Buffer para leer del micrófono
int16_t i2s_read_buffer[AUDIO_BUFFER_SAMPLES];

// =================================================================
// --- Variables Globales de Estado ---
// =================================================================
// --- Red ---
WiFiClient wifiClient;
HttpClient httpClient(wifiClient, SERVER_HOST, SERVER_PORT);
WebSocketsClient webSocket;
String globalToken;    // Token de autenticación
String globalDeviceId; // ID de este dispositivo
bool isWebSocketConnected = false;
unsigned long lastPingTime = 0;

// --- Estado PTT ---
// 'volatile' es crucial porque estas variables son modificadas
// por tareas (Tasks) y leídas por otras.
volatile bool isPttActive = false;
volatile bool pttStateChanged = false; // Flag para notificar al loop principal

// --- Estado Audio Entrante ---
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
// --- Helpers de LED (del template main.cpp) ---
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
// --- Funciones de Configuración PTT ---
// =================================================================

void setupI2S()
{
    Serial.println("Configurando I2S...");

    i2s_config_t i2s_config = {
        .mode = (i2s_mode_t)(I2S_MODE_MASTER | I2S_MODE_TX | I2S_MODE_RX),
        .sample_rate = SAMPLE_RATE,
        .bits_per_sample = I2S_BITS_PER_SAMPLE_16BIT,
        .channel_format = I2S_CHANNEL_FMT_ONLY_RIGHT, // Mono (o LEFT, depende del mic)
        .communication_format = I2S_COMM_FORMAT_STAND_I2S,
        .intr_alloc_flags = ESP_INTR_FLAG_LEVEL1,
        .dma_buf_count = 8,
        .dma_buf_len = 64,
        .use_apll = false,
        .tx_desc_auto_clear = true
    };

    // Usar los pines definidos en kodedot/pin_config.h
    i2s_pin_config_t pin_config = {
        .bck_io_num = MIC_I2S_SCK,
        .ws_io_num = MIC_I2S_WS,
        // Speaker pin not defined in BSP; leave as -1 if unused
        .data_out_num = -1, // Altavoz (no definido en BSP)
        .data_in_num = MIC_I2S_DIN     // Micrófono
    };

    i2s_driver_install(I2S_NUM_0, &i2s_config, 0, NULL);
    i2s_set_pin(I2S_NUM_0, &pin_config);
    i2s_set_clk(I2S_NUM_0, SAMPLE_RATE, I2S_BITS_PER_SAMPLE_16BIT, I2S_CHANNEL_MONO);

    Serial.println("I2S configurado.");
}

void setupWifi()
{
    lv_label_set_text(lblStatus, "Connecting to WiFi...");
    Serial.print("Conectando a ");
    Serial.println(WIFI_SSID);
    WiFi.begin(WIFI_SSID, WIFI_PASS);
    while (WiFi.status() != WL_CONNECTED)
    {
        delay(500);
        Serial.print(".");
    }
    Serial.println("\nWiFi conectado.");
    Serial.print("IP: ");
    Serial.println(WiFi.localIP());
}

bool tryRegisterUser()
{
    Serial.println("[REGISTER] Intentando registrar nuevo usuario...");
    lv_label_set_text(lblStatus, "Registering...");

    String friendlyName = String(USERNAME) + "_device";
    
    // Prepare JSON: {"username": "...", "password": "...", "friendlyName": "..."}
    JsonDocument doc;
    doc["username"] = USERNAME;
    doc["password"] = PASSWORD;
    doc["friendlyName"] = friendlyName;
    
    String jsonStr;
    serializeJson(doc, jsonStr);
    
    Serial.println("Enviando registro...");
    Serial.println(jsonStr);
    
    httpClient.post("/register", "application/json", jsonStr);
    int statusCode = httpClient.responseStatusCode();
    String responseBody = httpClient.responseBody();

    Serial.printf("[REGISTER] Status: %d\n", statusCode);
    Serial.println(responseBody);

    if (statusCode == 200 || statusCode == 201)
    {
        Serial.println("[REGISTER] ¡Registro exitoso!");
        return true;
    }
    return false;
}

bool loginAndGetDevice()
{
    lv_label_set_text(lblStatus, "Authenticating...");
    Serial.println("1. Autenticando (obteniendo token)...");

    String contentType = "application/x-www-form-urlencoded";
    String postData = "username=" + String(USERNAME) + "&password=" + String(PASSWORD);

    httpClient.post("/token", contentType, postData);
    int statusCode = httpClient.responseStatusCode();
    String responseBody = httpClient.responseBody();

    // Si obtiene 401, intenta registrar un nuevo usuario
    if (statusCode == 401)
    {
        Serial.println("[AUTH] 401 Unauthorized. Intentando auto-registro...");
        lv_label_set_text(lblStatus, "Registering...");
        delay(1000);

        if (tryRegisterUser())
        {
            // Ahora intenta login de nuevo
            Serial.println("[AUTH] Registro exitoso. Reintentando login...");
            delay(1000);
            
            httpClient.post("/token", contentType, postData);
            statusCode = httpClient.responseStatusCode();
            responseBody = httpClient.responseBody();
            
            if (statusCode != 200)
            {
                Serial.printf("[AUTH] Login después de registro falló, status: %d\n", statusCode);
                Serial.println(responseBody);
                lv_label_set_text(lblStatus, "Auth failed after reg");
                return false;
            }
        }
        else
        {
            Serial.println("[AUTH] Registro falló. Verifica el servidor.");
            lv_label_set_text(lblStatus, "Register failed");
            return false;
        }
    }
    else if (statusCode != 200)
    {
        Serial.printf("[AUTH] Error al obtener token, status: %d\n", statusCode);
        Serial.println(responseBody);
        lv_label_set_text(lblStatus, "Auth failed");
        return false;
    }

    JsonDocument doc;
    deserializeJson(doc, responseBody);
    globalToken = doc["access_token"].as<String>();
    Serial.println("[AUTH] Token obtenido.");
    Serial.printf("[AUTH] Token: %s\n", globalToken.c_str());

    // 2. Obtener Device ID
    lv_label_set_text(lblStatus, "Getting Device ID...");
    Serial.println("2. Obteniendo Device ID...");
    httpClient.beginRequest();
    httpClient.get("/devices/me");
    httpClient.sendHeader("Authorization", "Bearer " + globalToken);
    httpClient.endRequest();

    statusCode = httpClient.responseStatusCode();
    responseBody = httpClient.responseBody();

    if (statusCode != 200)
    {
        Serial.printf("[AUTH] Error al obtener device ID, status: %d\n", statusCode);
        Serial.println(responseBody);
        lv_label_set_text(lblStatus, "Auth failed (Device)");
        return false;
    }

    deserializeJson(doc, responseBody);
    globalDeviceId = doc["deviceId"].as<String>();
    Serial.print("[AUTH] Device ID obtenido: ");
    Serial.println(globalDeviceId);
    return true;
}

void webSocketEvent(WStype_t type, uint8_t *payload, size_t length)
{
    size_t bytes_written = 0;
    switch (type)
    {
    case WStype_DISCONNECTED: {
        Serial.println("[WS] Desconectado.");
        isWebSocketConnected = false;
        if (lblStatus) lv_label_set_text(lblStatus, "Reconnecting...");
        break;
    }

    case WStype_CONNECTED: {
        Serial.println("[WS] Conectado.");
        isWebSocketConnected = true;
        if (lblStatus) lv_label_set_text(lblStatus, "Ready");
        break;
    }

    case WStype_TEXT: {
        // El client.py usa esto para "talk_start" / "talk_stop"
        // (Ignorado por ahora, usamos WStype_BIN para el indicador 'incoming')
        Serial.printf("[WS] Texto recibido: %s\n", payload);
        break;
    }

    case WStype_BIN: {
        // ¡Audio entrante!
        lastAudioReceiveTime = millis();
        isReceivingAudio = true;

        // Escribir los datos de audio directamente al altavoz I2S
        i2s_write(I2S_NUM_0, payload, length, &bytes_written, portMAX_DELAY);
        if (bytes_written != length) {
            Serial.println("[I2S] Error al escribir en altavoz");
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
    lv_label_set_text(lblStatus, "Connecting to WS...");
    Serial.println("3. Conectando a WebSocket...");
    String ws_path = "/ws/" + globalDeviceId + "?token=" + globalToken;
    webSocket.begin(SERVER_HOST, SERVER_PORT, ws_path);
    webSocket.onEvent(webSocketEvent);
    webSocket.setReconnectInterval(5000);
}

// =================================================================
// --- UI (LVGL) ---
// =================================================================
void create_ptt_ui()
{
    lv_obj_t *scr = lv_scr_act();
    lv_obj_clean(scr); // Limpiar cualquier UI de ejemplo

    // --- Estilo de Fuente ---
    static lv_style_t style_status;
    lv_style_init(&style_status);
    lv_style_set_text_font(&style_status, &Inter_20);

    static lv_style_t style_ptt;
    lv_style_init(&style_ptt);
    lv_style_set_text_font(&style_ptt, &Inter_40);
    
    static lv_style_t style_incoming;
    lv_style_init(&style_incoming);
    lv_style_set_text_font(&style_incoming, &Inter_30);
    lv_style_set_text_color(&style_incoming, lv_palette_main(LV_PALETTE_ORANGE));

    // --- Etiqueta de Estado (Superior) ---
    lblStatus = lv_label_create(scr);
    lv_obj_add_style(lblStatus, &style_status, 0);
    lv_label_set_text(lblStatus, "Initializing...");
    lv_obj_align(lblStatus, LV_ALIGN_TOP_MID, 0, 10);

    // --- Etiqueta de Estado PTT (Centro) ---
    lblPttStatus = lv_label_create(scr);
    lv_obj_add_style(lblPttStatus, &style_ptt, 0);
    lv_label_set_text(lblPttStatus, "HOLD TO TALK");
    lv_obj_align(lblPttStatus, LV_ALIGN_CENTER, 0, 0);

    // --- Etiqueta de Estado Entrante (Inferior) ---
    lblIncomingStatus = lv_label_create(scr);
    lv_obj_add_style(lblIncomingStatus, &style_incoming, 0);
    lv_label_set_text(lblIncomingStatus, ""); // Vacío al inicio
    lv_obj_align(lblIncomingStatus, LV_ALIGN_BOTTOM_MID, 0, -30);
}

// =================================================================
// --- Tareas de FreeRTOS (Núcleo de la Lógica) ---
// =================================================================

/**
 * Tarea (Core 0): Lee el botón PTT (BTN_A) desde el expansor de E/S.
 * Establece el flag 'isPttActive' y notifica al loop principal.
 */
void ptt_button_task(void *pvParameters)
{
    Serial.println("Iniciando PTT Button Task (Core 0)...");
    
    // El 'Wire' y el 'io_expander' se inicializan en setup()
    bool lastState = false;

    while (true)
    {
        // El botón bottom del expansor es active-low
        bool currentState = !io_expander.read1(EXPANDER_BUTTON_BOTTOM);

        if (currentState != lastState)
        {
            isPttActive = currentState;
            pttStateChanged = true; // Notificar al loop principal para que actúe
            lastState = currentState;
        }
        vTaskDelay(pdMS_TO_TICKS(20)); // Poll cada 20ms
    }
}

/**
 * Tarea (Core 0): Lee continuamente desde el micrófono I2S.
 * Si 'isPttActive' es true, envía los datos por WebSocket.
 */
void i2s_read_task(void *pvParameters)
{
    Serial.println("Iniciando I2S Read Task (Core 0)...");
    size_t bytes_read = 0;

    while (true)
    {
        // Leer datos del micrófono I2S
        esp_err_t err = i2s_read(I2S_NUM_0, (void *)i2s_read_buffer, I2S_READ_BUFFER_BYTES, &bytes_read, portMAX_DELAY);

        if (err != ESP_OK) {
            Serial.printf("[I2S Read Task] Error de lectura: %d\n", err);
            continue;
        }

        // Enviar solo si PTT está activo y conectado
        if (isPttActive && isWebSocketConnected && bytes_read > 0)
        {
            webSocket.sendBIN((uint8_t *)i2s_read_buffer, bytes_read);
        }
    }
}

// =================================================================
// --- Setup y Loop (Funciones Principales) ---
// =================================================================

void setup()
{
    Serial.begin(115200);
    Serial.println("--- Iniciando Kode Dot PTT Client ---");

    // --- Inicialización de Hardware (del template) ---
    // Iniciar I2C para el expansor de E/S (usar pines del BSP)
    Wire.begin(IOEXP_I2C_SDA, IOEXP_I2C_SCL);

    // Configurar Expansor de E/S
    if (!io_expander.begin())
    {
        Serial.println("Error: No se pudo encontrar el expansor de E/S TCA9555.");
        while (1) delay(100);
    }
    // Configurar BTN (bottom) como entrada en el expansor
    io_expander.pinMode1(EXPANDER_BUTTON_BOTTOM, INPUT);

    // Iniciar LED
    led_setup();
    led_set_rgb(0, 0, 20); // Azul durante el inicio
    led_show();

    // Iniciar Display y LVGL mediante DisplayManager
    if (!displayManager.init()) {
        Serial.println("Error: Display init failed");
        while (1) delay(100);
    }
    create_ptt_ui(); // Crear nuestra UI personalizada
    // --- Fin de Inicialización de Hardware ---


    // --- Inicialización PTT ---
    setupI2S();
    setupWifi();

    if (loginAndGetDevice())
    {
        setupWebSocket();
    }
    else
    {
        Serial.println("Error en la autenticación. deteniendo.");
        lv_label_set_text(lblStatus, "Auth Failed!");
        while (1) delay(100);
    }
    
    led_set_rgb(0, 0, 0); // Apagar LED
    led_show();

    // --- Iniciar Tareas (en Core 0) ---
    xTaskCreatePinnedToCore(
        ptt_button_task,   // Función de la Tarea
        "PTTButtonTask",   // Nombre
        2048,              // Stack size
        NULL,              // Parámetros
        5,                 // Prioridad
        NULL,              // Handle
        0                  // Core 0
    );

    xTaskCreatePinnedToCore(
        i2s_read_task,     // Función de la Tarea
        "I2SReadTask",     // Nombre
        4096,              // Stack size
        NULL,              // Parámetros
        5,                 // Prioridad
        NULL,              // Handle
        0                  // Core 0
    );
    
    Serial.println("--- Configuración Completa ---");
}

void loop()
{
    // --- Tareas del Loop Principal (Core 1) ---

    // 1. Manejar el cliente WebSocket (muy importante)
    webSocket.loop();

    // 2. Manejar LVGL mediante DisplayManager (actualiza ticks y handlers)
    displayManager.update();
    delay(5);

    // 3. Manejar cambios de estado de PTT (desde el flag)
    if (pttStateChanged)
    {
        pttStateChanged = false; // Resetear el flag
        if (isPttActive)
        {
            // --- PTT PRESIONADO ---
            Serial.println("PTT: START");
            if (isWebSocketConnected) {
                // Enviar "talk_start" (como en client.py)
                webSocket.sendTXT("{\"type\":\"talk_start\"}");
            }
            lv_label_set_text(lblPttStatus, "TALKING");
            led_set_rgb(0, 50, 0); // Verde
        }
        else
        {
            // --- PTT SOLTADO ---
            Serial.println("PTT: STOP");
             if (isWebSocketConnected) {
                // Enviar "talk_stop" (como en client.py)
                webSocket.sendTXT("{\"type\":\"talk_stop\"}");
            }
            lv_label_set_text(lblPttStatus, "HOLD TO TALK");
            led_set_rgb(0, 0, 0); // Apagado
        }
        led_show();
    }

    // 4. Manejar estado de audio entrante (LED y UI)
    if (isReceivingAudio)
    {
        // Si estamos recibiendo, actualizar el estado
        if (!isPttActive) // No mostrar "incoming" si estamos hablando
        { 
            lv_label_set_text(lblIncomingStatus, "INCOMING");
            led_set_rgb(60, 30, 0); // Naranja
            led_show();
        }
        isReceivingAudio = false; // Resetear (se activará en el próximo paquete)
    }
    else if (millis() - lastAudioReceiveTime > AUDIO_DECAY_MS)
    {
        // Si ha pasado tiempo desde el último paquete, limpiar
        if (strlen(lv_label_get_text(lblIncomingStatus)) > 0)
        {
            lv_label_set_text(lblIncomingStatus, "");
            if (!isPttActive) {
                led_set_rgb(0, 0, 0); // Apagar
                led_show();
            }
        }
    }

    // 5. Enviar Ping de Keepalive (como en client.py)
    if (isWebSocketConnected && (millis() - lastPingTime > KEEPALIVE_MS))
    {
        webSocket.sendTXT("{\"type\":\"ping\"}");
        lastPingTime = millis();
    }
}