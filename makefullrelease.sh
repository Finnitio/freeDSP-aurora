#!/bin/bash

if [ "$#" -ne 1 ]; then
  echo "usage: makefullrelease.sh versionnumber"
  exit 1
fi

if [ -d $1 ]; then
  rm -rf $1
fi

echo "Deploying firmware"
mkdir $1
mkdir $1/firmware
cp -rf ./SOURCES/WEBAPP/ESP32/aurora/aurora.ino.esp32.bin $1/firmware/aurora.ino.esp32.bin

echo "Deploying updater"
mkdir $1/updater
cp -rf ./SOURCES/updater/* $1/updater/
cp -rf ./SOURCES/WEBAPP/ESP32/aurora/aurora.ino.esp32.bin $1/updater/aurora.ino.esp32.bin

echo "Deploying documentation"
cp -rf ./DOCUMENTATION/UserManual_*.pdf $1

echo "Compressing JavaScript"
cd SOURCES/WEBAPP/js
cat aurora.js | gzip > aurora.jgz
cd ../../..

echo "Deploying plugins"
PLUGIN_VERSION=v2.2.2
cd SOURCES/sigma2aurora
# convert <project> [gui]: stop the release if the converter fails
convert() {
  local gui=()
  if [ -n "$2" ]; then
    gui=(--gui "$2")
  fi
  if ! python sigma2aurora.py ../SIGMASTUDIO/$1/$1.xml $1 --version $PLUGIN_VERSION "${gui[@]}" --outputdir plugins; then
    echo "Converting plugin $1 failed" >&2
    exit 1
  fi
}
convert 8channels ../WEBAPP/plugins/8channels/dsp.html
convert HomeCinema71 ../WEBAPP/plugins/homecinema71/dsp.html
convert 4FIRs ../WEBAPP/plugins/4FIRs/dsp.html
# The matrix page is built into the firmware, so matrix has no --gui
convert matrix

cd ../..

echo "Cleaning up"
mv -f ./SOURCES/sigma2aurora/plugins $1

echo "Finished"