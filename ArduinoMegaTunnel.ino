#include "WiFiEsp.h"

char ssid[] = ".";
char pass[] = "heheheha";

WiFiEspClient client;
WiFiEspServer server(80);

bool esp_ok = false;
bool wifi_ok = false;
bool registered = false;

unsigned long lastRegisterTry = 0;
unsigned long lastBlink = 0;
bool blinkState = false;

#define PIN_A 8
#define PIN_B 9
#define PIN_RELAY 7

void setup() {
  Serial.begin(9600);
  Serial1.begin(115200);

  pinMode(PIN_A, OUTPUT);
  pinMode(PIN_B, OUTPUT);
  pinMode(PIN_RELAY, OUTPUT);

  digitalWrite(PIN_A, LOW);
  digitalWrite(PIN_B, LOW);
  digitalWrite(PIN_RELAY, LOW);

  WiFi.init(&Serial1);

  if (WiFi.status() == WL_NO_SHIELD) {
    Serial.println("ESP NICHT GEFUNDEN!");
    esp_ok = false;
    return;
  }

  esp_ok = true;
  Serial.println("ESP OK");

  Serial.println("Verbinde WLAN...");
  WiFi.begin(ssid, pass);

  unsigned long start = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - start < 15000) {
    delay(500);
    Serial.print(".");
  }

  if (WiFi.status() == WL_CONNECTED) {
    wifi_ok = true;
    Serial.println("\nWLAN OK");
    Serial.print("IP: ");
    Serial.println(WiFi.localIP());
    server.begin();
  } else {
    Serial.println("\nWLAN FEHLER");
    wifi_ok = false;
  }
}

void loop() {
  if (!esp_ok) {
    blinkError();
    return;
  }

  if (wifi_ok && !registered && millis() - lastRegisterTry > 10000) {
    lastRegisterTry = millis();
    tryRegister();
  }

  if (wifi_ok) {
    WiFiEspClient webClient = server.available();
    if (webClient) {
      handleRequest(webClient);
    }
  }
}

// ================= ERROR BLINK =================

void blinkError() {
  if (millis() - lastBlink > 1000) {
    lastBlink = millis();
    blinkState = !blinkState;
    digitalWrite(PIN_A, blinkState);
  }
}

// ================= REGISTER =================

void tryRegister() {
  Serial.println("Versuche Registrierung...");

  if (client.connect("orangepi.local", 80)) {
    client.println("GET /api/register/?device=Mega-Tunnel HTTP/1.1");
    client.println("Host: orangepi.local");
    client.println("Connection: close");
    client.println();

    String response = "";
    while (client.connected()) {
      while (client.available()) {
        char c = client.read();
        response += c;
      }
    }
    client.stop();

    if (response.indexOf("\"status\": \"ok\"") != -1) {
      registered = true;
      Serial.println("REGISTRIERT!");
    } else {
      Serial.println("Registrierung fehlgeschlagen");
    }
  } else {
    Serial.println("OrangePi nicht erreichbar");
  }
}

// ================= SERVER =================

void handleRequest(WiFiEspClient &c) {
  String req = "";
  while (c.connected() && c.available()) {
    char ch = c.read();
    req += ch;
  }

  // ---- /api/tunnelleds ----
  if (req.indexOf("GET /api/tunnelleds") != -1) {
    sendJson(c, 200, "{\"status\":\"ok\"}");
    blinkTransistors();
  }

  // ---- /api/tunnel ----
  else if (req.indexOf("GET /api/tunnel") != -1) {
    sendJson(c, 200, "{\"status\":\"ok\"}");
    tunnelSequence();
  }

  // ---- /api/nebel ----
  else if (req.indexOf("GET /api/nebel") != -1) {
    float duration = parseDuration(req);
    sendJson(c, 200, "{\"status\":\"ok\"}");
    triggerRelay(duration);
  }

  // ---- /api/checkconnection ----
  else if (req.indexOf("GET /api/checkconnection") != -1) {
    if (esp_ok && wifi_ok && registered) {
      sendJson(c, 200, "{\"status\":\"ok\"}");
    } else {
      sendJson(c, 500, "{\"status\":\"error\"}");
    }
  }

  else {
    c.print("HTTP/1.1 404 Not Found\r\n\r\n");
  }

  delay(10);
  c.stop();
}

// ================= LED EFFECT =================

void blinkTransistors() {
  unsigned long start = millis();

  while (millis() - start < 5000) {
    digitalWrite(PIN_A, HIGH);
    digitalWrite(PIN_B, LOW);
    delay(250);

    digitalWrite(PIN_A, LOW);
    digitalWrite(PIN_B, HIGH);
    delay(250);
  }

  digitalWrite(PIN_A, LOW);
  digitalWrite(PIN_B, LOW);
}

// ================= TUNNEL SEQUENCE =================

void tunnelSequence() {
  // Nebel an
  digitalWrite(PIN_RELAY, HIGH);

  // 1s warten
  delay(1000);

  // LEDs starten (5s)
  unsigned long ledStart = millis();
  bool fogOffDone = false;

  while (millis() - ledStart < 5000) {
    digitalWrite(PIN_A, HIGH);
    digitalWrite(PIN_B, LOW);
    delay(250);

    digitalWrite(PIN_A, LOW);
    digitalWrite(PIN_B, HIGH);
    delay(250);

    // nach insgesamt 2s Nebel aus
    if (!fogOffDone && millis() - ledStart >= 1000) {
      digitalWrite(PIN_RELAY, LOW);
      fogOffDone = true;
    }
  }

  digitalWrite(PIN_A, LOW);
  digitalWrite(PIN_B, LOW);
}

// ================= NEBEL =================

float parseDuration(String req) {
  int idx = req.indexOf("duration=");
  if (idx == -1) return 1.0;

  String sub = req.substring(idx + 9);
  int end = sub.indexOf(" ");
  if (end != -1) sub = sub.substring(0, end);

  sub.replace(",", ".");
  return sub.toFloat();
}

void triggerRelay(float seconds) {
  unsigned long ms = (unsigned long)(seconds * 1000);

  digitalWrite(PIN_RELAY, HIGH);
  delay(ms);
  digitalWrite(PIN_RELAY, LOW);
}

// ================= UTILS =================

void sendJson(WiFiEspClient &c, int code, const char* body) {
  c.print("HTTP/1.1 ");
  c.print(code);
  if (code == 200) c.print(" OK\r\n");
  else c.print(" ERROR\r\n");

  c.print("Content-Type: application/json\r\n");
  c.print("Connection: close\r\n\r\n");
  c.print(body);
}
