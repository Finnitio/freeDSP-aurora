# Software build instructions

If you just want to use the Aurora in your project, please use the pre-built releases instead of building your own firmware (this is **not** necessary for creating your own plugins).
But if you want to assist with development by contributing code, you will find the Aurora actually runs three firmwares:

1. **ESP32**: Runs the primary program which controls everything else, and offers the WiFi interface. Probably the most interesting of the bunch. See [this document](SOURCES/WEBAPP/ESP32/README.md) for more details and how to build.
2. **ADAU1452**: The DSP firmware is build using SigmaStudio. The firmware is stored on the ESP32 as part of the installed plugin.
3. **XMOS**: The USB interface comes with a 7.1 firmware pre-installed; if you want to change this, you need to build your own firmware.

Please note that a complete Aurora Plugin consists of a DSP firmware image, an associated control website and some meta-data.
The actual XMOS and ESP32 firmware are not modified by plugins.

## Release build

`./makefullrelease.sh <version>` collects a release into the folder `<version>`: the ESP32 firmware (build `SOURCES/WEBAPP/ESP32/aurora/aurora.ino.esp32.bin` first), the updater, the user manuals and the plugins.
It converts each SigmaStudio project in `SOURCES/SIGMASTUDIO` with `sigma2aurora` into `<version>/plugins`: 8channels, HomeCinema71, 4FIRs and matrix, all with the same plugin version.
The matrix plugin has no generated `dsp.html`, because its page is part of the ESP32 firmware.
The script stops with an error if any plugin conversion fails.

## Host tests

`SOURCES/WEBAPP/ESP32/test/run.sh` runs all host tests from any folder: every `test_*.cpp` and `test_*.js` in `SOURCES/WEBAPP/ESP32/test`, and the pytest suites there and in `SOURCES/sigma2aurora/tests`.
It exits non-zero and lists the failing tests if any fails.
It needs `bash`, `g++`, `node` and Python with `pytest`; on Windows run it in Git Bash.
New test files are picked up automatically. A C++ test names the firmware sources it links (`// Links: matrixmodel.cpp`) and the libraries it uses (`// Uses: ArduinoJson`) in its header.
ArduinoJson is taken from `ARDUINOJSON_DIR`, or else from `~/Arduino/libraries/ArduinoJson` or `~/Documents/Arduino/libraries/ArduinoJson`.
