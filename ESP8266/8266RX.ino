#include <ESP8266WiFi.h>
#include <espnow.h>

#define PPM_OUTPUT_PIN 2 // GPIO2 (D4)，可改
#define CHANNELS 8

// PPM timing (us)
#define PPM_FRAME_LENGTH 22500UL
#define PPM_PULSE_LENGTH 400UL
#define PPM_CENTER 1500

struct __attribute__((packed)) Packet {
  uint16_t channels[CHANNELS];
  uint16_t crc;
};

volatile uint16_t rxChannels[CHANNELS];
volatile uint32_t lastPacketTime = 0; // millis()

// PPM 状态变量
volatile int nextChannel = 0;
volatile uint32_t accumChannels = 0;
volatile bool highState = false;
volatile bool syncPending = false;

// CRC16-CCITT
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

// 接收回调
void onDataRecv(uint8_t *mac, uint8_t *incomingData, uint8_t len) {
  if (len != sizeof(Packet)) return;

  Packet pkt;
  memcpy(&pkt, incomingData, sizeof(pkt));

  uint16_t calc = crc16_ccitt((uint8_t *)pkt.channels, sizeof(pkt.channels));
  if (calc != pkt.crc) return;

  noInterrupts();
  for (int i = 0; i < CHANNELS; ++i) {
    uint16_t v = pkt.channels[i];
    if (v < 1000) v = 1000;
    if (v > 2000) v = 2000;
    rxChannels[i] = v;
  }
  lastPacketTime = millis();
  interrupts();
}

// PPM 定时器中断
void ICACHE_RAM_ATTR onPpmTimer() {
  if (syncPending) {
    syncPending = false;
    digitalWrite(PPM_OUTPUT_PIN, HIGH); // 高电平
    highState = true;
    timer1_write(PPM_PULSE_LENGTH * 80); // us 转换成时钟周期
    return;
  }

  if (highState) {
    // 高电平结束，进入低电平
    digitalWrite(PPM_OUTPUT_PIN, LOW);
    highState = false;

    uint32_t lowDuration = PPM_PULSE_LENGTH;

    noInterrupts();
    if (nextChannel < CHANNELS) {
      uint16_t ch = rxChannels[nextChannel];
      accumChannels += ch;
      nextChannel++;
    }
    interrupts();

    timer1_write(lowDuration * 80);
    return;
  } else {
    // 低电平结束，进入下一个高电平 或 Sync
    noInterrupts();
    if (nextChannel >= CHANNELS) {
      uint32_t syncLow = (PPM_FRAME_LENGTH > accumChannels + PPM_PULSE_LENGTH)
                           ? (PPM_FRAME_LENGTH - accumChannels)
                           : (PPM_PULSE_LENGTH + 1000);
      accumChannels = 0;
      nextChannel = 0;
      syncPending = true;
      interrupts();

      timer1_write(syncLow * 80);
      return;
    } else {
      digitalWrite(PPM_OUTPUT_PIN, HIGH);
      highState = true;
      uint32_t highDuration = 0;
      uint16_t chVal = rxChannels[nextChannel];
      highDuration = (chVal > PPM_PULSE_LENGTH) ? (chVal - PPM_PULSE_LENGTH) : 1;
      interrupts();

      timer1_write(highDuration * 80);
      return;
    }
  }
}

void startPPM() {
  noInterrupts();
  nextChannel = 0;
  accumChannels = 0;
  highState = false;
  syncPending = false;
  interrupts();

  digitalWrite(PPM_OUTPUT_PIN, LOW);
  timer1_write(PPM_PULSE_LENGTH * 80);
}

void setup() {
  Serial.begin(115200);
  delay(50);

  pinMode(PPM_OUTPUT_PIN, OUTPUT);
  digitalWrite(PPM_OUTPUT_PIN, HIGH); // idle 高电平

  for (int i = 0; i < CHANNELS; ++i) rxChannels[i] = PPM_CENTER;
  lastPacketTime = 0;

  WiFi.mode(WIFI_STA);
  Serial.print("RX MAC: ");
  Serial.println(WiFi.macAddress());

  if (esp_now_init() != 0) {
    Serial.println("ESP-NOW init failed");
    while (1) delay(1000);
  }

  esp_now_set_self_role(ESP_NOW_ROLE_SLAVE);
  esp_now_register_recv_cb(onDataRecv);

  // 启动定时器
  timer1_attachInterrupt(onPpmTimer);
  timer1_enable(TIM_DIV1, TIM_EDGE, TIM_SINGLE); // 80MHz, 边沿触发, 单次
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
      noInterrupts();
      for (int i = 0; i < CHANNELS; ++i) {
        Serial.print(rxChannels[i]);
        if (i < CHANNELS - 1) Serial.print(", ");
      }
      interrupts();
      Serial.println();
    }
  }

  // 超时复位到中心值
  if (millis() - lastPacketTime > 1000) {
    noInterrupts();
    for (int i = 0; i < CHANNELS; ++i) rxChannels[i] = PPM_CENTER;
    interrupts();
  }

  delay(10);
}
