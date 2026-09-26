# batmon-remote -- a touch display for the battery monitor, on the ESP32-2432S028
# ("Cheap Yellow Display"). A separate firmware from the monitor in the repository
# root: different chip (classic ESP32), different role (BLE central), its own
# version.txt. See README.md.

cmake_minimum_required(VERSION 3.16)

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(batmon-remote)
