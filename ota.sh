#!/bin/sh
# Compile and upload over WiFi through the bridge web server.
# Usage: ./ota.sh <bridge-ip-or-hostname>
set -e
cd "$(dirname "$0")"
HOST=${1:?usage: ./ota.sh <bridge-ip-or-hostname>}
PW=$(grep '^ota_password:' secrets.yaml | cut -d'"' -f2)
.venv/bin/esphome compile tarken.yaml
curl -sS -m 300 --anyauth -u "admin:$PW" -F "update=@.esphome/build/tarken-bridge/build/firmware.ota.bin" "http://$HOST/update"
echo
