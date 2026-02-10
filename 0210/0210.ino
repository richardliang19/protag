#include <BLEDevice.h>
#include <BLEServer.h>
#include <BLEUtils.h>
#include <BLE2902.h>
#include "esp_bt_main.h"
#include "esp_bt_device.h"
#include "esp_gap_ble_api.h"
//issues


// --- 硬體腳位設定 (ESP32-C3 SuperMini) ---
#define REED_PIN        5     // 磁簧開關 (接 GPIO 5 和 GND)
#define BUZZER_PIN      4     // 蜂鳴器 (正極接 GPIO 4，負極接 GND)

// --- UUID 設定 (必須與 App 相同) ---
#define SERVICE_UUID           "6E400001-B5A3-F393-E0A9-E50E24DCCA9E" 
#define CHARACTERISTIC_UUID_TX "6E400003-B5A3-F393-E0A9-E50E24DCCA9E"
#define CHARACTERISTIC_UUID_RX "6E400002-B5A3-F393-E0A9-E50E24DCCA9E"

// --- 省電設定 ---
#define IDLE_LOOP_DELAY   200   // 鎖上時的輪詢間隔 (ms)
#define ACTIVE_LOOP_DELAY 50    // 分開時的輪詢間隔 (ms)

BLEServer* pServer = NULL;
BLECharacteristic* pTxCharacteristic;
bool deviceConnected = false;
bool oldDeviceConnected = false;
bool isOpen = false;

// ★ 新增：連線後延遲發送用 ★
bool needSendInitialState = false;
unsigned long connectedTime = 0;
#define INITIAL_STATE_DELAY 1000  // 連線後等 1 秒再發送初始狀態

// --- 藍牙連線與斷線回調 ---
class MyServerCallbacks: public BLEServerCallbacks {
    void onConnect(BLEServer* pServer) {
      deviceConnected = true;
    };
    
    void onDisconnect(BLEServer* pServer) {
      deviceConnected = false;
    }
};

// --- 接收手機指令回調 ---
class MyCallbacks: public BLECharacteristicCallbacks {
    void onWrite(BLECharacteristic *pCharacteristic) {
      std::string rxValue = pCharacteristic->getValue();
      if (rxValue.length() > 0) {
        
        Serial.print("收到資料: ");
        Serial.println(rxValue.c_str());
        
        if (rxValue.find("ALARM_ON") != std::string::npos) {
           Serial.println(">>> 啟動警報！");
           digitalWrite(BUZZER_PIN, HIGH);
        }
        else if (rxValue.find("RESET") != std::string::npos) {
           Serial.println(">>> 解除警報！");
           digitalWrite(BUZZER_PIN, LOW);
        }
      }
    }
};

void setupPowerSaving() {
  setCpuFrequencyMhz(80);
  Serial.print("CPU 頻率: ");
  Serial.print(getCpuFrequencyMhz());
  Serial.println(" MHz");
  
  esp_ble_tx_power_set(ESP_BLE_PWR_TYPE_DEFAULT, ESP_PWR_LVL_P3);
  esp_ble_tx_power_set(ESP_BLE_PWR_TYPE_ADV, ESP_PWR_LVL_P3);
  esp_ble_tx_power_set(ESP_BLE_PWR_TYPE_CONN_HDL0, ESP_PWR_LVL_P3);
}

void setupAdvertising() {
  BLEAdvertising *pAdvertising = BLEDevice::getAdvertising();
  pAdvertising->addServiceUUID(SERVICE_UUID);
  pAdvertising->setScanResponse(true);
  pAdvertising->setMinPreferred(0x190);
  pAdvertising->setMaxPreferred(0x320);
  BLEDevice::startAdvertising();
}

void setup() {
  Serial.begin(115200);
  delay(1000);
  
  Serial.println("=============================");
  Serial.println("  ESP32 Smart Guard v4");
  Serial.println("  修正連線時發送時機");
  Serial.println("=============================");
  
  pinMode(REED_PIN, INPUT_PULLUP);
  pinMode(BUZZER_PIN, OUTPUT);
  
  digitalWrite(BUZZER_PIN, LOW);
  
  setupPowerSaving();

  BLEDevice::init("ESP32_Smart_Guard"); 
  BLEDevice::setPower(ESP_PWR_LVL_P3);
  
  pServer = BLEDevice::createServer();
  pServer->setCallbacks(new MyServerCallbacks());
  
  BLEService *pService = pServer->createService(SERVICE_UUID);

  pTxCharacteristic = pService->createCharacteristic(
                    CHARACTERISTIC_UUID_TX,
                    BLECharacteristic::PROPERTY_NOTIFY
                  );
  pTxCharacteristic->addDescriptor(new BLE2902());

  BLECharacteristic *pRxCharacteristic = pService->createCharacteristic(
                        CHARACTERISTIC_UUID_RX,
                        BLECharacteristic::PROPERTY_WRITE | BLECharacteristic::PROPERTY_WRITE_NR
                      );
  pRxCharacteristic->setCallbacks(new MyCallbacks());

  pService->start();
  setupAdvertising();
  
  // 讀取初始狀態
  isOpen = (digitalRead(REED_PIN) == HIGH);
  Serial.print("初始狀態：");
  Serial.println(isOpen ? "分開" : "鎖上");
  Serial.println("");
  Serial.println(">>> 等待連線中...");
}

void loop() {
  int loopDelay = isOpen ? ACTIVE_LOOP_DELAY : IDLE_LOOP_DELAY;
  unsigned long currentMillis = millis();
  
  // ---------------------------------------------------------
  // 剛連線 - 標記需要發送初始狀態
  // ---------------------------------------------------------
  if (deviceConnected && !oldDeviceConnected) {
      oldDeviceConnected = deviceConnected;
      Serial.println("");
      Serial.println(">>> 手機已連線！");
      
      // ★ 不立即發送，而是標記等待 ★
      needSendInitialState = true;
      connectedTime = currentMillis;
      Serial.println(">>> 等待 1 秒後發送初始狀態...");
  }
  
  // ---------------------------------------------------------
  // ★ 延遲發送初始狀態 ★
  // ---------------------------------------------------------
  if (needSendInitialState && deviceConnected) {
    if (currentMillis - connectedTime >= INITIAL_STATE_DELAY) {
      needSendInitialState = false;
      
      // 讀取目前狀態並發送
      isOpen = (digitalRead(REED_PIN) == HIGH);
      
      if (isOpen) {
        pTxCharacteristic->setValue("OPEN");
        Serial.println(">> 發送初始狀態: OPEN");
      } else {
        pTxCharacteristic->setValue("CLOSE");
        Serial.println(">> 發送初始狀態: CLOSE");
      }
      pTxCharacteristic->notify();
    }
  }
  
  // ---------------------------------------------------------
  // 已連線狀態 - 偵測狀態變化
  // ---------------------------------------------------------
  if (deviceConnected && !needSendInitialState) {
    int sensorState = digitalRead(REED_PIN);
    bool currentOpen = (sensorState == HIGH);

    // 只有狀態改變時才發送
    if (currentOpen != isOpen) {
      isOpen = currentOpen;
      
      if (isOpen) {
        Serial.println("狀態改變：分開");
        pTxCharacteristic->setValue("OPEN");
        pTxCharacteristic->notify();
        Serial.println(">> 發送: OPEN");
      } 
      else {
        Serial.println("狀態改變：鎖上");
        digitalWrite(BUZZER_PIN, LOW);
        pTxCharacteristic->setValue("CLOSE");
        pTxCharacteristic->notify();
        Serial.println(">> 發送: CLOSE");
      }
    }
  }

  // ---------------------------------------------------------
  // 剛斷線
  // ---------------------------------------------------------
  if (!deviceConnected && oldDeviceConnected) {
      Serial.println("");
      Serial.println(">>> 藍牙已斷線！");
      delay(500);
      setupAdvertising();
      Serial.println(">>> 重新廣播中...");
      digitalWrite(BUZZER_PIN, LOW);
      needSendInitialState = false;
      oldDeviceConnected = deviceConnected;
  }
  
  delay(loopDelay);
}
