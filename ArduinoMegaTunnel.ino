#include "WiFiEsp.h"

char ssid[] = "rescuerobotcar";
char pass[] = "mint2025";

WiFiEspClient client;
WiFiEspServer server(80);

bool esp_ok = false;
bool wifi_ok = false;
bool registered = false;

unsigned long lastRegisterTry = 0;
unsigned long lastBlink = 0;
bool blinkState = false;

// Action flags for non-blocking execution
bool doBlink = false;
unsigned long doBlinkUntil = 0;
bool doTunnel = false;
unsigned long tunnelStartTime = 0;
unsigned long tunnelEndTime = 0;
bool tunnelRelayOffDone = false;
bool doTriggerRelay = false;
unsigned long triggerRelayUntil = 0;
unsigned long lastActionBlink = 0;
bool actionBlinkState = false;

#define PIN_A 8
#define PIN_B 9
#define PIN_RELAY 7

// dynamische IP des OrangePi, wird nur während Laufzeit gehalten
String orangepiIP = "";

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

  // execute background actions (non-blocking)
  pollActions();

  // Registrierung nur wenn WLAN OK und noch nicht registriert
  if (wifi_ok && !registered && millis() - lastRegisterTry > 10000) {
    lastRegisterTry = millis();
    findOrangePiAndRegister();
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

// ================= FIND ORANGEPI =================
void findOrangePiAndRegister() {
  Serial.println("Suche OrangePi im Netzwerk...");

  IPAddress localIP = WiFi.localIP();
  byte subnet = localIP[2]; // z.B. 192.168.X.Y
  byte base1 = localIP[0];
  byte base2 = localIP[1];

  for (int host = 180; host <= 254; host++) {
    String testIP = String(base1) + "." + String(base2) + "." + String(subnet) + "." + String(host);

    if (tryPingOrangePi(testIP)) {
      orangepiIP = testIP;
      Serial.print("OrangePi gefunden: ");
      Serial.println(orangepiIP);

      if (tryRegister()) {
        Serial.println("REGISTRIERT!");
      } else {
        Serial.println("Registrierung fehlgeschlagen");
      }
      return;
    }
  }

  Serial.println("OrangePi nicht gefunden.");
}

bool tryPingOrangePi(String ip) {
  if (client.connect(ip.c_str(), 80)) {
    client.println("GET /api/checkorangepi HTTP/1.1");
    client.print("Host: ");
    client.println(ip);
    client.println("Connection: close");
    client.println();

    unsigned long timeout = millis() + 2000;
    String response = "";
    // Read available data until timeout to handle servers that close connection early
    while (millis() < timeout) {
      while (client.available()) {
        char c = client.read();
        response += c;
      }
      // If connection closed and no more data, break early
      if (!client.connected() && client.available() == 0) break;
      delay(10);
    }
    client.stop();

    // Debug: show response so you can inspect why detection failed
    Serial.print("Ping response from ");
    Serial.print(ip);
    Serial.print(": ");
    Serial.println(response);

    // Accept common success indicators: HTTP 200, JSON status ok, or presence of the endpoint name
    if (response.indexOf("200 OK") != -1 || response.indexOf("HTTP/1.1 200") != -1 || response.indexOf("\"status\": \"ok\"") != -1 || response.indexOf("checkorangepi") != -1) {
      return true;
    }
  }
  return false;
}

// ================= REGISTER =================
bool tryRegister() {
  if (orangepiIP == "") return false;

  // Try a couple times in case the OrangePi is slow to respond
  for (int attempt = 0; attempt < 2; attempt++) {
    if (client.connect(orangepiIP.c_str(), 80)) {
      client.println("GET /api/register/?device=Mega-Tunnel HTTP/1.1");
      client.print("Host: ");
      client.println(orangepiIP);
      client.println("Connection: close");
      client.println();

      unsigned long timeout = millis() + 2000;
      String response = "";
      while (millis() < timeout) {
        while (client.available()) {
          char c = client.read();
          response += c;
        }
        if (!client.connected() && client.available() == 0) break;
        delay(10);
      }
      client.stop();

      Serial.print("Register response from ");
      Serial.print(orangepiIP);
      Serial.print(": ");
      Serial.println(response);

      if (response.indexOf("\"status\": \"ok\"") != -1 || response.indexOf("200 OK") != -1 || response.indexOf("200 ") != -1) {
        registered = true;
        return true;
      }
    }
    delay(500);
  }
  return false;
}

// ================= SERVER =================
void handleRequest(WiFiEspClient &c) {
  String req = "";
  // Read request with small timeout and stop at end of headers
  unsigned long start = millis();
  while (c.connected() && millis() - start < 500) {
    while (c.available()) {
      char ch = c.read();
      req += ch;
      start = millis(); // reset timeout on incoming data
      if (req.indexOf("\r\n\r\n") != -1) break;
    }
    if (req.indexOf("\r\n\r\n") != -1) break;
    delay(1);
  }

  // ---- /api/tunnelleds ----
  if (req.indexOf("GET /api/tunnelleds") != -1) {
    sendJson(c, 200, "{\"status\":\"ok\"}");
    // start non-blocking blink for 5s
    doBlink = true;
    doBlinkUntil = millis() + 5000;
    lastActionBlink = 0;
    actionBlinkState = false;
  }

  // ---- /api/tunnel ----
  else if (req.indexOf("GET /api/tunnel") != -1) {
    sendJson(c, 200, "{\"status\":\"ok\"}");
    // start non-blocking tunnel sequence
    doTunnel = true;
    tunnelStartTime = millis();
    tunnelEndTime = tunnelStartTime + 5000;
    tunnelRelayOffDone = false;
    digitalWrite(PIN_RELAY, HIGH);
    lastActionBlink = 0;
    actionBlinkState = false;
  }

  // ---- /api/nebel ----
  else if (req.indexOf("GET /api/nebel") != -1) {
    float duration = parseDuration(req);
    sendJson(c, 200, "{\"status\":\"ok\"}");
    // trigger relay non-blocking
    doTriggerRelay = true;
    triggerRelayUntil = millis() + (unsigned long)(duration * 1000);
    digitalWrite(PIN_RELAY, HIGH);
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
  digitalWrite(PIN_RELAY, HIGH);
  delay(1000);

  unsigned long ledStart = millis();
  bool fogOffDone = false;

  while (millis() - ledStart < 5000) {
    digitalWrite(PIN_A, HIGH);
    digitalWrite(PIN_B, LOW);
    delay(250);

    digitalWrite(PIN_A, LOW);
    digitalWrite(PIN_B, HIGH);
    delay(250);

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

// ================= ACTION POLLING =================
void pollActions() {
  unsigned long now = millis();

  // common blink for actions
  if ((doBlink && now < doBlinkUntil) || (doTunnel && now < tunnelEndTime)) {
    if (now - lastActionBlink >= 250) {
      lastActionBlink = now;
      actionBlinkState = !actionBlinkState;
      if (actionBlinkState) {
        digitalWrite(PIN_A, HIGH);
        digitalWrite(PIN_B, LOW);
      } else {
        digitalWrite(PIN_A, LOW);
        digitalWrite(PIN_B, HIGH);
      }
    }
  } else {
    // stop blinking when not needed
    if (doBlink || doTunnel) {
      digitalWrite(PIN_A, LOW);
      digitalWrite(PIN_B, LOW);
      actionBlinkState = false;
    }
    doBlink = false;
    if (doTunnel && now >= tunnelEndTime) {
      doTunnel = false;
      tunnelRelayOffDone = false;
    }
  }

  // tunnel relay off after 1s
  if (doTunnel && !tunnelRelayOffDone && now - tunnelStartTime >= 1000) {
    digitalWrite(PIN_RELAY, LOW);
    tunnelRelayOffDone = true;
  }

  // trigger relay for fog
  if (doTriggerRelay && now >= triggerRelayUntil) {
    doTriggerRelay = false;
    digitalWrite(PIN_RELAY, LOW);
  }
}

// ================= UTILS =================
void sendJson(WiFiEspClient &c, int code, const char* body) {
  c.print("HTTP/1.1 ");
  c.print(code);
  if (code == 200) c.print(" OK\r\n");
  else c.print(" ERROR\r\n");

  c.print("Content-Type: application/json\r\n");
  c.print("Content-Length: ");
  c.print(strlen(body));
  c.print("\r\n");
  c.print("Connection: close\r\n\r\n");
  c.print(body);
}
