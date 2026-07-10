# AGENTS.md – IBIS / LED-Dotmatrix Haltestellenanzeige

## Projektziel

Dieses Projekt steuert eine LED-Dotmatrix-Haltestellenanzeige aus einem Bus über ein IBIS-kompatibles serielles Protokoll an. Als Controller wird ein **ESP32-C3 Super Mini** verwendet. Der ESP hängt direkt am RX-Eingang des Controllers auf dem IBIS-Board der Anzeige, nicht am originalen RS422/IBIS-Bus.

Die Anzeige konnte erfolgreich mit **WinIBIS** getestet werden. Der finale funktionierende Datensatz basiert auf **DS009neu**.

## Hardware

### Controller

- Board: ESP32-C3 Super Mini
- Framework: Arduino / ESP32 Arduino Core
- UART zur Anzeige: `HardwareSerial IBIS(1)`
- TX-Pin zur Anzeige: `GPIO4`
- RX-Pin: nicht benötigt, aktuell `-1`
- Pegel: 3,3 V TTL direkt zum Controller-RX des IBIS-Boards
- GND von ESP und Anzeige müssen verbunden sein

### Verdrahtung

```text
ESP32-C3 Super Mini        IBIS-Controller / Anzeigenboard
---------------------------------------------------------
GND                       GND
GPIO4 / UART TX           RX-Eingang am Controller
```

Empfohlen ist ein Serienwiderstand zwischen ESP und Controller-RX:

```text
GPIO4 -> 1 kΩ bis 4,7 kΩ -> Controller-RX
```

Nicht direkt auf den originalen RS422/IBIS-Bus gehen. Dieser Code erzeugt TTL-UART, keinen RS422-Pegel.

## Serielle Schnittstelle

Die funktionierende Übertragung zur Anzeige ist:

```text
1200 Baud
7 Datenbits
Even Parity
2 Stopbits
nicht invertiert
```

Arduino-Konfiguration:

```cpp
IBIS.begin(1200, SERIAL_7E2, IBIS_RX_PIN, IBIS_TX_PIN);
```

## Funktionierendes Telegrammformat

Die Anzeige verwendet für die Textanzeige einen DS009neu-artigen Datensatz.

Der funktionierende Aufbau ist:

```text
v + 20 Zeichen Textfeld + CR + BCC
```

Wichtig:

- Das erste Byte ist immer ASCII `v` (`0x76`).
- Danach folgen exakt **20 sichtbare Textzeichen**.
- Kürzere Texte müssen rechts mit echten Leerzeichen `0x20` aufgefüllt werden.
- Das Textfeld darf nicht mit `\0` oder zufälligen Speicherinhalten aufgefüllt werden.
- Danach folgt `CR` (`0x0D`).
- Danach folgt ein BCC-Prüfzeichen.

Beispiel für `TEST`:

```text
vTEST________________
```

Die Unterstriche stehen nur für die Darstellung. Gesendet werden echte Leerzeichen.

Hex-Beispiel:

```text
76-54-45-53-54-20-20-20-20-20-20-20-20-20-20-20-20-20-20-20-20-0D-BCC
```

Beispiel für exakt 20 Zeichen:

```text
v12345678901234567890<CR><BCC>
```

WinIBIS zeigte dafür:

```text
76-31-32-33-34-35-36-37-38-39-30-31-32-33-34-35-36-37-38-39-30-0D-35
```

## BCC-Berechnung

Die funktionierende BCC-Berechnung ist:

```text
BCC = 0x7F XOR XOR(Payload + CR)
```

Dabei ist `Payload` der komplette Nutzdatenbereich vor dem CR, also:

```text
v + 20 Zeichen Textfeld
```

C++-Funktion:

```cpp
uint8_t calcIbisBcc(const uint8_t *payload, size_t len) {
  uint8_t x = 0x00;

  for (size_t i = 0; i < len; i++) {
    x ^= payload[i];
  }

  x ^= 0x0D;
  return 0x7F ^ x;
}
```

## Aktueller funktionierender Basissketch

Dieser Sketch ist die stabile Grundlage. Änderungen bitte vorsichtig und minimal halten.

```cpp
#include <Arduino.h>

static constexpr int IBIS_TX_PIN = 4;
static constexpr int IBIS_RX_PIN = -1;

HardwareSerial IBIS(1);

static constexpr int TEXT_FIELD_LEN = 20;

uint8_t calcIbisBcc(const uint8_t *payload, size_t len) {
  uint8_t x = 0x00;

  for (size_t i = 0; i < len; i++) {
    x ^= payload[i];
  }

  x ^= 0x0D;
  return 0x7F ^ x;
}

void makeDs009neuPayload(uint8_t *out, size_t outSize, const char *text) {
  // Aufbau:
  // v + 20 Zeichen Text
  // Also exakt 21 Bytes Payload ohne CR/BCC

  if (outSize < 1 + TEXT_FIELD_LEN) {
    return;
  }

  out[0] = 'v';

  size_t textLen = strlen(text);

  for (int i = 0; i < TEXT_FIELD_LEN; i++) {
    if ((size_t)i < textLen) {
      out[1 + i] = (uint8_t)text[i];
    } else {
      out[1 + i] = 0x20;  // echtes Leerzeichen
    }
  }
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

void setup() {
  Serial.begin(115200);
  delay(1000);

  Serial.println();
  Serial.println("ESP32-C3 IBIS DS009neu Sender");
  Serial.println("TX GPIO4");
  Serial.println("1200 Baud, 7E2");

  IBIS.begin(1200, SERIAL_7E2, IBIS_RX_PIN, IBIS_TX_PIN);
}

void loop() {
  sendDs009neu("Hauptbahnhof");
  delay(3000);

  sendDs009neu("12345678901234567890");
  delay(3000);

  sendDs009neu("TEST");
  delay(3000);
}
```

## Bekannte Fallstricke

### 1. Die `1` ist kein Steuerzeichen

Bei einem früheren WinIBIS-Test wurde `1Hauptbahnhof` gesendet. Die `1` war Teil des Textes und wurde von der Anzeige auch gedruckt. Deshalb darf keine zusätzliche Feldnummer nach dem `v` eingefügt werden.

Falsch:

```text
v1Hauptbahnhof...
```

Richtig:

```text
vHauptbahnhof...
```

### 2. Textfeld muss exakt 20 Zeichen lang sein

Kurze Texte müssen mit Leerzeichen aufgefüllt werden. Sonst bleiben alte Zeichen auf der Anzeige stehen oder der Controller liest zufällige Speicherinhalte.

Falsch:

```cpp
if (text[i] != '\0') out[pos++] = text[i];
else out[pos++] = ' ';
```

Dieser Ansatz liest nach dem Stringende weiter aus dem Speicher.

Richtig:

```cpp
size_t textLen = strlen(text);

for (int i = 0; i < TEXT_FIELD_LEN; i++) {
  if ((size_t)i < textLen) {
    out[1 + i] = (uint8_t)text[i];
  } else {
    out[1 + i] = 0x20;
  }
}
```

### 3. Keine zI/zl/z1-Datensätze verwenden

Frühere Tests mit `zI5`, `zl6`, `z16` usw. waren nicht der funktionierende Weg für diese Anzeige. Der erfolgreiche Weg ist `v + 20 Zeichen + CR + BCC`.

### 4. Kein automatisches Nullbyte senden

Das Payload wird als Bytearray mit fester Länge gesendet:

```cpp
IBIS.write(payload, sizeof(payload));
```

Nicht als C-String mit `print()`, wenn dadurch Längen- oder Terminierungsprobleme entstehen könnten.

## Codex-Arbeitsregeln

Bitte bei Änderungen beachten:

1. Den funktionierenden Telegrammaufbau nicht ohne Grund ändern.
2. `TEXT_FIELD_LEN = 20` beibehalten, solange keine andere Anzeigevariante unterstützt wird.
3. BCC-Formel beibehalten: `0x7F ^ XOR(payload + CR)`.
4. UART-Parameter beibehalten: `1200, SERIAL_7E2`.
5. TX-Pin `GPIO4` beibehalten, sofern keine Hardwareänderung ausdrücklich gewünscht ist.
6. Kürzere Texte immer mit echten Leerzeichen `0x20` auffüllen.
7. Keine Umlaute verwenden, solange der Zeichensatz der Anzeige nicht geklärt ist.
8. Debug-Ausgaben dürfen erweitert werden, aber die gesendeten Bytes dürfen dadurch nicht verändert werden.
9. Änderungen bevorzugt klein und nachvollziehbar halten.
10. Wenn neue Funktionen ergänzt werden, zuerst die bestehende Funktion `sendDs009neu()` als stabile Basisschnittstelle erhalten.

## Mögliche nächste Erweiterungen

Sinnvolle nächste Schritte für Codex:

- Text über USB-Serial eingeben und auf der Anzeige ausgeben.
- Lange Texte als Laufschrift über 20 Zeichen scrollen.
- Vordefinierte Haltestellenliste im Flash speichern.
- Automatischer Wechsel zwischen mehreren Haltestellen.
- Webinterface oder WLAN-Steuerung für die Anzeige.
- UDP-Steuerung, z. B. `TEXT Hauptbahnhof`.
- MQTT-Anbindung für Home Assistant.
- UTF-8/Umlaut-Transliteration, z. B. `ä -> ae`, `ö -> oe`, `ü -> ue`, `ß -> ss`.

## Testreferenz

Die Anzeige funktioniert nachweislich mit folgendem Test:

```cpp
sendDs009neu("Hauptbahnhof");
sendDs009neu("12345678901234567890");
sendDs009neu("TEST");
```

Erwartung:

- `Hauptbahnhof` wird angezeigt und der Rest der Zeile ist leer.
- `12345678901234567890` füllt die komplette Anzeige mit 20 Zeichen.
- `TEST` löscht alte Reste durch Padding mit Leerzeichen.
