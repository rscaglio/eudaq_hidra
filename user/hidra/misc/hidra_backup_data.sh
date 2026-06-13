#!/usr/bin/env bash

set -euo pipefail

##############################################################################
# Configuration
##############################################################################

if [[ -z "${EUDAQHIDRA:-}" ]]; then
    echo "ERROR: EUDAQHIDRA environment variable is not set."
    exit 1
fi


LOCAL_DIR_DATA="/home/eudaq/daq/TB2026_HidraData/HidraData"
BACKUP_DIR_DATA="/home/eudaq/cernbox/TB2026_H8/raw"
LOCAL_DIR_LOG="/home/eudaq/daq/TB2026_HidraData/Logs"
BACKUP_DIR_LOG="/home/eudaq/cernbox/TB2026_H8/logs"
LOCAL_DIR_TRACKER_DATA="/home/eudaq/TB2026_TrackerData/ascii_dream_2026"
BACKUP_DIR_TRACKER_DATA="/home/eudaq/cernbox/TB2026_H8/tracker_data/ascii_dream_2026"
BACKUP_DATE_FILE="/home/eudaq/TB2026_H8_last_backup_dates.log"

# Shared rsync options, used for every transfer so they stay consistent.
RSYNC_OPTS=(
    -av
    --update
    --partial
    --human-readable
    --stats
)

##############################################################################
# Transfer
##############################################################################


echo "============================================================"
echo "Starting local rsync"
echo "Source data         : $LOCAL_DIR_DATA"
echo "Destination data    : $BACKUP_DIR_DATA"
echo "Source log          : $LOCAL_DIR_LOG"
echo "Destination log     : $BACKUP_DIR_LOG"
echo "Source tracker      : $LOCAL_DIR_TRACKER_DATA"
echo "Destination tracker : $BACKUP_DIR_TRACKER_DATA"
echo "============================================================"

rsync "${RSYNC_OPTS[@]}" "${LOCAL_DIR_DATA}/"         "${BACKUP_DIR_DATA}/"
rsync "${RSYNC_OPTS[@]}" "${LOCAL_DIR_LOG}/"          "${BACKUP_DIR_LOG}/"
rsync "${RSYNC_OPTS[@]}" "${LOCAL_DIR_TRACKER_DATA}/" "${BACKUP_DIR_TRACKER_DATA}/"

# Record the backup timestamp only after every transfer above succeeded.
# `set -euo pipefail` aborts the script on the first failing rsync, so this
# line is reached only on a fully successful backup.
date '+%Y-%m-%d %H:%M:%S %Z' >> "${BACKUP_DATE_FILE}"

echo "============================================================"
echo "Done."
echo "============================================================"
