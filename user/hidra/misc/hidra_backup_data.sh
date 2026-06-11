#!/usr/bin/env bash

set -euo pipefail

##############################################################################
# Configuration
##############################################################################

if [[ -z "${EUDAQHIDRA:-}" ]]; then
    echo "ERROR: EUDAQHIDRA environment variable is not set."
    exit 1
fi


LOCAL_DIR_DATA="${EUDAQHIDRA}/run/out_data"
BACKUP_DIR_DATA="/home/eudaq/cernbox/TB2026_H8/raw"
LOCAL_DIR_LOG="${EUDAQHIDRA}/run/logs"
BACKUP_DIR_LOG="/home/eudaq/cernbox/TB2026_H8/logs"
LOCAL_DIR_TRACKER_DATA="/home/eudaq/TB2026_TrackerData"
BACKUP_DIR_TRACKER_DATA="/home/eudaq/cernbox/TB2026_H8/tracker_data"
BACKUP_DATE_FILE="/home/eudaq/TB2026_H8_last_backup_dates.log"

##############################################################################
# Transfer
##############################################################################


echo "============================================================"
echo "Starting local rsync"
echo "Source data      : $LOCAL_DIR_DATA"
echo "Destination data : $BACKUP_DIR_DATA"
echo "Source log       : $LOCAL_DIR_LOG"
echo "Destination log  : $BACKUP_DIR_LOG"
echo "============================================================"

rsync \
    -av \
    --update \
    --partial \
    --human-readable \
    --stats \
    "${LOCAL_DIR_DATA}/" \
    "${BACKUP_DIR_DATA}/"

rsync \
    -av \
    --update \
    --partial \
    --human-readable \
    --stats \
    "${LOCAL_DIR_LOG}/" \
    "${BACKUP_DIR_LOG}/"

rsync \
    -av \
    --update \
    --partial \
    --human-readable \
    --stats \
    "${LOCAL_DIR_TRACKER_DATA}/" \
    "${BACKUP_DIR_TRACKER_DATA}/"

date '+%Y-%m-%d %H:%M:%S %Z' >> "${BACKUP_DATE_FILE}"

echo "============================================================"
echo "Done."
echo "============================================================"



