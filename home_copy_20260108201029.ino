#include <Arduino.h>
#include <BLEDevice.h>
#include <BLEServer.h>
#include <BLEUtils.h>
#include <BLE2902.h>
#include <RCSwitch.h>
#include <EEPROM.h>

/* ================= RF ================= */
#define RF_TX_PIN 26

/* ================= EEPROM ================= */
#define EEPROM_SIZE 16

unsigned long rfCode = 0;
unsigned int  rfBits = 0;
bool rfStored = false;

/* ================= BLE CONFIG ================= */
static const char* BLE_NAME = "ESP32-GATE";
static const char* PIN_CODE = "1234";   // CHANGE THIS PIN

// UUIDs
#define SERVICE_UUID        "7b6a2e3c-3f2e-4c6e-9b4b-3e77a4f5f101"
#define CMD_CHAR_UUID       "7b6a2e3c-3f2e-4c6e-9b4b-3e77a4f5f102"
#define STATUS_CHAR_UUID    "7b6a2e3c-3f2e-4c6e-9b4b-3e77a4f5f103"

RCSwitch rf;
BLECharacteristic* statusChar = nullptr;

unsigned long lastUnlockMs = 0;
const unsigned long UNLOCK_COOLDOWN_MS = 1500;

static inline String trimBoth(String s) { s.trim(); return s; }

void sendGate() {
  rf.send(rfCode, rfBits);
  delay(5);
  rf.send(rfCode, rfBits); // repeat helps many gates
}

void setStatus(const String& s) {
  Serial.println(s);
  if (statusChar) {
    statusChar->setValue(s);  // Arduino String
    statusChar->notify();
  }
}

// Parse "<code>,<bits>"
bool parseCodeBits(const String& in, unsigned long &c, unsigned int &b) {
  int comma = in.indexOf(',');
  if (comma < 0) return false;

  String codeStr = trimBoth(in.substring(0, comma));
  String bitsStr = trimBoth(in.substring(comma + 1));

  unsigned long cc = (unsigned long)codeStr.toInt();
  int bb = bitsStr.toInt();

  if (cc == 0 || bb <= 0) return false;
  c = cc;
  b = (unsigned int)bb;
  return true;
}

class CmdCallbacks : public BLECharacteristicCallbacks {
  void onWrite(BLECharacteristic* pCharacteristic) override {
    // ESP32 core 3.x: use Arduino String API
    String cmd = pCharacteristic->getValue(); // <-- returns String in your build
    cmd = trimBoth(cmd);
    cmd.replace("\r", "");
    cmd.replace("\n", "");

    if (cmd.length() == 0) return;

    // Commands:
    // STATUS
    // OPEN:<PIN>
    // SEND:<PIN>
    // CLEAR:<PIN>
    // SET:<PIN>:<code>,<bits>       (store only)
    // CUSTOM:<PIN>:<code>,<bits>    (send + store)

    if (cmd.equalsIgnoreCase("STATUS")) {
      if (rfStored) setStatus("OK Stored code=" + String(rfCode) + " bits=" + String(rfBits));
      else          setStatus("OK No RF stored");
      return;
    }

    int c1 = cmd.indexOf(':');
    if (c1 < 0) { setStatus("ERR Bad format"); return; }

    String action = cmd.substring(0, c1);
    action.toUpperCase();

    // OPEN:<PIN>
    if (action == "OPEN") {
      String pin = trimBoth(cmd.substring(c1 + 1));
      if (pin != PIN_CODE) { setStatus("ERR Bad PIN"); return; }
      if (!rfStored) { setStatus("ERR No RF stored"); return; }

      unsigned long now = millis();
      if (now - lastUnlockMs < UNLOCK_COOLDOWN_MS) { setStatus("ERR Cooldown"); return; }
      lastUnlockMs = now;

      sendGate();
      setStatus("OK Unlocked");
      return;
    }

    // SEND:<PIN>
    if (action == "SEND") {
      String pin = trimBoth(cmd.substring(c1 + 1));
      if (pin != PIN_CODE) { setStatus("ERR Bad PIN"); return; }
      if (!rfStored) { setStatus("ERR No RF stored"); return; }

      sendGate();
      setStatus("OK Sent stored");
      return;
    }

    // CLEAR:<PIN>
    if (action == "CLEAR") {
      String pin = trimBoth(cmd.substring(c1 + 1));
      if (pin != PIN_CODE) { setStatus("ERR Bad PIN"); return; }

      rfCode = 0; rfBits = 0; rfStored = false;
      EEPROM.put(0, rfCode);
      EEPROM.put(4, rfBits);
      EEPROM.commit();
      setStatus("OK Cleared");
      return;
    }

    // SET / CUSTOM: SET:<PIN>:<code>,<bits>
    if (action == "SET" || action == "CUSTOM") {
      int c2 = cmd.indexOf(':', c1 + 1);
      if (c2 < 0) { setStatus("ERR Bad format"); return; }

      String pin = trimBoth(cmd.substring(c1 + 1, c2));
      if (pin != PIN_CODE) { setStatus("ERR Bad PIN"); return; }

      String payload = trimBoth(cmd.substring(c2 + 1));
      unsigned long c; unsigned int b;
      if (!parseCodeBits(payload, c, b)) { setStatus("ERR Bad code/bits"); return; }

      rfCode = c; rfBits = b; rfStored = true;
      EEPROM.put(0, rfCode);
      EEPROM.put(4, rfBits);
      EEPROM.commit();

      if (action == "CUSTOM") {
        sendGate();
        setStatus("OK Sent+Stored code=" + String(rfCode));
      } else {
        setStatus("OK Stored code=" + String(rfCode));
      }
      return;
    }

    setStatus("ERR Unknown cmd");
  }
};

void setup() {
  Serial.begin(115200);

  // RF TX
  rf.enableTransmit(RF_TX_PIN);
  rf.setPulseLength(350);

  // EEPROM load
  EEPROM.begin(EEPROM_SIZE);
  EEPROM.get(0, rfCode);
  EEPROM.get(4, rfBits);
  if (rfCode > 0 && rfBits > 0) rfStored = true;

  Serial.println("BLE Gate Ready");
  Serial.println(String("Stored? ") + (rfStored ? "YES" : "NO"));

  // BLE init
  BLEDevice::init(BLE_NAME);
  BLEServer* pServer = BLEDevice::createServer();

  BLEService* pService = pServer->createService(SERVICE_UUID);

  BLECharacteristic* cmdChar = pService->createCharacteristic(
    CMD_CHAR_UUID,
    BLECharacteristic::PROPERTY_WRITE
  );
  cmdChar->setCallbacks(new CmdCallbacks());

  statusChar = pService->createCharacteristic(
    STATUS_CHAR_UUID,
    BLECharacteristic::PROPERTY_READ | BLECharacteristic::PROPERTY_NOTIFY
  );
  statusChar->addDescriptor(new BLE2902());
  statusChar->setValue("READY");

  pService->start();

  BLEAdvertising* pAdv = BLEDevice::getAdvertising();
  pAdv->addServiceUUID(SERVICE_UUID);
  pAdv->setScanResponse(true);
  BLEDevice::startAdvertising();

  Serial.println("Advertising as: ESP32-GATE");
  Serial.println("Commands:");
  Serial.println("  STATUS");
  Serial.println("  OPEN:1234");
  Serial.println("  SEND:1234");
  Serial.println("  CUSTOM:1234:1234567,24");
  Serial.println("  SET:1234:1234567,24");
  Serial.println("  CLEAR:1234");
}

void loop() {
  // no blocking work here
}
