#include <ESP8266WiFi.h>
#include <espnow.h>

#define PPM_INPUT_PIN 2 // ESP8266 GPIO2 示例
#define CHANNELS 8

struct __attribute__((packed)) Packet {
  uint16_t channels[CHANNELS];
  uint16_t crc;
};

Packet pkt;
volatile uint16_t ppmValues[CHANNELS];
volatile uint8_t channelIndex = 0;
volatile unsigned long lastRise = 0;

// 接收端MAC地址（需要替换为你的接收器MAC）
uint8_t rxMac[6] = {0x5C, 0xCF, 0x7F, 0xB1, 0x43, 0x89};

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

void ICACHE_RAM_ATTR ppmISR() {
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

void onDataSent(uint8_t *mac_addr, uint8_t sendStatus) {
  Serial.print("Send status: ");
  Serial.println(sendStatus == 0 ? "Success" : "Fail");
}

void setup() {
  Serial.begin(115200);
  WiFi.mode(WIFI_STA);

  Serial.print("TX MAC Address: ");
  Serial.println(WiFi.macAddress());

  if (esp_now_init() != 0) {
    Serial.println("ESP-NOW init failed");
    while (1) delay(1000);
  }

  esp_now_set_self_role(ESP_NOW_ROLE_CONTROLLER);
  esp_now_register_send_cb(onDataSent);

  if (esp_now_add_peer(rxMac, ESP_NOW_ROLE_SLAVE, 1, NULL, 0) == 0) {
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

  uint8_t result = esp_now_send(rxMac, (uint8_t *)&pkt, sizeof(pkt));
  if (result != 0) {
    Serial.print("Send error: ");
    Serial.println(result);
  }
  delay(20);
}
