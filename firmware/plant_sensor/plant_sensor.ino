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

void setup() {
  Serial.begin(115200);
  while (!Serial && millis() < 5000) ;
  // ensure the built-in LED is available for loop heartbeat
  pinMode(LED_BUILTIN, OUTPUT);

  // Print a startup message
  Serial.println("Plant sensor starting...");
  // Unique banner to verify firmware version running on device
  Serial.println("PLANT_SENSOR_BIN:v2");

  // Connect to WiFi
  int status = WL_IDLE_STATUS;
  unsigned long start = millis();
  Serial.print("Connecting to "); Serial.println(ssid);
  int attempt = 0;
  while (status != WL_CONNECTED && millis() - start < 20000) {
    attempt++;
    Serial.print("WiFi.begin attempt #"); Serial.println(attempt);
    status = WiFi.begin(ssid, pass);
    Serial.print("WiFi.status() -> "); Serial.println(status);
    delay(1000);
  }

  if (status == WL_CONNECTED) {
    IPAddress ip = WiFi.localIP();
    Serial.print("Connected, IP: "); Serial.println(ip);
  } else {
    Serial.println("WiFi connect failed");
  }

  // MAC
  byte mac[6];
  WiFi.macAddress(mac);
  snprintf(macStr, sizeof(macStr), "%02x:%02x:%02x:%02x:%02x:%02x",
           mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
  Serial.print("MAC: "); Serial.println(macStr);

  Udp.begin(localPort);
  Serial.println("UDP socket started");

  Serial.println("setup complete");
}

void loop() {
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
  Udp.beginPacket(targetIp, 12345);
  Udp.write((const uint8_t*)buf, n);
  Udp.endPacket();

  // Debug: print clear multi-line output so values can't be mistaken
  Serial.println("=== LOOP START v2 ===");
  Serial.print("time(ms): "); Serial.println(millis());
  Serial.print("MAC: "); Serial.println(macStr);
  Serial.print("RAW: "); Serial.println(raw);
  Serial.print("V: "); Serial.print(voltage, 3); Serial.println(" V");
  Serial.print("mV: "); Serial.print(milliv); Serial.println(" mV");
  Serial.print("PCT: "); Serial.print(percent); Serial.println(" %");
  Serial.print("UDP: "); Serial.println(buf);
  Serial.println("=== LOOP END ===");

  // Flush the serial buffer to make sure all bytes are sent
  Serial.flush();

  // Blink the onboard LED briefly so you can see loop activity even if serial is problematic
  digitalWrite(LED_BUILTIN, HIGH);
  delay(50);
  digitalWrite(LED_BUILTIN, LOW);

  delay(2000);
}
