#include <Arduino.h>
#include <BLEDevice.h>
#include <BLEUtils.h>
#include <BLEServer.h>
#include <BLE2902.h>

BLEServer* server;

BLECharacteristic* txdCharacteristic;
BLECharacteristic* rxdCharacteristic;
BLECharacteristic* atsendCharacteristic;
BLECharacteristic* atrespCharacteristic;

#define SERVICE_MAIN_UUID "F1F0"
#define CHARACTERISTIC_TXD_UUID "F1F1"
#define CHARACTERISTIC_RXD_UUID "F1F2"

#define SERVICE_AT_UUID "F2F0"
#define CHARACTERISTIC_ATSEND_UUID "F2F1"
#define CHARACTERISTIC_ATRESP_UUID "F2F2"

char* hexToString(const unsigned char* array, size_t length) {
    char* hexString = (char*)malloc(length * 2 + 1);

    for (size_t i = 0; i < length; ++i) {
        sprintf(hexString + i * 2, "%02X", array[i]);
    }

    hexString[length * 2] = '\0';
    return hexString;
}

class ServerCallbacks : public BLEServerCallbacks {
    void onConnect(BLEServer* server) {
      Serial.println("Connected to client");
    }

    void onDisconnect(BLEServer* server) {
      Serial.println("Disconnected from client");
      BLEDevice::startAdvertising();
    }
};

class TxdCharacteristicCallbacks : public BLECharacteristicCallbacks {
    void onWrite(BLECharacteristic* characteristic) {
      std::string value = characteristic->getValue();

      if (value.length() > 0) {
        char* hexString = hexToString((unsigned char*)value.c_str(), value.length());
        Serial.println("Received data on TXD: " + String(hexString));
        free(hexString);

        int dType = value[3]; // "dType" as-is in the original code
        
        // FEFE 09B0 0101 0000: start stage 1
        if (dType == 0xB0) {
          Serial.println("Received B0 on TXD, sending FDFD 09B0 on RXD");
          std::vector<uint8_t> response = {0xFD, 0xFD, 0x09, 0xB0, 0x01, 0x42, 0x02, 0x00, 0x07, 0xE2, 0xEB, 0x20, 0x01, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00};
          rxdCharacteristic->setValue(response.data(), response.size());
          rxdCharacteristic->notify();
        }

        // FEFE 09B2...: start stage 2
        // 
        // Example B2 payload 1: FE FE 09 B2 01 01 0D 00 01 15 92 09 11 09 13 40 26 00 00 00
        // Example B2 payload 2: FE FE 09 B2 01 xx xx 00 70 E2 EB 20 01 01 00 00 00 6C 30 00
        // Example B2 payload 3: FE FE 09 B2 01 xx xx 00 00 73 37 20 01 01 00 00 00 E0 01 00
        // Example B2 payload 4: FE FE 09 B2 01 01 0D 00 00 41 63 19 08 31 22 10 03 0? 52 64 (we lost half a byte here)
        //
        // known parts:
        // xx xx: crc-16, refer to waterctl for the implementation
        // 20 01 01 00 00 00: yy-mm-dd hh:mm:ss
        if (dType == 0xB2) {
          Serial.println("Received B2 on TXD, sending FDFD 09B2 on RXD");
          std::vector<uint8_t> responseB2 = {0xFD, 0xFD, 0x09, 0xB2, 0x01, 0x42, 0x09, 0x01, 0x00, 0x00, 0x6D, 0x6C, 0x00, 0x02, 0x79, 0x32};
          rxdCharacteristic->setValue(responseB2.data(), responseB2.size());
          rxdCharacteristic->notify();

          std::vector<uint8_t> responseBa = {0xFD, 0xFD, 0x09, 0xBA, 0x07, 0x42, 0x0D, 0x6D, 0x6C, 0x00, 0x02, 0x79, 0x32, 0x00, 0x14, 0xB8, 0x10, 0x00, 0x00, 0x00};
          rxdCharacteristic->setValue(responseBa.data(), responseBa.size());
          rxdCharacteristic->notify();
        }

        // FEFE 09B3 0000: end/disconnect stage 1
        if (dType == 0xB3) {
          Serial.println("Received B3 on TXD, sending response FDFD 09B3 on RXD");
          std::vector<uint8_t> response = {0xFD, 0xFD, 0x09, 0xB3, 0x38, 0xBB, 0x02, 0x00, 0x70, 0xE2, 0xEB, 0x20, 0x01, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00};
          // known parts (RXD response):
          // last 3 bytes: cost in little-endian; to calculate the cost, convert to decimal, divide by 10, multiply by hotWaterPrice (25 by default), divide by 1000 and round to 2 decimal places
          // Example: B2 00 00 -> 0x0000B2 -> 0xB2 / 10 * 25 / 1000 = 0.45 RMB
          rxdCharacteristic->setValue(response.data(), response.size());
          rxdCharacteristic->notify();
        } 

        // FEFE 09B4 0000: end/disconnect stage 2
        if (dType == 0xB4) {
          Serial.println("Received B4 on TXD. It should disconnect now");
          // uint16_t connId = characteristic->getHandle();
          // if (connId != 0) {
          //   server->disconnect(connId);
          // }
        }
      }
    }
};

class AtsendCharacteristicCallbacks : public BLECharacteristicCallbacks {
    void onWrite(BLECharacteristic* characteristic) {
      std::string value = characteristic->getValue();
      if (value.length() > 0) {
        Serial.println("Received ATSend data: " + String(value.c_str()));
      }
    }
};

void setup() {
  Serial.begin(115200);
  Serial.println("wateremu by celesWuff");

  BLEDevice::init("Water12345");
  server = BLEDevice::createServer();
  server->setCallbacks(new ServerCallbacks());

  // Service 0xF1F0, TXD 0xF1F1, RXD 0xF1F2
  BLEService* mainService = server->createService(SERVICE_MAIN_UUID);
  txdCharacteristic = mainService->createCharacteristic(
                                          CHARACTERISTIC_TXD_UUID,
                                          BLECharacteristic::PROPERTY_WRITE | BLECharacteristic::PROPERTY_WRITE_NR
                                        );
  txdCharacteristic->setCallbacks(new TxdCharacteristicCallbacks());

  rxdCharacteristic = mainService->createCharacteristic(
                                          CHARACTERISTIC_RXD_UUID,
                                          BLECharacteristic::PROPERTY_NOTIFY
                                        );
  rxdCharacteristic->addDescriptor(new BLE2902());

  mainService->start();

  // Service 0xF2F0, ATSend 0xF2F1, ATResp 0xF2F2
  BLEService* atService = server->createService(SERVICE_AT_UUID);
  atsendCharacteristic = atService->createCharacteristic(
                                          CHARACTERISTIC_ATSEND_UUID,
                                          BLECharacteristic::PROPERTY_WRITE | BLECharacteristic::PROPERTY_WRITE_NR
                                        );
  atsendCharacteristic->setCallbacks(new AtsendCharacteristicCallbacks());

  atrespCharacteristic = atService->createCharacteristic(
                                          CHARACTERISTIC_ATRESP_UUID,
                                          BLECharacteristic::PROPERTY_NOTIFY
                                        );
  atrespCharacteristic->addDescriptor(new BLE2902());

  atService->start();

  BLEAdvertising* advertising = BLEDevice::getAdvertising();
  advertising->addServiceUUID(SERVICE_MAIN_UUID);
  advertising->addServiceUUID(SERVICE_AT_UUID);
  advertising->setScanResponse(true);
  BLEDevice::startAdvertising();
  Serial.println("BLE server started");
}

void loop() {
  // けものになりたい！
}