# DIY Zigbee Sensors with ESP32-C6 / ESP32-H2

*** WORK IN PROGRESS ***  
************************

READ FULL GUIDE HERE:  
👉 [DIY Zigbee mmWave Presence Sensor with ESP32-C6 and LD2410](https://smarthomescene.com/guides/diy-zigbee-mmwave-presence-sensor-with-esp32-c6-and-ld2410/)

---

## SHS01 – Zigbee mmWave Presence Sensor [ESP32-C6 + HLK-LD2410C]

This project provides open firmware for building your own **Zigbee mmWave presence sensor** using an ESP32-C6 development board and the Hi-Link LD2410C radar module.  
It runs as a **Zigbee router**, so it helps strengthen your mesh while providing reliable human presence detection. 
Firmware works with the LD2410, LD2410B, LD2410C and can be adapted to the new LD2412 (Work in progress, SHS02).

### Current status
- ✅ Power configuration fixed  
- ✅ Zigbee router mode enabled  
- ✅ Occupancy, moving target, and static target states exposed  
- ✅ Configurable cooldowns, sensitivities, and range gates  
- ⚠️ Still under active development (expect changes)  

---

## Features
- **Dual human presence detection** (moving + static targets)  
- **Zigbee router role** (joins existing network, strengthens mesh)  
- **Custom configuration cluster** (0xFDCD) with attributes for:  
  - Movement cooldown (0–300s)  
  - Occupancy clear delay (0–65535s)  
  - Moving sensitivity (0–10 proxy → 0–100 internal)  
  - Static sensitivity (0–10 proxy → 0–100 internal)  
  - Moving max gate (0–8)  
  - Static max gate (2–8)  
- **Persistent storage** in NVS (settings survive reboot)  
- **BOOT button reset** (hold for 6s to factory reset Zigbee + restart)  

---

## Hardware
- **ESP32-C6-WROOM-1** development board  
- **Hi-Link LD2410C** mmWave radar sensor (UART1, 256000 baud)
- **DFRobot SEN0557** mmWave radar sensor (UART1, 57600 baud) - [24GHz Human Presence Sensing Module Wiki - DFRobot](https://wiki.dfrobot.com/SKU_SEN0557_24GHz_Human_Presence_Sensing_Module)
- USB-C or regulated 5V power supply  

---

## Firmware Notes
- Built on **ESP-IDF + esp-zigbee-sdk**  
- Based on Espressif’s `ha_dimmable_light` example  
- Minimal dependencies, lightweight and stable  
- Exposes attributes to Zigbee2MQTT (via external converter)  

---

## Setup
1. Install **ESP-IDF v5.1+**
   ```bash
   cd ~/esp-idf-v5.5/
   ./install.sh
   . ./export.sh
3. Clone this repo  
4. Build & flash:  
   ```bash
   idf.py set-target esp32c6
   idf.py build
   idf.py -p /dev/ttyUSB0 flash monitor
