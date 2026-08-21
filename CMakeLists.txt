# bat-monitor — BLE battery monitor for XIAO ESP32-C6 + INA219
# See DESIGN.md. This is milestone M1: bring-up (I2C + INA219 + console dump).

cmake_minimum_required(VERSION 3.16)

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(bat-monitor)
