/* plant_sensor.ino
 * Minimal Nano 33 IoT firmware that connects to a WiFi AP and sends
 * "mac value" UDP packets containing the device MAC and the voltage
 * read from A0. It also prints the MAC on serial for registration.
 *
 * Configure SSID/password by defining WIFI_SSID and WIFI_PASS before
 * compiling. If not defined, defaults are used (SSID: "PiTestAP",
 * PASS: "doublepump").
 */

#include "config.h"
#include <WiFiNINA.h>
#include <WiFiUdp.h>
#include <FlashStorage.h>

#ifndef WIFI_SSID
#define WIFI_SSID "PiTestAP"
#endif
#ifndef WIFI_PASS
#define WIFI_PASS "doublepump"
#endif

const char* ssid = WIFI_SSID;
const char* pass = WIFI_PASS;

WiFiUDP Udp;
unsigned int localPort = 12345;

char macStr[32];
IPAddress targetIp; /* global target IP for UDP debug messages */
// Persisted threshold using FlashStorage (works on SAMD)
FlashStorage(thresholdStore, uint8_t);
static uint8_t stored_threshold = 50;

// Control pin for remote commands. Default to D2 (safer than D0 which is Serial RX).
#ifndef CONTROL_PIN
#define CONTROL_PIN 2
#endif

void setup() {
  Serial.begin(115200);
  while (!Serial && millis() < 5000) ;
  // ensure the built-in LED is available for loop heartbeat
  pinMode(LED_BUILTIN, OUTPUT);

  // Print a startup message
  Serial.println("Plant sensor starting...");
  // Unique banner to verify firmware version running on device
  Serial.println("PLANT_SENSOR_BIN:v2");

  // Connect to WiFi (retry every 5 seconds indefinitely)
  int status = WL_IDLE_STATUS;
  Serial.print("Connecting to "); Serial.println(ssid);
  int attempt = 0;
  
  while (status != WL_CONNECTED) {
    attempt++;
    Serial.print("WiFi attempt #"); Serial.println(attempt);
    status = WiFi.begin(ssid, pass);
    
    if (status == WL_CONNECTED) {
      IPAddress ip = WiFi.localIP();
      Serial.print("Connected, IP: "); Serial.println(ip);
      break;
    } else {
      Serial.print("WiFi.status() -> "); Serial.println(status);
      delay(5000); // Retry every 5 seconds
    }
  }

  // MAC
  byte mac[6];
  WiFi.macAddress(mac);
  snprintf(macStr, sizeof(macStr), "%02x:%02x:%02x:%02x:%02x:%02x",
           mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
  Serial.print("MAC: "); Serial.println(macStr);

  Udp.begin(localPort);
  Serial.println("UDP socket started");
  // initialize targetIp once in setup
#ifndef TARGET_IP
  targetIp = IPAddress(192,168,50,1);
#else
  {
    const char *t = TARGET_IP;
    int a,b,c,d;
    if (sscanf(t, "%d.%d.%d.%d", &a,&b,&c,&d) == 4) targetIp = IPAddress(a,b,c,d);
    else targetIp = IPAddress(255,255,255,255);
  }
#endif

  // Ensure control pin is available and set as output
  pinMode(CONTROL_PIN, OUTPUT);
  digitalWrite(CONTROL_PIN, LOW);
  // Print initial control pin state for debugging
  Serial.print("CONTROL_PIN (D"); Serial.print(CONTROL_PIN); Serial.print(") initial state: ");
  Serial.println(digitalRead(CONTROL_PIN) == HIGH ? "HIGH" : "LOW");

  // Read persisted threshold from flash storage
  uint8_t t = thresholdStore.read();
  if (t <= 100) stored_threshold = t; // accept only sane values
  Serial.print("STORED_THRESHOLD (from flash): "); Serial.println(stored_threshold);
  // also notify gateway with stored threshold for debug
  {
    char dbuf[128]; int n = snprintf(dbuf, sizeof(dbuf), "%s STORED_THRESHOLD %d", macStr, stored_threshold);
    Udp.beginPacket(targetIp, localPort); Udp.write((const uint8_t*)dbuf, n); Udp.endPacket();
  }

  Serial.println("setup complete");
}

/* Send a debug string over UDP to the gateway (prefix not added). */
void udp_debug(const char *msg) {
  if (!msg) return;
  Udp.beginPacket(targetIp, localPort);
  Udp.write((const uint8_t*)msg, strlen(msg));
  Udp.endPacket();
}

void loop() {
  // Check WiFi connection; mirror setup-style reconnect without scans/resets
  static unsigned long last_wifi_check = 0;
  static bool attempting_reconnect = false;
  static unsigned long attempt_start = 0;
  static uint32_t attempt_counter = 0;
  if (WiFi.status() != WL_CONNECTED) {
    // Start an attempt every 5 seconds if not already trying
    if (!attempting_reconnect && millis() - last_wifi_check > 5000) {
      last_wifi_check = millis();
      attempting_reconnect = true;
      attempt_start = millis();
      attempt_counter++;
      Serial.println("WiFi reconnect: begin");
      // Stop UDP to avoid socket residue during rejoin
      Udp.stop();
      // On every attempt, fully end the WiFi stack before begin (mirrors cold boot)
      WiFi.end();
      delay(200);
      // Also clear stored network info every second attempt to purge stale state
      if (attempt_counter % 2 == 0) {
        Serial.println("WiFi.disconnect(true) before begin");
        WiFi.disconnect(true);
        delay(200);
      }
      WiFi.begin(ssid, pass);
    }
    // Poll for up to 20s, like setup()
    if (attempting_reconnect) {
      if (WiFi.status() == WL_CONNECTED) {
        attempting_reconnect = false;
        IPAddress ip = WiFi.localIP();
        Serial.print("Reconnected, IP: "); Serial.println(ip);
        Udp.stop();
        Udp.begin(localPort);
        char onbuf[64];
        int nn = snprintf(onbuf, sizeof(onbuf), "%s ONLINE", macStr);
        Udp.beginPacket(targetIp, localPort);
        Udp.write((const uint8_t*)onbuf, nn);
        Udp.endPacket();
      } else if (millis() - attempt_start > 20000) {
        // Give up this attempt and will retry on next 5s tick
        attempting_reconnect = false;
        Serial.println("WiFi reconnect: attempt timed out");
      }
    }
  } else {
    attempting_reconnect = false;
  }

  // Read analog value from A0 -> map to voltage (0..3.3V). Nano 33 ADC ref is 3.3V.
  int raw = analogRead(A0);
  const float ADC_MAX = 1023.0f;
  float voltage = (raw / ADC_MAX) * 3.3f;

  // Convert voltage to inverted percent (3.3V -> 0%, 0V -> 100%)
  int percent = 0;
  if (voltage <= 0.0f) percent = 100;
  else if (voltage >= 3.3f) percent = 0;
  else percent = (int)roundf((1.0f - (voltage / 3.3f)) * 100.0f);

  // Send "mac value" as text via UDP to the Pi gateway by default.
  // Default target IP is 192.168.50.1 (Pi hotspot gateway). Can be
  // overridden at build time by defining TARGET_IP.
#ifndef TARGET_IP
  const IPAddress targetIp(192,168,50,1);
#else
  IPAddress targetIp;
  {
    // TARGET_IP expected as string literal like "192.168.50.1"
    const char *t = TARGET_IP;
    int a,b,c,d;
    if (sscanf(t, "%d.%d.%d.%d", &a,&b,&c,&d) == 4) {
      targetIp = IPAddress(a,b,c,d);
    } else {
      targetIp = IPAddress(255,255,255,255);
    }
  }
#endif

  char buf[64];
  // Format the float into a string using integer math (millivolts)
  char vbuf[16];
  int milliv = (int)roundf(voltage * 1000.0f);
  int v_int = milliv / 1000;
  int v_frac = abs(milliv % 1000);
  snprintf(vbuf, sizeof(vbuf), "%d.%03d", v_int, v_frac);
  int n = snprintf(buf, sizeof(buf), "%s %s", macStr, vbuf);
  if (WiFi.status() == WL_CONNECTED) {
    Udp.beginPacket(targetIp, 12345);
    Udp.write((const uint8_t*)buf, n);
    Udp.endPacket();
  }

  // Before building the debug block, ensure the control pin reflects the
  // configured threshold. If the automatic decision changes the pin state
  // notify the gateway with a short CMD-style packet so the server parser
  // can update the UI immediately.
  int current_state = digitalRead(CONTROL_PIN) == HIGH ? 1 : 0;
  int desired_state = (percent < stored_threshold) ? 1 : 0;
  if (current_state != desired_state) {
    digitalWrite(CONTROL_PIN, desired_state ? HIGH : LOW);
    // Send a compact ACK/notification that matches the server's parser
    char ackpkt[64];
    int an = snprintf(ackpkt, sizeof(ackpkt), "CMD: set D%d = %d", CONTROL_PIN, desired_state);
    Udp.beginPacket(targetIp, localPort);
    Udp.write((const uint8_t*)ackpkt, an);
    Udp.endPacket();
    Serial.print("AUTOCMD: "); Serial.println(ackpkt);
  }

  // Build the multi-line debug block (same content as Serial prints) and send over UDP
  char dbg[512];
  int dn = snprintf(dbg, sizeof(dbg),
    "=== LOOP START v2 ===\n"
    "time(ms): %lu\n"
    "MAC: %s\n"
    "RAW: %d\n"
    "V: %.3f V\n"
    "mV: %d mV\n"
    "PCT: %d %%\n"
    "UDP: %s\n"
    "CONTROL_PIN (D%d) state: %s\n"
    "STORED_THRESHOLD: %d\n"
    "=== LOOP END ===\n",
    millis(), macStr, raw, voltage, milliv, percent, buf, CONTROL_PIN, digitalRead(CONTROL_PIN) == HIGH ? "HIGH" : "LOW", stored_threshold);
  // Send debug over UDP
  if (WiFi.status() == WL_CONNECTED) udp_debug(dbg);
  // Also print locally to Serial for USB-attached debugging
  Serial.print(dbg);
  Serial.flush();

  // Blink the onboard LED briefly so you can see loop activity even if serial is problematic
  digitalWrite(LED_BUILTIN, HIGH);
  delay(50);
  digitalWrite(LED_BUILTIN, LOW);

  delay(2000);

  // Non-blocking: check for incoming UDP control packets and act on them
  int packetSize = (WiFi.status() == WL_CONNECTED) ? Udp.parsePacket() : 0;
  if (packetSize > 0) {
    char cmdbuf[64];
    int len = Udp.read(cmdbuf, sizeof(cmdbuf) - 1);
    if (len > 0) {
      cmdbuf[len] = '\0';
      // Accept commands like: "D0 1" or "D0 0" or "D0,1"
      int pin = -1, val = -1;
      if (sscanf(cmdbuf, "D%d %d", &pin, &val) == 2 || sscanf(cmdbuf, "D%d,%d", &pin, &val) == 2 || sscanf(cmdbuf, "D%d:%d", &pin, &val) == 2) {
        if (pin == CONTROL_PIN && (val == 0 || val == 1)) {
          digitalWrite(CONTROL_PIN, val ? HIGH : LOW);
          Serial.print("CMD: set D"); Serial.print(pin); Serial.print(" = "); Serial.println(val);
          // Optional: send an ACK back to gateway (same port)
          char ack[64]; int n = snprintf(ack, sizeof(ack), "%s CMD D%d %d", macStr, pin, val);
          Udp.beginPacket(Udp.remoteIP(), Udp.remotePort());
          Udp.write((const uint8_t*)ack, n);
          Udp.endPacket();
        }
      }
      else {
        // Accept threshold update commands: "THRESHOLD <n>", "TH <n>", or "T <n>"
        int tval = -1;
        if (sscanf(cmdbuf, "THRESHOLD %d", &tval) == 1 || sscanf(cmdbuf, "TH %d", &tval) == 1 || sscanf(cmdbuf, "T %d", &tval) == 1) {
          if (tval < 0) tval = 0; if (tval > 100) tval = 100;
          stored_threshold = (uint8_t)tval;
          thresholdStore.write(stored_threshold);
          Serial.print("Saved STORED_THRESHOLD = "); Serial.println(stored_threshold);
          char ack2[64]; int n2 = snprintf(ack2, sizeof(ack2), "%s STORED_THRESHOLD %d", macStr, stored_threshold);
          Udp.beginPacket(Udp.remoteIP(), Udp.remotePort()); Udp.write((const uint8_t*)ack2, n2); Udp.endPacket();
        }
      }
    }
  }

  // No watchdog reboot: focus on reliable reconnect behavior like setup()
}
