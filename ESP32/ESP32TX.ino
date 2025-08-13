#include <WiFi.h>
#include <esp_now.h>

#define PPM_INPUT_PIN 34
#define CHANNELS 8

struct __attribute__((packed)) Packet {
  uint16_t channels[CHANNELS];
  uint16_t crc;
};

Packet pkt;
volatile uint16_t ppmValues[CHANNELS];
volatile uint8_t channelIndex = 0;
volatile unsigned long lastRise = 0;

const uint8_t rxMac[6] = {0xC0, 0x49, 0xEF, 0xCB, 0x94, 0xA4}; // RX MAC地址

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

void IRAM_ATTR ppmISR() {
  unsigned long now = micros();
  unsigned long width = now - lastRise;
  lastRise = now;

  if (width >= 3000) {
    channelIndex = 0;
  } else if (channelIndex < CHANNELS) {
    ppmValues[channelIndex] = constrain(width, 1000, 2000);
    channelIndex++;
  }
}

void printMac(const uint8_t *mac) {
  for (int i = 0; i < 6; i++) {
    if (i) Serial.print(":");
    Serial.printf("%02X", mac[i]);
  }
  Serial.println();
}

void onDataSent(const wifi_tx_info_t *info, esp_now_send_status_t status) {
  Serial.print("Send status: ");
  Serial.println(status == ESP_NOW_SEND_SUCCESS ? "Success" : "Fail");
}

void setup() {
  Serial.begin(115200);
  WiFi.mode(WIFI_STA);

  Serial.print("TX MAC Address: ");
  Serial.println(WiFi.macAddress());

  if (esp_now_init() != ESP_OK) {
    Serial.println("ESP-NOW init failed");
    while (1) delay(1000);
  }

  esp_now_register_send_cb(onDataSent);

  esp_now_peer_info_t peerInfo = {};
  memcpy(peerInfo.peer_addr, rxMac, 6);
  peerInfo.channel = 0;
  peerInfo.encrypt = false;

  if (esp_now_add_peer(&peerInfo) == ESP_OK) {
    Serial.print("Added peer: ");
    printMac(rxMac);
  } else {
    Serial.println("Failed to add peer");
  }

  attachInterrupt(digitalPinToInterrupt(PPM_INPUT_PIN), ppmISR, RISING);

  for (uint8_t i = 0; i < CHANNELS; i++) ppmValues[i] = 1500;
}

void loop() {
  for (uint8_t i = 0; i < CHANNELS; i++) {
    pkt.channels[i] = ppmValues[i];
  }
  pkt.crc = crc16_ccitt((uint8_t *)pkt.channels, sizeof(pkt.channels));

  esp_err_t result = esp_now_send(rxMac, (uint8_t *)&pkt, sizeof(pkt));
  if (result != ESP_OK) {
    Serial.print("Send error: ");
    Serial.println(result);
  }
  delay(20);
}
