#include <WiFi.h>
#include <esp_now.h>
#include "esp_timer.h"
#include "driver/gpio.h"

#define PPM_OUTPUT_PIN 25
#define CHANNELS 8

// PPM timing (us)
#define PPM_FRAME_LENGTH 22500UL
#define PPM_PULSE_LENGTH 400UL
#define PPM_CENTER 1500

struct __attribute__((packed)) Packet {
  uint16_t channels[CHANNELS];
  uint16_t crc;
};

static Packet incomingPkt;
static volatile uint16_t rxChannels[CHANNELS];
static volatile uint32_t lastPacketTime = 0; // millis()
static portMUX_TYPE mux = portMUX_INITIALIZER_UNLOCKED;

// CRC16-CCITT (与你的 TX 一致)
uint16_t crc16_ccitt(const uint8_t *data, size_t len) {
  uint16_t crc = 0xFFFF;
  while (len--) {
    crc ^= (uint16_t)(*data++) << 8;
    for (uint8_t i = 0; i < 8; i++) {
      if (crc & 0x8000)
        crc = (crc << 1) ^ 0x1021;
      else
        crc <<= 1;
    }
  }
  return crc;
}

// ESP-NOW receive callback
void onDataRecv(const esp_now_recv_info_t *recv_info, const uint8_t *incomingData, int len) {
  (void)recv_info;
  if (len != sizeof(Packet)) {
    return;
  }

  Packet pkt;
  memcpy(&pkt, incomingData, sizeof(pkt));

  uint16_t calc = crc16_ccitt((uint8_t *)pkt.channels, sizeof(pkt.channels));
  if (calc != pkt.crc) {
    return;
  }

  portENTER_CRITICAL(&mux);
  for (int i = 0; i < CHANNELS; ++i) {
    uint16_t v = pkt.channels[i];
    if (v < 1000) v = 1000;
    if (v > 2000) v = 2000;
    rxChannels[i] = v;
  }
  lastPacketTime = millis();
  portEXIT_CRITICAL(&mux);
}

// PPM生成器
static esp_timer_handle_t ppm_timer = NULL;
static volatile int nextChannel = 0;
static volatile uint32_t accumChannels = 0;
static volatile bool highState = false;     // true = 高电平输出，false = 低电平输出
static volatile bool syncPending = false;

void onPpmTimer(void *arg) {
  (void)arg;

  if (syncPending) {
    syncPending = false;
    gpio_set_level((gpio_num_t)PPM_OUTPUT_PIN, 1); // 反极性，高电平
    highState = true;
    esp_timer_start_once(ppm_timer, PPM_PULSE_LENGTH);
    return;
  }

  if (highState) {
    // 高电平结束，切低电平输出低脉冲
    gpio_set_level((gpio_num_t)PPM_OUTPUT_PIN, 0); // 反极性，低电平
    highState = false;

    uint32_t lowDuration = PPM_PULSE_LENGTH;

    portENTER_CRITICAL(&mux);
    if (nextChannel < CHANNELS) {
      uint16_t ch = rxChannels[nextChannel];
      accumChannels += ch;
      nextChannel++;
    }
    portEXIT_CRITICAL(&mux);

    esp_timer_start_once(ppm_timer, lowDuration);
    return;
  } else {
    // 低电平结束，开始高电平输出，或者开始sync脉冲
    portENTER_CRITICAL(&mux);
    if (nextChannel >= CHANNELS) {
      uint32_t syncLow = 0;
      if (PPM_FRAME_LENGTH > accumChannels + PPM_PULSE_LENGTH) {
        syncLow = PPM_FRAME_LENGTH - accumChannels;
      } else {
        syncLow = PPM_PULSE_LENGTH + 1000; // 防护
      }
      accumChannels = 0;
      nextChannel = 0;
      syncPending = true;
      portEXIT_CRITICAL(&mux);

      esp_timer_start_once(ppm_timer, syncLow);
      return;
    } else {
      gpio_set_level((gpio_num_t)PPM_OUTPUT_PIN, 1); // 反极性，高电平
      highState = true;
      uint32_t highDuration = 0;
      uint16_t chVal = rxChannels[nextChannel];
      if (chVal > PPM_PULSE_LENGTH)
        highDuration = chVal - PPM_PULSE_LENGTH;
      else
        highDuration = 1;
      portEXIT_CRITICAL(&mux);

      esp_timer_start_once(ppm_timer, highDuration);
      return;
    }
  }
}

void startPPM() {
  portENTER_CRITICAL(&mux);
  nextChannel = 0;
  accumChannels = 0;
  highState = false;  // 先输出低电平开始脉冲
  syncPending = false;
  portEXIT_CRITICAL(&mux);

  gpio_set_level((gpio_num_t)PPM_OUTPUT_PIN, 0); // 反极性，先拉低开始脉冲
  esp_timer_start_once(ppm_timer, PPM_PULSE_LENGTH);
}

void setup() {
  Serial.begin(115200);
  delay(50);

  gpio_set_direction((gpio_num_t)PPM_OUTPUT_PIN, GPIO_MODE_OUTPUT);
  gpio_set_level((gpio_num_t)PPM_OUTPUT_PIN, 1); // idle 高电平

  for (int i = 0; i < CHANNELS; ++i) rxChannels[i] = PPM_CENTER;
  lastPacketTime = 0;

  WiFi.mode(WIFI_STA);
  Serial.print("RX MAC: ");
  Serial.println(WiFi.macAddress());

  if (esp_now_init() != ESP_OK) {
    Serial.println("ESP-NOW init failed");
    while (1) delay(1000);
  }

  esp_now_register_recv_cb(onDataRecv);

  esp_timer_create_args_t timer_args;
  timer_args.callback = &onPpmTimer;
  timer_args.arg = NULL;
  timer_args.name = "ppm_timer";

  if (esp_timer_create(&timer_args, &ppm_timer) != ESP_OK) {
    Serial.println("Failed to create esp_timer");
    while (1) delay(1000);
  }

  startPPM();

  Serial.println("RX ready, PPM output started");
}

void loop() {
  static unsigned long lastPrint = 0;
  unsigned long now = millis();

  if (now - lastPrint > 250) {
    lastPrint = now;

    if (millis() - lastPacketTime > 500) {
      Serial.print("[RX] No recent packet (>");
      Serial.print(millis() - lastPacketTime);
      Serial.println(" ms). Holding last values.");
    } else {
      Serial.print("[RX] Channels: ");
      portENTER_CRITICAL(&mux);
      for (int i = 0; i < CHANNELS; ++i) {
        Serial.print(rxChannels[i]);
        if (i < CHANNELS - 1) Serial.print(", ");
      }
      portEXIT_CRITICAL(&mux);
      Serial.println();
    }
  }

  if (millis() - lastPacketTime > 1000) {
    portENTER_CRITICAL(&mux);
    for (int i = 0; i < CHANNELS; ++i) rxChannels[i] = PPM_CENTER;
    portEXIT_CRITICAL(&mux);
  }

  delay(10);
}
