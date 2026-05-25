#!/bin/bash
# tests/emulator_cleanup.sh
# Gracefully shuts down the Android emulator.

echo "Killing Android emulator..."
adb -e emu kill || true
echo "Emulator shutdown command sent."
