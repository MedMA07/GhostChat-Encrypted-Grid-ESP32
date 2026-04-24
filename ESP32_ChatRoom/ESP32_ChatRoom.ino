// ╔══════════════════════════════════════════════════════════════╗
// ║          ESP32 Private Chat Room — Phase 1                   ║
// ║  WiFi AP + WebSocket Server + ST7735S TFT Display            ║
// ║                                                              ║
// ║  Libraries needed (install via Library Manager):             ║
// ║   • ESPAsyncWebServer  (by lacamera / ESP Async WebServer)   ║
// ║   • AsyncTCP           (by dvarrel)                          ║
// ║   • ArduinoJson        v6.x (by Benoit Blanchon)             ║
// ║   • Adafruit GFX Library                                     ║
// ║   • Adafruit ST7735 and ST7789 Library                       ║
// ╚══════════════════════════════════════════════════════════════╝

#include <Arduino.h>
#include <WiFi.h>
#include <AsyncTCP.h>
#include <ESPAsyncWebServer.h>
#include <ArduinoJson.h>
#include <Adafruit_GFX.h>
#include <Adafruit_ST7735.h>
#include <SPI.h>
#include <map>
#include <vector>

// ════════════════════════════════════════════════════════════════
//  ⚙️  CONFIG — Change these!
// ════════════════════════════════════════════════════════════════

#define WIFI_SSID   "ESP32_ChatRoom"   // WiFi network name (visible to users)
#define WIFI_PASS   "esp32chat"        // WiFi password    (min 8 characters)
#define ROOM_KEY    "vip2024"          // Secret room key  (users enter this)

// ════════════════════════════════════════════════════════════════
//  📌 PIN DEFINITIONS — ESP32 38-pin DevKit
// ════════════════════════════════════════════════════════════════

// ── ST7735S (SPI — VSPI) ────────────────────────────────────────
#define TFT_MOSI   23   // SDA
#define TFT_SCLK   18   // SCL
#define TFT_CS     13   // CS
#define TFT_DC      2   // DC
#define TFT_RST    15   // RST
// BLK → 220Ω → 3.3V  (no GPIO)

// ── NRF24L01 PA+LNA — Reserved for Phase 3 ─────────────────────
// #define NRF_MOSI   23   // shared SPI
// #define NRF_MISO   19
// #define NRF_SCK    18   // shared SPI
// #define NRF_CSN     5
// #define NRF_CE     17

// ════════════════════════════════════════════════════════════════
//  🎨 TFT Color Palette
// ════════════════════════════════════════════════════════════════
#define C_BG        0x0841   // Deep dark navy
#define C_ACCENT    0x07FF   // Cyan
#define C_GREEN     0x07E0   // Green
#define C_RED       0xF800   // Red
#define C_YELLOW    0xFFE0   // Yellow
#define C_WHITE     0xFFFF   // White
#define C_GRAY      0x8410   // Gray
#define C_CYAN      0x07FF  
#define C_PURPLE    0xA81F   // Purple

// ════════════════════════════════════════════════════════════════
//  📦 Globals
// ════════════════════════════════════════════════════════════════

Adafruit_ST7735 tft = Adafruit_ST7735(TFT_CS, TFT_DC, TFT_RST);
AsyncWebServer  server(80);
AsyncWebSocket  ws("/ws");

struct User {
    String   username;
    String   room;          // "lobby" or "private"
    bool     authenticated;
    uint32_t privateTarget; // ID of private chat partner (0 = none)
};

std::map<uint32_t, User> users;  // key = ws client id

uint8_t  totalMessages  = 0;
uint32_t lastTFTUpdate  = 0;
#define  TFT_REFRESH_MS  500

// ════════════════════════════════════════════════════════════════
//  🖥️  TFT FUNCTIONS
// ════════════════════════════════════════════════════════════════

void tftSplash() {
    tft.fillScreen(C_BG);

    // Title bar
    tft.fillRect(0, 0, 160, 18, C_ACCENT);
    tft.setTextColor(C_BG);
    tft.setTextSize(1);
    tft.setCursor(4, 5);
    tft.print("  ESP32 Chat Hub v1.0");

    // Subtitle
    tft.setTextColor(C_GRAY);
    tft.setCursor(4, 24);
    tft.print("Booting...");
}

void tftShowIP(IPAddress ip) {
    tft.fillRect(0, 20, 160, 12, C_BG);
    tft.setTextColor(C_GREEN);
    tft.setCursor(4, 22);
    tft.printf("IP: %s", ip.toString().c_str());

    tft.setTextColor(C_YELLOW);
    tft.setCursor(4, 34);
    tft.printf("SSID: %s", WIFI_SSID);

    tft.setTextColor(C_GRAY);
    tft.setCursor(4, 46);
    tft.print("Waiting for users...");
}

void tftUpdateDashboard() {
    // Count authenticated users
    int authCount = 0;
    for (auto& [id, u] : users) {
        if (u.authenticated) authCount++;
    }

    tft.fillScreen(C_BG);

    // ── Header bar ──
    tft.fillRect(0, 0, 160, 16, C_ACCENT);
    tft.setTextColor(C_BG);
    tft.setTextSize(1);
    tft.setCursor(3, 4);
    tft.print("ESP32 ChatRoom");

    // ── Stats row ──
    tft.fillRect(0, 17, 160, 14, 0x0020); // dark blue strip
    tft.setTextColor(C_GREEN);
    tft.setCursor(3, 21);
    tft.printf("Users: %d", authCount);

    tft.setTextColor(C_YELLOW);
    tft.setCursor(80, 21);
    tft.printf("Msgs: %d", totalMessages);

    // ── User list ──
    tft.setTextColor(C_GRAY);
    tft.setCursor(3, 35);
    tft.print("Online:");

    int y = 45;
    for (auto& [id, u] : users) {
        if (!u.authenticated) continue;
        if (y > 118) {
            tft.setTextColor(C_GRAY);
            tft.setCursor(3, y);
            tft.print("...");
            break;
        }

        // Color dot based on room
        uint16_t dotColor = (u.room == "lobby") ? C_GREEN : C_PURPLE;
        tft.fillCircle(7, y + 3, 3, dotColor);

        tft.setTextColor(C_WHITE);
        tft.setCursor(14, y);
        tft.printf("%-10s", u.username.substring(0, 10).c_str());

        tft.setTextColor(C_GRAY);
        tft.setCursor(96, y);
        if (u.room == "lobby") {
            tft.print("Lobby");
        } else {
            tft.print("PM");
        }

        y += 14;
    }

    if (authCount == 0) {
        tft.setTextColor(C_GRAY);
        tft.setCursor(3, 48);
        tft.print("No users yet...");
        tft.setCursor(3, 62);
        tft.print("Connect to WiFi:");
        tft.setTextColor(C_CYAN);
        tft.setCursor(3, 76);
        tft.printf("%s", WIFI_SSID);
    }

    // ── Bottom status bar ──
    tft.fillRect(0, 122, 160, 6, C_ACCENT);
}

// ════════════════════════════════════════════════════════════════
//  🔧 HELPER FUNCTIONS
// ════════════════════════════════════════════════════════════════

String getUsernameById(uint32_t id) {
    if (users.count(id)) return users[id].username;
    return "";
}

uint32_t getIdByUsername(const String& uname) {
    for (auto& [id, u] : users) {
        if (u.username == uname) return id;
    }
    return 0;
}

bool usernameExists(const String& uname) {
    for (auto& [id, u] : users) {
        if (u.authenticated && u.username == uname) return true;
    }
    return false;
}

// Send JSON to one client
void sendTo(uint32_t cid, JsonDocument& doc) {
    String out;
    serializeJson(doc, out);
    ws.text(cid, out);
}

// Broadcast JSON to all authenticated users in lobby
void broadcastLobby(JsonDocument& doc, uint32_t excludeId = 0) {
    String out;
    serializeJson(doc, out);
    for (auto& [id, u] : users) {
        if (u.authenticated && id != excludeId) {
            ws.text(id, out);
        }
    }
}

// Send updated user list to a specific client
void sendUsersList(uint32_t toId) {
    StaticJsonDocument<512> doc;
    doc["type"] = "users_list";
    JsonArray arr = doc.createNestedArray("users");
    for (auto& [id, u] : users) {
        if (u.authenticated && id != toId) {
            arr.add(u.username);
        }
    }
    sendTo(toId, doc);
}

// Broadcast updated user list to everyone
void broadcastUsersList() {
    for (auto& [id, u] : users) {
        if (u.authenticated) sendUsersList(id);
    }
}

// ════════════════════════════════════════════════════════════════
//  📨 WEBSOCKET EVENT HANDLER
// ════════════════════════════════════════════════════════════════

void onWsEvent(AsyncWebSocket* srv, AsyncWebSocketClient* client,
               AwsEventType type, void* arg, uint8_t* data, size_t len) {

    uint32_t cid = client->id();

    // ── CLIENT CONNECTED ─────────────────────────────────────────
    if (type == WS_EVT_CONNECT) {
        users[cid] = {"", "lobby", false, 0};
        Serial.printf("[WS] Client #%u connected from %s\n",
                      cid, client->remoteIP().toString().c_str());

        // Ask client to login
        StaticJsonDocument<64> req;
        req["type"] = "ask_login";
        sendTo(cid, req);
    }

    // ── CLIENT DISCONNECTED ──────────────────────────────────────
    else if (type == WS_EVT_DISCONNECT) {
        String uname = users[cid].username;
        bool wasAuth = users[cid].authenticated;
        uint32_t partnerID = users[cid].privateTarget;
        users.erase(cid);

        Serial.printf("[WS] Client #%u (%s) disconnected\n", cid, uname.c_str());

        if (wasAuth && uname.length() > 0) {
            // Notify private chat partner if any
            if (partnerID != 0 && users.count(partnerID)) {
                StaticJsonDocument<128> notif;
                notif["type"] = "partner_left";
                notif["username"] = uname;
                sendTo(partnerID, notif);
            }

            // Notify lobby
            StaticJsonDocument<128> notif;
            notif["type"] = "user_left";
            notif["username"] = uname;
            broadcastLobby(notif);

            broadcastUsersList();
            tftUpdateDashboard();
        }
    }

    // ── DATA RECEIVED ────────────────────────────────────────────
    else if (type == WS_EVT_DATA) {
        AwsFrameInfo* info = (AwsFrameInfo*)arg;
        if (!info->final || info->index != 0 || info->len != len) return;
        if (info->opcode != WS_TEXT) return;

        // Parse JSON
        StaticJsonDocument<512> doc;
        DeserializationError err = deserializeJson(doc, (char*)data, len);
        if (err) {
            Serial.printf("[WS] JSON parse error: %s\n", err.c_str());
            return;
        }

        String msgType = doc["type"].as<String>();

        // ── LOGIN ────────────────────────────────────────────────
        if (msgType == "login") {
            String pass  = doc["password"].as<String>();
            String uname = doc["username"].as<String>();

            // Trim + validate
            uname.trim();
            if (uname.length() < 2 || uname.length() > 12) {
                StaticJsonDocument<128> resp;
                resp["type"] = "login_fail";
                resp["msg"]  = "Username: 2–12 characters";
                sendTo(cid, resp);
                return;
            }

            // Check password
            if (pass != ROOM_KEY) {
                StaticJsonDocument<128> resp;
                resp["type"] = "login_fail";
                resp["msg"]  = "Wrong password!";
                sendTo(cid, resp);
                Serial.printf("[LOGIN] #%u failed — bad password\n", cid);
                return;
            }

            // Check username uniqueness
            if (usernameExists(uname)) {
                StaticJsonDocument<128> resp;
                resp["type"] = "login_fail";
                resp["msg"]  = "Username already taken!";
                sendTo(cid, resp);
                return;
            }

            // ✅ Login OK
            users[cid].username      = uname;
            users[cid].authenticated = true;
            users[cid].room          = "lobby";

            Serial.printf("[LOGIN] ✅ '%s' joined (#%u)\n", uname.c_str(), cid);

            // Confirm to new user
            StaticJsonDocument<128> resp;
            resp["type"]     = "login_ok";
            resp["username"] = uname;
            sendTo(cid, resp);

            // Send current users list to new user
            sendUsersList(cid);

            // Notify everyone else
            StaticJsonDocument<128> notif;
            notif["type"]     = "user_joined";
            notif["username"] = uname;
            broadcastLobby(notif, cid);

            // Update everyone's user list
            broadcastUsersList();
            tftUpdateDashboard();
        }

        // ── LOBBY MESSAGE ────────────────────────────────────────
        else if (msgType == "lobby_msg") {
            if (!users[cid].authenticated) return;

            String text = doc["text"].as<String>();
            text.trim();
            if (text.length() == 0 || text.length() > 200) return;

            String sender = users[cid].username;
            totalMessages++;

            Serial.printf("[LOBBY] %s: %s\n", sender.c_str(), text.c_str());

            StaticJsonDocument<384> resp;
            resp["type"] = "lobby_msg";
            resp["from"] = sender;
            resp["text"] = text;
            broadcastLobby(resp); // including sender (echo)

            tftUpdateDashboard();
        }

        // ── PRIVATE MESSAGE ──────────────────────────────────────
        else if (msgType == "private_msg") {
            if (!users[cid].authenticated) return;

            String toUser = doc["to"].as<String>();
            String text   = doc["text"].as<String>();
            text.trim();
            if (text.length() == 0 || text.length() > 200) return;

            String   sender   = users[cid].username;
            uint32_t targetId = getIdByUsername(toUser);

            if (targetId == 0) {
                StaticJsonDocument<128> err;
                err["type"] = "error";
                err["msg"]  = "User not found or offline";
                sendTo(cid, err);
                return;
            }

            totalMessages++;
            Serial.printf("[PM] %s → %s: %s\n", sender.c_str(), toUser.c_str(), text.c_str());

            // Update private target tracking
            users[cid].privateTarget          = targetId;
            users[targetId].privateTarget = cid;

            StaticJsonDocument<384> resp;
            resp["type"] = "private_msg";
            resp["from"] = sender;
            resp["to"]   = toUser;
            resp["text"] = text;
            String out;
            serializeJson(resp, out);

            ws.text(cid, out);      // Echo to sender
            ws.text(targetId, out); // Send to target

            tftUpdateDashboard();
        }

        // ── PING (keep-alive) ────────────────────────────────────
        else if (msgType == "ping") {
            StaticJsonDocument<32> pong;
            pong["type"] = "pong";
            sendTo(cid, pong);
        }
    }

    // ── WS ERROR ─────────────────────────────────────────────────
    else if (type == WS_EVT_ERROR) {
        Serial.printf("[WS] Error on client #%u\n", cid);
    }
}

// ════════════════════════════════════════════════════════════════
//  🌐 HTML PAGE (served at http://192.168.4.1)
// ════════════════════════════════════════════════════════════════

const char HTML_PAGE[] PROGMEM = R"rawhtml(
<!DOCTYPE html>
<html lang="en">
<head>
<meta charset="UTF-8">
<meta name="viewport" content="width=device-width, initial-scale=1.0, maximum-scale=1.0, user-scalable=no">
<title>ESP32 Chat</title>
<style>
@import url('https://fonts.googleapis.com/css2?family=Share+Tech+Mono&family=Rajdhani:wght@400;600;700&display=swap');

:root {
  --bg:        #080c10;
  --bg2:       #0d1219;
  --bg3:       #111820;
  --border:    #1a2535;
  --border2:   #243040;
  --cyan:      #00d4ff;
  --green:     #00e676;
  --red:       #ff4444;
  --yellow:    #ffcc00;
  --purple:    #b06aff;
  --text:      #c8d8e8;
  --text-dim:  #4a6070;
  --font-mono: 'Share Tech Mono', monospace;
  --font-ui:   'Rajdhani', sans-serif;
}

* { box-sizing: border-box; margin: 0; padding: 0; -webkit-tap-highlight-color: transparent; }

body {
  background: var(--bg);
  color: var(--text);
  font-family: var(--font-ui);
  height: 100dvh;
  overflow: hidden;
  display: flex;
  flex-direction: column;
}

/* ══════════════ LOGIN ══════════════ */
#login-screen {
  display: flex;
  align-items: center;
  justify-content: center;
  height: 100dvh;
  padding: 20px;
  background:
    radial-gradient(ellipse at 20% 50%, rgba(0,212,255,0.04) 0%, transparent 60%),
    radial-gradient(ellipse at 80% 20%, rgba(0,230,118,0.04) 0%, transparent 60%),
    var(--bg);
}

.login-card {
  width: 100%;
  max-width: 360px;
  background: var(--bg2);
  border: 1px solid var(--border2);
  border-radius: 4px;
  overflow: hidden;
  box-shadow: 0 0 40px rgba(0,212,255,0.06), 0 0 80px rgba(0,0,0,0.8);
}

.login-topbar {
  background: var(--border);
  border-bottom: 1px solid var(--border2);
  padding: 8px 14px;
  display: flex;
  align-items: center;
  gap: 6px;
}
.dot-r { width:10px;height:10px;border-radius:50%;background:#ff5f57; }
.dot-y { width:10px;height:10px;border-radius:50%;background:#ffbd2e; }
.dot-g { width:10px;height:10px;border-radius:50%;background:#28c840; }
.topbar-title {
  flex: 1;
  text-align: center;
  font-family: var(--font-mono);
  font-size: 11px;
  color: var(--text-dim);
  letter-spacing: 1px;
}

.login-body {
  padding: 28px 28px 32px;
}

.login-logo {
  text-align: center;
  margin-bottom: 6px;
}
.login-logo svg { width: 48px; height: 48px; }
.login-title {
  text-align: center;
  font-size: 22px;
  font-weight: 700;
  color: var(--cyan);
  letter-spacing: 2px;
  margin-bottom: 4px;
}
.login-sub {
  text-align: center;
  font-family: var(--font-mono);
  font-size: 11px;
  color: var(--text-dim);
  margin-bottom: 28px;
  letter-spacing: 1px;
}

.field-label {
  font-size: 11px;
  font-weight: 600;
  letter-spacing: 2px;
  color: var(--cyan);
  margin-bottom: 6px;
  display: block;
  text-transform: uppercase;
}
.field-wrap {
  display: flex;
  align-items: center;
  background: var(--bg);
  border: 1px solid var(--border2);
  border-radius: 3px;
  margin-bottom: 16px;
  transition: border-color 0.2s;
}
.field-wrap:focus-within { border-color: var(--cyan); }
.field-wrap span {
  padding: 0 10px;
  font-family: var(--font-mono);
  color: var(--cyan);
  font-size: 14px;
  border-right: 1px solid var(--border2);
}
.field-wrap input {
  flex: 1;
  background: transparent;
  border: none;
  outline: none;
  padding: 11px 12px;
  color: var(--text);
  font-family: var(--font-mono);
  font-size: 14px;
}
.field-wrap input::placeholder { color: var(--text-dim); }

.btn-join {
  width: 100%;
  background: linear-gradient(135deg, var(--cyan), #0090b3);
  border: none;
  border-radius: 3px;
  padding: 13px;
  color: var(--bg);
  font-family: var(--font-ui);
  font-size: 15px;
  font-weight: 700;
  letter-spacing: 3px;
  text-transform: uppercase;
  cursor: pointer;
  margin-top: 6px;
  transition: opacity 0.2s, transform 0.1s;
}
.btn-join:active { transform: scale(0.98); opacity: 0.9; }

.login-err {
  font-family: var(--font-mono);
  font-size: 12px;
  color: var(--red);
  text-align: center;
  margin-top: 14px;
  height: 16px;
  transition: opacity 0.3s;
}

/* ══════════════ CHAT SCREEN ══════════════ */
#chat-screen {
  display: none;
  flex-direction: column;
  height: 100dvh;
}

/* Header */
.hdr {
  background: var(--bg2);
  border-bottom: 1px solid var(--border2);
  padding: 0 14px;
  display: flex;
  align-items: center;
  gap: 10px;
  height: 46px;
  flex-shrink: 0;
}
.hdr-sig {
  width: 8px;
  height: 8px;
  border-radius: 50%;
  background: var(--green);
  box-shadow: 0 0 6px var(--green);
  animation: blink 2s ease-in-out infinite;
}
@keyframes blink { 50% { opacity: 0.4; } }
.hdr-title {
  font-family: var(--font-mono);
  font-size: 13px;
  color: var(--cyan);
  letter-spacing: 1px;
  flex: 1;
}
.hdr-badge {
  font-family: var(--font-mono);
  font-size: 11px;
  color: var(--text-dim);
  border: 1px solid var(--border2);
  border-radius: 20px;
  padding: 3px 10px;
}
.hdr-badge b { color: var(--green); }

/* Tabs */
.tabs {
  background: var(--bg2);
  border-bottom: 2px solid var(--border);
  display: flex;
  flex-shrink: 0;
}
.tab {
  padding: 10px 16px;
  font-size: 13px;
  font-weight: 600;
  letter-spacing: 1px;
  color: var(--text-dim);
  cursor: pointer;
  border-bottom: 2px solid transparent;
  margin-bottom: -2px;
  white-space: nowrap;
  text-transform: uppercase;
  transition: color 0.2s;
  display: flex;
  align-items: center;
  gap: 6px;
}
.tab:hover { color: var(--text); }
.tab.active { color: var(--cyan); border-bottom-color: var(--cyan); }
.badge {
  background: var(--red);
  color: white;
  font-size: 10px;
  font-family: var(--font-mono);
  border-radius: 10px;
  padding: 1px 5px;
  display: none;
  line-height: 1.4;
}
.badge.show { display: inline-block; }

/* Body */
.body {
  display: flex;
  flex: 1;
  overflow: hidden;
}

/* Sidebar */
.sidebar {
  width: 180px;
  background: var(--bg2);
  border-right: 1px solid var(--border);
  display: flex;
  flex-direction: column;
  flex-shrink: 0;
  overflow: hidden;
}
.sidebar-hdr {
  padding: 10px 12px 6px;
  font-size: 10px;
  font-weight: 700;
  letter-spacing: 2px;
  color: var(--text-dim);
  text-transform: uppercase;
  border-bottom: 1px solid var(--border);
}
.users-list {
  flex: 1;
  overflow-y: auto;
  padding: 6px 0;
}
.user-row {
  display: flex;
  align-items: center;
  gap: 8px;
  padding: 8px 12px;
  cursor: pointer;
  transition: background 0.15s;
}
.user-row:hover { background: var(--bg3); }
.user-dot { width:7px;height:7px;border-radius:50%;background:var(--green);flex-shrink:0; }
.user-name {
  flex: 1;
  font-size: 13px;
  font-weight: 600;
  overflow: hidden;
  text-overflow: ellipsis;
  white-space: nowrap;
}
.user-pm {
  font-size: 13px;
  opacity: 0;
  transition: opacity 0.15s;
}
.user-row:hover .user-pm { opacity: 1; }

/* Chat panel */
.panel {
  flex: 1;
  display: flex;
  flex-direction: column;
  overflow: hidden;
}

/* Private target bar */
.pm-bar {
  background: rgba(176,106,255,0.08);
  border-bottom: 1px solid rgba(176,106,255,0.25);
  padding: 8px 14px;
  font-size: 13px;
  color: var(--text-dim);
  display: none;
  align-items: center;
}
.pm-bar.show { display: flex; }
.pm-bar b { color: var(--purple); margin: 0 4px; }
.pm-bar .close-pm {
  margin-left: auto;
  cursor: pointer;
  color: var(--red);
  font-size: 16px;
  line-height: 1;
  padding: 0 4px;
}

/* Messages */
.msgs {
  flex: 1;
  overflow-y: auto;
  padding: 14px 16px;
  display: flex;
  flex-direction: column;
  gap: 10px;
  scroll-behavior: smooth;
}
.msgs::-webkit-scrollbar { width: 4px; }
.msgs::-webkit-scrollbar-thumb { background: var(--border2); border-radius: 2px; }

.msg-sys {
  text-align: center;
  font-family: var(--font-mono);
  font-size: 11px;
  color: var(--text-dim);
  padding: 2px 0;
}

.msg-item { display: flex; flex-direction: column; gap: 3px; }
.msg-meta {
  display: flex;
  align-items: baseline;
  gap: 8px;
}
.msg-author {
  font-size: 13px;
  font-weight: 700;
  color: var(--cyan);
  font-family: var(--font-mono);
}
.msg-author.is-me { color: var(--green); }
.msg-time {
  font-size: 10px;
  font-family: var(--font-mono);
  color: var(--text-dim);
}
.msg-pm-badge {
  font-size: 10px;
  font-family: var(--font-mono);
  color: var(--purple);
  border: 1px solid rgba(176,106,255,0.4);
  border-radius: 3px;
  padding: 0 5px;
  letter-spacing: 1px;
}
.msg-text {
  font-size: 15px;
  color: var(--text);
  line-height: 1.5;
  word-break: break-word;
}

/* Input */
.input-bar {
  background: var(--bg2);
  border-top: 1px solid var(--border2);
  padding: 10px 14px;
  display: flex;
  align-items: center;
  gap: 10px;
  flex-shrink: 0;
}
.msg-inp {
  flex: 1;
  background: var(--bg);
  border: 1px solid var(--border2);
  border-radius: 3px;
  padding: 10px 14px;
  color: var(--text);
  font-family: var(--font-mono);
  font-size: 14px;
  outline: none;
  transition: border-color 0.2s;
}
.msg-inp:focus { border-color: var(--cyan); }
.msg-inp::placeholder { color: var(--text-dim); }
.send-btn {
  background: var(--cyan);
  border: none;
  border-radius: 3px;
  width: 40px;
  height: 40px;
  cursor: pointer;
  color: var(--bg);
  font-size: 17px;
  display: flex;
  align-items: center;
  justify-content: center;
  flex-shrink: 0;
  transition: opacity 0.2s, transform 0.1s;
}
.send-btn:active { transform: scale(0.9); }

/* No-users placeholder */
.no-users {
  padding: 14px 12px;
  font-family: var(--font-mono);
  font-size: 11px;
  color: var(--text-dim);
  text-align: center;
}

@media (max-width: 480px) {
  .sidebar { width: 140px; }
  .hdr-title { font-size: 11px; }
  .msg-text { font-size: 14px; }
}
</style>
</head>
<body>

<!-- ════════════ LOGIN PAGE ════════════ -->
<div id="login-screen">
  <div class="login-card">
    <div class="login-topbar">
      <div class="dot-r"></div>
      <div class="dot-y"></div>
      <div class="dot-g"></div>
      <div class="topbar-title">esp32-chatroom — secure</div>
    </div>
    <div class="login-body">
      <div class="login-logo">
        <svg viewBox="0 0 48 48" fill="none">
          <rect x="4" y="14" width="40" height="28" rx="3" stroke="#00d4ff" stroke-width="1.5"/>
          <rect x="10" y="8" width="28" height="6" rx="2" fill="#0d1219" stroke="#1a2535" stroke-width="1"/>
          <circle cx="24" cy="28" r="5" stroke="#00e676" stroke-width="1.5"/>
          <path d="M24 23v-4M24 33v4M19 28h-4M29 28h4" stroke="#00e676" stroke-width="1" stroke-linecap="round"/>
          <circle cx="24" cy="28" r="2" fill="#00e676"/>
        </svg>
      </div>
      <div class="login-title">CHATROOM</div>
      <div class="login-sub">[ PRIVATE ACCESS — ESP32 NODE ]</div>

      <label class="field-label">Room Password</label>
      <div class="field-wrap">
        <span>🔑</span>
        <input type="password" id="inp-pass" placeholder="Enter secret key..."
               autocomplete="off" onkeydown="if(e.key==='Enter'||event.key==='Enter')doLogin()">
      </div>

      <label class="field-label">Your Callsign</label>
      <div class="field-wrap">
        <span>$</span>
        <input type="text" id="inp-name" placeholder="username (2-12 chars)"
               maxlength="12" autocomplete="off" autocapitalize="none"
               onkeydown="if(e.key==='Enter'||event.key==='Enter')doLogin()">
      </div>

      <button class="btn-join" onclick="doLogin()">ACCESS ROOM</button>
      <div class="login-err" id="err-msg"></div>
    </div>
  </div>
</div>

<!-- ════════════ CHAT SCREEN ════════════ -->
<div id="chat-screen">

  <!-- Header -->
  <div class="hdr">
    <div class="hdr-sig"></div>
    <div class="hdr-title">📡 ESP32 // CHAT HUB</div>
    <div class="hdr-badge">YOU: <b id="my-badge">–</b></div>
  </div>

  <!-- Tabs -->
  <div class="tabs">
    <div class="tab active" id="tab-lobby" onclick="switchTab('lobby')">
      🌐 Lobby
      <span class="badge" id="notif-lobby">0</span>
    </div>
    <div class="tab" id="tab-pm" onclick="switchTab('pm')">
      🔒 Private
      <span class="badge" id="notif-pm">0</span>
    </div>
  </div>

  <!-- Body -->
  <div class="body">

    <!-- Sidebar -->
    <div class="sidebar">
      <div class="sidebar-hdr">⬤ ONLINE</div>
      <div class="users-list" id="users-list">
        <div class="no-users">No other users<br>online yet...</div>
      </div>
    </div>

    <!-- Chat Panel -->
    <div class="panel">

      <!-- Private target bar -->
      <div class="pm-bar" id="pm-bar">
        <span>PM ▸</span>
        <b id="pm-target-name">–</b>
        <span class="close-pm" onclick="closePM()" title="Close PM">✕</span>
      </div>

      <!-- Messages: Lobby -->
      <div class="msgs" id="msgs-lobby"></div>

      <!-- Messages: PM -->
      <div class="msgs" id="msgs-pm" style="display:none;"></div>

      <!-- Input -->
      <div class="input-bar">
        <input class="msg-inp" id="msg-inp"
               placeholder="Type a message and press Enter..."
               onkeydown="if(event.key==='Enter')sendMsg()">
        <button class="send-btn" onclick="sendMsg()" title="Send">➤</button>
      </div>
    </div>
  </div>
</div>

<script>
// ── State ─────────────────────────────────────────────────────
let ws, myName = '', curTab = 'lobby', pmTarget = '';
let unread = { lobby: 0, pm: 0 };
let pingInterval;

// ── WebSocket ─────────────────────────────────────────────────
function initWS() {
  ws = new WebSocket('ws://' + location.host + '/ws');
  ws.onopen    = ()    => { startPing(); };
  ws.onclose   = ()    => { clearInterval(pingInterval); setTimeout(initWS, 2000); addSys('msgs-lobby', '⚠ Reconnecting...'); };
  ws.onerror   = (e)   => console.error('WS error', e);
  ws.onmessage = (evt) => handleMsg(JSON.parse(evt.data));
}

function startPing() {
  clearInterval(pingInterval);
  pingInterval = setInterval(() => ws.readyState === 1 && ws.send('{"type":"ping"}'), 20000);
}

// ── Message Handler ───────────────────────────────────────────
function handleMsg(d) {
  switch(d.type) {
    case 'ask_login': break;

    case 'login_ok':
      myName = d.username;
      document.getElementById('my-badge').textContent = myName;
      document.getElementById('login-screen').style.display = 'none';
      document.getElementById('chat-screen').style.display = 'flex';
      addSys('msgs-lobby', '✅ You are now in the room. Say hi!');
      document.getElementById('msg-inp').focus();
      break;

    case 'login_fail':
      showErr(d.msg);
      break;

    case 'users_list':
      renderUsers(d.users);
      break;

    case 'user_joined':
      addSys('msgs-lobby', `▶ ${d.username} joined the room`);
      break;

    case 'user_left':
      addSys('msgs-lobby', `◀ ${d.username} left the room`);
      if (pmTarget === d.username) addSys('msgs-pm', `⚠ ${d.username} disconnected`);
      break;

    case 'partner_left':
      addSys('msgs-pm', `⚠ ${d.username} disconnected`);
      break;

    case 'lobby_msg':
      addMsg('msgs-lobby', d.from, d.text, d.from === myName, false);
      if (curTab !== 'lobby') bump('lobby');
      break;

    case 'private_msg': {
      const other = d.from === myName ? d.to : d.from;
      if (pmTarget !== other) openPMWith(other, false);
      addMsg('msgs-pm', d.from, d.text, d.from === myName, true);
      if (curTab !== 'pm') bump('pm');
      break;
    }

    case 'error':
      showErr(d.msg);
      break;
  }
}

// ── Login ─────────────────────────────────────────────────────
function doLogin() {
  const pass = document.getElementById('inp-pass').value.trim();
  const name = document.getElementById('inp-name').value.trim();
  if (!pass || !name) { showErr('Fill all fields!'); return; }
  if (ws.readyState !== 1) { showErr('Not connected, wait...'); return; }
  ws.send(JSON.stringify({ type: 'login', password: pass, username: name }));
}

function showErr(msg) {
  const el = document.getElementById('err-msg');
  el.textContent = '⚠ ' + msg;
  setTimeout(() => el.textContent = '', 3000);
}

// ── Send ──────────────────────────────────────────────────────
function sendMsg() {
  const inp = document.getElementById('msg-inp');
  const txt = inp.value.trim();
  if (!txt || ws.readyState !== 1) return;
  inp.value = '';

  if (curTab === 'lobby') {
    ws.send(JSON.stringify({ type: 'lobby_msg', text: txt }));
  } else if (curTab === 'pm' && pmTarget) {
    ws.send(JSON.stringify({ type: 'private_msg', to: pmTarget, text: txt }));
  }
}

// ── Tabs ──────────────────────────────────────────────────────
function switchTab(tab) {
  curTab = tab;
  document.getElementById('tab-lobby').classList.toggle('active', tab === 'lobby');
  document.getElementById('tab-pm').classList.toggle('active', tab === 'pm');
  document.getElementById('msgs-lobby').style.display = tab === 'lobby' ? 'flex' : 'none';
  document.getElementById('msgs-pm').style.display    = tab === 'pm'    ? 'flex' : 'none';
  document.getElementById('pm-bar').classList.toggle('show', tab === 'pm' && !!pmTarget);

  if (tab === 'lobby') {
    unread.lobby = 0;
    document.getElementById('msg-inp').placeholder = 'Message everyone in Lobby...';
  } else {
    unread.pm = 0;
    document.getElementById('msg-inp').placeholder = pmTarget
      ? `Message ${pmTarget} privately...`
      : 'Click a user to start PM...';
  }
  updateBadges();
  const el = document.getElementById('msgs-' + tab);
  el.scrollTop = el.scrollHeight;
  document.getElementById('msg-inp').focus();
}

// ── PM ────────────────────────────────────────────────────────
function openPMWith(username, switchToTab = true) {
  pmTarget = username;
  document.getElementById('pm-target-name').textContent = username;
  document.getElementById('pm-bar').classList.add('show');
  const tEl = document.getElementById('tab-pm');
  tEl.innerHTML = `🔒 ${username} <span class="badge" id="notif-pm"></span>`;
  if (switchToTab) {
    document.getElementById('msgs-pm').innerHTML = '';
    switchTab('pm');
  }
}

function closePM() {
  pmTarget = '';
  document.getElementById('pm-bar').classList.remove('show');
  document.getElementById('msgs-pm').innerHTML = '';
  document.getElementById('tab-pm').innerHTML = '🔒 Private <span class="badge" id="notif-pm"></span>';
  switchTab('lobby');
}

// ── Users List ────────────────────────────────────────────────
function renderUsers(list) {
  const el = document.getElementById('users-list');
  if (!list.length) {
    el.innerHTML = '<div class="no-users">No others<br>online yet...</div>';
    return;
  }
  el.innerHTML = '';
  list.forEach(u => {
    const d = document.createElement('div');
    d.className = 'user-row';
    d.innerHTML = `<div class="user-dot"></div>
                   <div class="user-name">${esc(u)}</div>
                   <div class="user-pm" title="Private message">💬</div>`;
    d.onclick = () => openPMWith(u);
    el.appendChild(d);
  });
}

// ── UI Helpers ────────────────────────────────────────────────
function addMsg(cid, from, text, isMe, isPM) {
  const el = document.getElementById(cid);
  const now = new Date();
  const t = now.getHours().toString().padStart(2,'0') + ':' + now.getMinutes().toString().padStart(2,'0');
  const div = document.createElement('div');
  div.className = 'msg-item';
  div.innerHTML = `<div class="msg-meta">
    ${isPM ? '<span class="msg-pm-badge">PM</span>' : ''}
    <span class="msg-author ${isMe ? 'is-me' : ''}">${esc(from)}</span>
    <span class="msg-time">${t}</span>
  </div>
  <div class="msg-text">${esc(text)}</div>`;
  el.appendChild(div);
  el.scrollTop = el.scrollHeight;
}

function addSys(cid, text) {
  const el = document.getElementById(cid);
  const div = document.createElement('div');
  div.className = 'msg-sys';
  div.textContent = '─── ' + text + ' ───';
  el.appendChild(div);
  el.scrollTop = el.scrollHeight;
}

function bump(tab) {
  unread[tab]++;
  updateBadges();
}

function updateBadges() {
  ['lobby','pm'].forEach(t => {
    const el = document.getElementById('notif-' + t);
    if (!el) return;
    el.textContent = unread[t];
    el.classList.toggle('show', unread[t] > 0);
  });
}

function esc(s) {
  return String(s)
    .replace(/&/g,'&amp;').replace(/</g,'&lt;').replace(/>/g,'&gt;')
    .replace(/"/g,'&quot;').replace(/'/g,'&#039;');
}

// ── Boot ──────────────────────────────────────────────────────
initWS();
</script>
</body>
</html>
)rawhtml";

// ════════════════════════════════════════════════════════════════
//  🚀 SETUP
// ════════════════════════════════════════════════════════════════

void setup() {
    Serial.begin(115200);
    Serial.println("\n\n╔══════════════════════════════╗");
    Serial.println("║  ESP32 Chat Room — Phase 1   ║");
    Serial.println("╚══════════════════════════════╝");

    // ── TFT Init ─────────────────────────────────────────────────
    pinMode(TFT_BL, OUTPUT);
    analogWrite(TFT_BL, 200); // ~78% brightness

    tft.initR(INITR_BLACKTAB);  // For 1.8" ST7735S with black tab
    tft.setRotation(1);         // Landscape: 160x128
    tft.fillScreen(C_BG);
    tftSplash();
    delay(500);

    // ── WiFi Access Point ────────────────────────────────────────
    Serial.printf("[WiFi] Starting AP: '%s'\n", WIFI_SSID);
    tft.setTextColor(C_YELLOW);
    tft.setCursor(4, 36);
    tft.print("Starting WiFi AP...");

    WiFi.mode(WIFI_AP);
    WiFi.softAP(WIFI_SSID, WIFI_PASS, 1, 0, 8); // ch1, not hidden, max 8 clients

    IPAddress ip = WiFi.softAPIP();
    Serial.printf("[WiFi] AP IP: %s\n", ip.toString().c_str());
    Serial.printf("[WiFi] SSID: %s / PASS: %s\n", WIFI_SSID, WIFI_PASS);
    Serial.printf("[Room] Key: %s\n", ROOM_KEY);

    tftShowIP(ip);

    // ── WebSocket ────────────────────────────────────────────────
    ws.onEvent(onWsEvent);
    server.addHandler(&ws);

    // ── Serve HTML ───────────────────────────────────────────────
    server.on("/", HTTP_GET, [](AsyncWebServerRequest* req) {
        req->send_P(200, "text/html; charset=utf-8", HTML_PAGE);
    });

    // ── 404 ──────────────────────────────────────────────────────
    server.onNotFound([](AsyncWebServerRequest* req) {
        req->redirect("/");
    });

    server.begin();
    Serial.println("[Server] Started! Open http://192.168.4.1 in browser\n");

    delay(500);
    tftUpdateDashboard();
}

// ════════════════════════════════════════════════════════════════
//  🔁 LOOP
// ════════════════════════════════════════════════════════════════

void loop() {
    ws.cleanupClients();

    // Periodic TFT refresh
    if (millis() - lastTFTUpdate > TFT_REFRESH_MS) {
        lastTFTUpdate = millis();
        // Only refresh if clients connected to reduce flicker
        if (users.size() > 0) {
            tftUpdateDashboard();
        }
    }

    delay(10);
}
