#!/bin/sh
# Live log over WiFi from the bridge web server.
# Usage: ./logs.sh <bridge-ip-or-hostname>
cd "$(dirname "$0")"
HOST=${1:?usage: ./logs.sh <bridge-ip-or-hostname>}
PW=$(grep '^ota_password:' secrets.yaml | cut -d'"' -f2)
curl -sN --anyauth -u "admin:$PW" "http://$HOST/events" | awk '/^event: log/{l=1;next} l&&/^data:/{sub(/^data: /,"");print;fflush();l=0}'
