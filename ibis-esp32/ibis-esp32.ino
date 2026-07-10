#include <Arduino.h>
#include <WiFi.h>
#include <WiFiUdp.h>

#include "secrets.h"

static constexpr int IBIS_TX_PIN = 4;
static constexpr int IBIS_RX_PIN = -1;
static constexpr int GONG_E1_PIN = 0;
static constexpr int GONG_E2_PIN = 1;

HardwareSerial IBIS(1);

static constexpr int TEXT_FIELD_LEN = 20;
static constexpr uint16_t UDP_PORT = 4210;
static constexpr size_t UDP_TEXT_BUFFER_LEN = 128;
static constexpr size_t SERIAL_COMMAND_BUFFER_LEN = 128;
static constexpr size_t STREAM_FIELD_BUFFER_LEN = 64;
static constexpr unsigned long DISPLAY_POWERUP_DELAY_MS = 20000;
static constexpr unsigned long WIFI_CONNECT_TIMEOUT_MS = 20000;
static constexpr unsigned long WIFI_RETRY_INTERVAL_MS = 10000;
static constexpr unsigned long GONG_TRIGGER_MS = 1000;
static constexpr unsigned long STREAM_HEADER_MS = 2000;
static constexpr unsigned long MEMBER_NAME_MS = 10000;
static constexpr unsigned long SUPERCHAT_AMOUNT_MS = 5000;
static constexpr unsigned long SUPERCHAT_NAME_MS = 5000;
static constexpr unsigned long IP_REPEAT_INTERVAL_MS = 3000;
static constexpr uint8_t IP_STARTUP_REPEAT_COUNT = 2;
static constexpr wifi_power_t WIFI_TX_POWER = WIFI_POWER_8_5dBm;

enum class StreamSequenceType {
  NONE,
  MEMBER,
  SUPERCHAT
};

WiFiUDP udp;
char udpTextBuffer[UDP_TEXT_BUFFER_LEN];
char serialCommandBuffer[SERIAL_COMMAND_BUFFER_LEN];
char streamAmountBuffer[STREAM_FIELD_BUFFER_LEN];
char streamNameBuffer[STREAM_FIELD_BUFFER_LEN];
size_t serialCommandLen = 0;
bool udpStarted = false;
StreamSequenceType streamSequenceType = StreamSequenceType::NONE;
uint8_t streamSequenceStep = 0;
uint8_t ipRepeatsRemaining = 0;
unsigned long streamSequenceNextMs = 0;
unsigned long nextIpRepeatMs = 0;
unsigned long lastWifiRetryMs = 0;

void handleSerialInput();

// BCC passend zu deinem WinIBIS-Test:
// BCC = 0x7F XOR XOR(Payload + CR)
uint8_t calcIbisBcc(const uint8_t *payload, size_t len) {
  uint8_t x = 0x00;

  for (size_t i = 0; i < len; i++) {
    x ^= payload[i];
  }

  x ^= 0x0D;
  return 0x7F ^ x;
}

uint32_t readUtf8Codepoint(const char *text, size_t *index) {
  uint8_t c = (uint8_t)text[*index];

  if (c < 0x80) {
    (*index)++;
    return c;
  }

  uint8_t expectedLen = 0;
  uint32_t codepoint = 0;

  if ((c & 0xE0) == 0xC0) {
    expectedLen = 2;
    codepoint = c & 0x1F;
  } else if ((c & 0xF0) == 0xE0) {
    expectedLen = 3;
    codepoint = c & 0x0F;
  } else if ((c & 0xF8) == 0xF0) {
    expectedLen = 4;
    codepoint = c & 0x07;
  } else {
    (*index)++;
    return 0xFFFD;
  }

  for (uint8_t i = 1; i < expectedLen; i++) {
    uint8_t next = (uint8_t)text[*index + i];

    if (next == '\0' || (next & 0xC0) != 0x80) {
      (*index)++;
      return 0xFFFD;
    }

    codepoint = (codepoint << 6) | (next & 0x3F);
  }

  *index += expectedLen;
  return codepoint;
}

const char *displayReplacementFor(uint32_t codepoint) {
  switch (codepoint) {
    case 0x00C4: return "[";    // A-Umlaut
    case 0x00D6: return "\\";   // O-Umlaut
    case 0x00DC: return "]";    // U-Umlaut
    case 0x00E4: return "|";    // a-Umlaut
    case 0x00F6: return "{";    // o-Umlaut
    case 0x00FC: return "}";    // u-Umlaut
    case 0x00DF: return "~";    // sz
    case 0x00E9: return "e";
    case 0x00E8: return "e";
    case 0x00E1: return "a";
    case 0x00E0: return "a";
    case 0x00F3: return "o";
    case 0x00F2: return "o";
    case 0x00FA: return "u";
    case 0x00F9: return "u";
    case 0x2013: return "-";
    case 0x2014: return "-";
    case 0x2018: return "'";
    case 0x2019: return "'";
    case 0x201C: return "\"";
    case 0x201D: return "\"";
    case 0x2026: return "...";
    case 0x20AC: return "EUR";
    default: return " ";
  }
}

void appendDisplayText(uint8_t *out, size_t *outLen, const char *replacement) {
  while (*replacement != '\0' && *outLen < TEXT_FIELD_LEN) {
    out[*outLen] = (uint8_t)*replacement;
    (*outLen)++;
    replacement++;
  }
}

void makeDisplayTextField(uint8_t *out, const char *text) {
  size_t textIndex = 0;
  size_t outLen = 0;

  while (outLen < TEXT_FIELD_LEN && text[textIndex] != '\0') {
    uint32_t codepoint = readUtf8Codepoint(text, &textIndex);

    if (codepoint >= 0x20 && codepoint <= 0x7E) {
      char replacement[2] = { (char)codepoint, '\0' };

      if (codepoint == '\\') {
        replacement[0] = ' ';
      }

      appendDisplayText(out, &outLen, replacement);
    } else {
      appendDisplayText(out, &outLen, displayReplacementFor(codepoint));
    }
  }

  while (outLen < TEXT_FIELD_LEN) {
    out[outLen] = 0x20;
    outLen++;
  }
}

void makeDs009neuPayload(uint8_t *out, size_t outSize, const char *text) {
  // Aufbau:
  // v + 20 Zeichen Text
  // Also exakt 21 Bytes Payload ohne CR/BCC

  if (outSize < 1 + TEXT_FIELD_LEN) {
    return;
  }

  out[0] = 'v';

  makeDisplayTextField(out + 1, text);
}

void sendDs009neu(const char *text) {
  uint8_t payload[1 + TEXT_FIELD_LEN];

  makeDs009neuPayload(payload, sizeof(payload), text);

  uint8_t bcc = calcIbisBcc(payload, sizeof(payload));

  IBIS.write(payload, sizeof(payload));
  IBIS.write(0x0D);
  IBIS.write(bcc);

  Serial.print("Gesendet ASCII: [");

  for (size_t i = 0; i < sizeof(payload); i++) {
    if (payload[i] == 0x20) {
      Serial.print('_');
    } else {
      Serial.print((char)payload[i]);
    }
  }

  Serial.print("] <CR> BCC=0x");
  if (bcc < 0x10) Serial.print("0");
  Serial.println(bcc, HEX);

  Serial.print("Gesendet HEX: ");

  for (size_t i = 0; i < sizeof(payload); i++) {
    if (payload[i] < 0x10) Serial.print("0");
    Serial.print(payload[i], HEX);
    Serial.print("-");
  }

  Serial.print("0D-");
  if (bcc < 0x10) Serial.print("0");
  Serial.println(bcc, HEX);
}

void playGong(int gongNumber) {
  bool e1High = gongNumber == 1 || gongNumber == 3;
  bool e2High = gongNumber == 2 || gongNumber == 3;

  if (!e1High && !e2High) {
    Serial.println("GONG ungueltig, erlaubt: GONG 1, GONG 2, GONG 3");
    return;
  }

  digitalWrite(GONG_E1_PIN, e1High ? HIGH : LOW);
  digitalWrite(GONG_E2_PIN, e2High ? HIGH : LOW);
  delay(GONG_TRIGGER_MS);
  digitalWrite(GONG_E1_PIN, LOW);
  digitalWrite(GONG_E2_PIN, LOW);

  Serial.print("Gong ");
  Serial.print(gongNumber);
  Serial.print(" ausgeloest: E1=");
  Serial.print(e1High ? "HIGH" : "LOW");
  Serial.print(", E2=");
  Serial.println(e2High ? "HIGH" : "LOW");
}

void copyStreamField(char *dest, size_t destSize, const char *source) {
  if (destSize == 0) {
    return;
  }

  size_t i = 0;

  while (i < destSize - 1 && source[i] != '\0') {
    dest[i] = source[i];
    i++;
  }

  dest[i] = '\0';
}

void cancelStreamSequence() {
  streamSequenceType = StreamSequenceType::NONE;
  streamSequenceStep = 0;
  streamSequenceNextMs = 0;
}

void startMemberSequence(const char *name) {
  copyStreamField(streamNameBuffer, sizeof(streamNameBuffer), name);

  if (streamNameBuffer[0] == '\0') {
    return;
  }

  cancelStreamSequence();
  sendDs009neu("Naechstes Mitglied:");
  streamSequenceType = StreamSequenceType::MEMBER;
  streamSequenceStep = 0;
  streamSequenceNextMs = millis() + STREAM_HEADER_MS;
  playGong(2);
}

void startSuperchatSequence(const char *payload) {
  const char *separator = strchr(payload, '|');

  if (separator == nullptr || separator == payload || separator[1] == '\0') {
    Serial.println("SUPERCHAT Format: SUPERCHAT <Betrag>|<Name>");
    return;
  }

  size_t amountLen = separator - payload;
  size_t i = 0;

  while (i < sizeof(streamAmountBuffer) - 1 && i < amountLen) {
    streamAmountBuffer[i] = payload[i];
    i++;
  }

  streamAmountBuffer[i] = '\0';
  copyStreamField(streamNameBuffer, sizeof(streamNameBuffer), separator + 1);

  cancelStreamSequence();
  sendDs009neu("Naechster Superchat:");
  streamSequenceType = StreamSequenceType::SUPERCHAT;
  streamSequenceStep = 0;
  streamSequenceNextMs = millis() + STREAM_HEADER_MS;
  playGong(3);
}

void updateStreamSequence() {
  if (streamSequenceType == StreamSequenceType::NONE || millis() < streamSequenceNextMs) {
    return;
  }

  if (streamSequenceType == StreamSequenceType::MEMBER) {
    if (streamSequenceStep == 0) {
      sendDs009neu(streamNameBuffer);
      streamSequenceStep = 1;
      streamSequenceNextMs = millis() + MEMBER_NAME_MS;
      return;
    }

    cancelStreamSequence();
    return;
  }

  if (streamSequenceType == StreamSequenceType::SUPERCHAT) {
    if (streamSequenceStep == 0) {
      sendDs009neu(streamAmountBuffer);
      streamSequenceStep = 1;
      streamSequenceNextMs = millis() + SUPERCHAT_AMOUNT_MS;
      return;
    }

    if (streamSequenceStep == 1) {
      sendDs009neu(streamNameBuffer);
      streamSequenceStep = 2;
      streamSequenceNextMs = millis() + SUPERCHAT_NAME_MS;
      return;
    }

    cancelStreamSequence();
  }
}

void sendIpToDisplay() {
  IPAddress ip = WiFi.localIP();
  char ipText[TEXT_FIELD_LEN + 1];

  snprintf(ipText, sizeof(ipText), "IP %u.%u.%u.%u", ip[0], ip[1], ip[2], ip[3]);
  sendDs009neu(ipText);
}

void scheduleIpRepeats() {
  ipRepeatsRemaining = IP_STARTUP_REPEAT_COUNT;
  nextIpRepeatMs = millis() + IP_REPEAT_INTERVAL_MS;
}

bool connectWifi() {
  Serial.print("Verbinde WLAN: ");
  Serial.println(WIFI_SSID);

  WiFi.mode(WIFI_STA);
  WiFi.setSleep(false);
  WiFi.setTxPower(WIFI_TX_POWER);
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);

  unsigned long startMs = millis();

  while (WiFi.status() != WL_CONNECTED && millis() - startMs < WIFI_CONNECT_TIMEOUT_MS) {
    handleSerialInput();
    delay(500);
    Serial.print(".");
  }

  Serial.println();

  if (WiFi.status() != WL_CONNECTED) {
    Serial.println("WLAN nicht verbunden, versuche spaeter erneut.");
    sendDs009neu("WLAN Fehler");
    return false;
  }

  Serial.print("WLAN verbunden, IP: ");
  Serial.println(WiFi.localIP());
  return true;
}

void startUdpIfReady() {
  if (udpStarted || WiFi.status() != WL_CONNECTED) {
    return;
  }

  udp.begin(UDP_PORT);
  udpStarted = true;

  Serial.print("UDP lauscht auf Port ");
  Serial.println(UDP_PORT);
  sendIpToDisplay();
  scheduleIpRepeats();
}

void updateIpRepeats() {
  if (ipRepeatsRemaining == 0 || WiFi.status() != WL_CONNECTED || millis() < nextIpRepeatMs) {
    return;
  }

  sendIpToDisplay();
  ipRepeatsRemaining--;
  nextIpRepeatMs = millis() + IP_REPEAT_INTERVAL_MS;
}

void maintainWifi() {
  if (WiFi.status() == WL_CONNECTED) {
    startUdpIfReady();
    return;
  }

  udpStarted = false;

  if (millis() - lastWifiRetryMs < WIFI_RETRY_INTERVAL_MS) {
    return;
  }

  lastWifiRetryMs = millis();

  if (connectWifi()) {
    startUdpIfReady();
  }
}

void handleCommand(const char *command) {
  if (strncmp(command, "GONG ", 5) == 0) {
    playGong(atoi(command + 5));
    return;
  }

  if (strcmp(command, "IP") == 0) {
    if (WiFi.status() == WL_CONNECTED) {
      cancelStreamSequence();
      sendIpToDisplay();
    } else {
      cancelStreamSequence();
      sendDs009neu("WLAN Fehler");
    }

    return;
  }

  if (strncmp(command, "MEMBER ", 7) == 0) {
    startMemberSequence(command + 7);
    return;
  }

  if (strncmp(command, "SUPERCHAT ", 10) == 0) {
    startSuperchatSequence(command + 10);
    return;
  }

  const char *displayText = command;

  if (strncmp(displayText, "TEXT ", 5) == 0) {
    displayText += 5;
  }

  if (displayText[0] == '\0') {
    return;
  }

  cancelStreamSequence();
  sendDs009neu(displayText);
}

void handleUdpPacket() {
  int packetSize = udp.parsePacket();

  if (packetSize <= 0) {
    return;
  }

  int len = udp.read(udpTextBuffer, sizeof(udpTextBuffer) - 1);

  if (len <= 0) {
    return;
  }

  udpTextBuffer[len] = '\0';

  if (udp.available() > 0) {
    char discardBuffer[32];

    while (udp.available() > 0) {
      udp.read(discardBuffer, sizeof(discardBuffer));
    }

    Serial.print("UDP Paket gekuerzt: ");
    Serial.print(packetSize);
    Serial.println(" Bytes");
  }

  while (len > 0 && (udpTextBuffer[len - 1] == '\r' || udpTextBuffer[len - 1] == '\n')) {
    udpTextBuffer[len - 1] = '\0';
    len--;
  }

  Serial.print("UDP von ");
  Serial.print(udp.remoteIP());
  Serial.print(":");
  Serial.print(udp.remotePort());
  Serial.print(" -> [");
  Serial.print(udpTextBuffer);
  Serial.println("]");

  handleCommand(udpTextBuffer);
}

void handleSerialInput() {
  while (Serial.available() > 0) {
    char c = (char)Serial.read();

    if (c == '\r' || c == '\n') {
      if (serialCommandLen > 0) {
        serialCommandBuffer[serialCommandLen] = '\0';
        Serial.print("Serial -> [");
        Serial.print(serialCommandBuffer);
        Serial.println("]");
        handleCommand(serialCommandBuffer);
        serialCommandLen = 0;
      }

      continue;
    }

    if (serialCommandLen < sizeof(serialCommandBuffer) - 1) {
      serialCommandBuffer[serialCommandLen++] = c;
    }
  }
}

void setup() {
  Serial.begin(115200);
  delay(1000);

  Serial.println();
  Serial.println("ESP32-C3 IBIS DS009neu Sender");
  Serial.println("TX GPIO4");
  Serial.println("Gong E1 GPIO0, E2 GPIO1");
  Serial.println("Befehle: TEXT <Text>, GONG 1, GONG 2, GONG 3");
  Serial.println("Stream: MEMBER <Name>, SUPERCHAT <Betrag>|<Name>, IP");
  Serial.println("1200 Baud, 7E2");

  pinMode(GONG_E1_PIN, OUTPUT);
  pinMode(GONG_E2_PIN, OUTPUT);
  digitalWrite(GONG_E1_PIN, LOW);
  digitalWrite(GONG_E2_PIN, LOW);

  IBIS.begin(1200, SERIAL_7E2, IBIS_RX_PIN, IBIS_TX_PIN);
  Serial.print("Warte auf Anzeigen-Controller: ");
  Serial.print(DISPLAY_POWERUP_DELAY_MS / 1000);
  Serial.println(" Sekunden");
  delay(DISPLAY_POWERUP_DELAY_MS);

  sendDs009neu("ESP Start");

  if (connectWifi()) {
    startUdpIfReady();
  }
}

void loop() {
  handleSerialInput();
  maintainWifi();
  updateStreamSequence();
  updateIpRepeats();

  if (udpStarted) {
    handleUdpPacket();
  }
}
