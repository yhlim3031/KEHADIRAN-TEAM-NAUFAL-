#include "esp_camera.h"
#include <WiFi.h>
#include <WebServer.h>
#include <HTTPClient.h>

// ===== WiFi Config =====
const char* WIFI_SSID     = "Redmi 9";
const char* WIFI_PASSWORD = "test1234";

// ===== Flask server =====
const char* FLASK_UPLOAD_URL = "http://192.168.245.172:5000/upload";

// ===== Camera pins (AI-Thinker) =====
#define PWDN_GPIO_NUM     32
#define RESET_GPIO_NUM    -1
#define XCLK_GPIO_NUM      0
#define SIOD_GPIO_NUM     26
#define SIOC_GPIO_NUM     27
#define Y9_GPIO_NUM       35
#define Y8_GPIO_NUM       34
#define Y7_GPIO_NUM       39
#define Y6_GPIO_NUM       36
#define Y5_GPIO_NUM       21
#define Y4_GPIO_NUM       19
#define Y3_GPIO_NUM       18
#define Y2_GPIO_NUM        5
#define VSYNC_GPIO_NUM    25
#define HREF_GPIO_NUM     23
#define PCLK_GPIO_NUM     22

#define FLASH_GPIO_NUM     4

WebServer server(80);
volatile bool captureEnabled = true; // 📸 auto ON dari mula

void startCameraServer();
void handle_jpg_stream();
void connectWiFi();
void postFrame(void *pvParameters);

void setup() {
  Serial.begin(115200);
  delay(1000);
  Serial.println("\n📸 ESP32-CAM starting...");

  camera_config_t config;
  config.ledc_channel = LEDC_CHANNEL_0;
  config.ledc_timer   = LEDC_TIMER_0;
  config.pin_d0       = Y2_GPIO_NUM;
  config.pin_d1       = Y3_GPIO_NUM;
  config.pin_d2       = Y4_GPIO_NUM;
  config.pin_d3       = Y5_GPIO_NUM;
  config.pin_d4       = Y6_GPIO_NUM;
  config.pin_d5       = Y7_GPIO_NUM;
  config.pin_d6       = Y8_GPIO_NUM;
  config.pin_d7       = Y9_GPIO_NUM;
  config.pin_xclk     = XCLK_GPIO_NUM;
  config.pin_pclk     = PCLK_GPIO_NUM;
  config.pin_vsync    = VSYNC_GPIO_NUM;
  config.pin_href     = HREF_GPIO_NUM;
  config.pin_sscb_sda = SIOD_GPIO_NUM;
  config.pin_sscb_scl = SIOC_GPIO_NUM;
  config.pin_pwdn     = PWDN_GPIO_NUM;
  config.pin_reset    = RESET_GPIO_NUM;
  config.xclk_freq_hz = 20000000;
  config.pixel_format = PIXFORMAT_JPEG;

  config.frame_size   = FRAMESIZE_VGA;  
  config.jpeg_quality = 12;
  config.fb_count     = 2;

  if (esp_camera_init(&config) != ESP_OK) {
    Serial.println("❌ Camera init failed");
    while (true) delay(1000);
  }

  pinMode(FLASH_GPIO_NUM, OUTPUT);
  digitalWrite(FLASH_GPIO_NUM, HIGH); // mula-mula hidup flash

  connectWiFi();
  startCameraServer();
  Serial.println("✅ Stream ready");

  // 📸 auto mula ambil gambar setiap 1s
  xTaskCreatePinnedToCore(postFrame, "PostFrameTask", 8192, NULL, 1, NULL, 1);
}

void loop() {
  server.handleClient();
}

void postFrame(void *pvParameters) {
  for (;;) {
    if (captureEnabled) {
      digitalWrite(FLASH_GPIO_NUM, HIGH);
      camera_fb_t * fb = esp_camera_fb_get();
      if (fb && WiFi.status() == WL_CONNECTED) {
        HTTPClient http;
        http.begin(FLASK_UPLOAD_URL);
        http.addHeader("Content-Type", "image/jpeg");
        int httpCode = http.POST(fb->buf, fb->len);
        if (httpCode > 0) {
          Serial.println("📤 Frame uploaded: " + http.getString());
        } else {
          Serial.println("❌ POST failed: " + String(httpCode));
        }
        http.end();
        esp_camera_fb_return(fb);
      }
    } else {
      digitalWrite(FLASH_GPIO_NUM, LOW);
    }
    vTaskDelay(5000 / portTICK_PERIOD_MS);
  }
}

void connectWiFi() {
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
  Serial.print("📶 Connecting to WiFi");
  while (WiFi.status() != WL_CONNECTED) {
    delay(500);
    Serial.print(".");
  }
  Serial.println("\n✅ WiFi connected");
  Serial.print("📍 ESP32-CAM IP: ");
  Serial.println(WiFi.localIP());
}

void handle_jpg_stream() {
  WiFiClient client = server.client();
  String response = "HTTP/1.1 200 OK\r\n";
  response += "Content-Type: multipart/x-mixed-replace; boundary=frame\r\n\r\n";
  server.sendContent(response);

  while (client.connected()) {
    if (!captureEnabled) break;
    camera_fb_t * fb = esp_camera_fb_get();
    if (!fb) break;
    server.sendContent("--frame\r\n");
    server.sendContent("Content-Type: image/jpeg\r\n\r\n");
    client.write(fb->buf, fb->len);
    server.sendContent("\r\n");
    esp_camera_fb_return(fb);
    delay(100);
  }
}

void startCameraServer() {
  server.on("/", HTTP_GET, []() {
    String html = "<html><head><title>ESP32-CAM</title></head><body>";
    html += "<h2>ESP32-CAM Stream</h2>";
    html += "<img src='/stream' width='640'><br>";
    html += "<p>Posting frames every 1s.</p>";
    html += "</body></html>";
    server.send(200, "text/html", html);
  });

  server.on("/stream", HTTP_GET, handle_jpg_stream);

  // ✅ Tambah dua endpoint kawalan
  server.on("/stop", HTTP_GET, []() {
    captureEnabled = false;
    server.send(200, "text/plain", "📸 Capture stopped");
  });
  server.on("/start", HTTP_GET, []() {
    captureEnabled = true;
    server.send(200, "text/plain", "📸 Capture started");
  });

  server.begin();
}
