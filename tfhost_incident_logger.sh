#!/usr/bin/env bash

LOGFILE="incident_$(TZ=Asia/Seoul date +%F_%H%M%S).log"

last_dns="unknown"
last_ssh="unknown"

echo "# TFHost incident monitor started $(TZ=Asia/Seoul date '+%F %T KST')" >> "$LOGFILE"

while true; do
    ts=$(TZ=Asia/Seoul date '+%F %T KST')

    dns=$(dig @1.1.1.1 +time=3 +tries=1 +short tfhost.ng | tr '\n' ' ')
    [ -z "$dns" ] && dns="SERVFAIL"

    if nc -z -w 3 160.119.197.71 22 >/dev/null 2>&1; then
        ssh_state="OPEN"
    else
        ssh_state="DOWN"
    fi

    echo "$ts DNS=$dns SSH=$ssh_state" >> "$LOGFILE"

    if [ "$dns" != "$last_dns" ]; then
        echo "$ts EVENT DNS_CHANGED old=$last_dns new=$dns" >> "$LOGFILE"
        last_dns="$dns"
    fi

    if [ "$ssh_state" != "$last_ssh" ]; then
        echo "$ts EVENT SSH_CHANGED old=$last_ssh new=$ssh_state" >> "$LOGFILE"
        last_ssh="$ssh_state"
    fi

    if [ "$dns" != "SERVFAIL" ] && [ "$ssh_state" = "OPEN" ]; then
        echo "$ts EVENT FULL_RECOVERY" >> "$LOGFILE"
    fi

    sleep 30
done
