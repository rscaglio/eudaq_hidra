#!/usr/bin/env sh

BINPATH=$EUDAQHIDRA/../../bin
TMUX_SESSION="hidra_run_monitoring"
DASHBOARD_DIR="$EUDAQHIDRA/misc/dashboard/rc_mon"

export PATH="$BINPATH:$PATH"

mkdir -p out_data logs

if [ -z "$EUDAQHIDRA" ]; then
    echo "Error: EUDAQHIDRA is not set."
    exit 1
fi

if [ ! -d "$DASHBOARD_DIR" ]; then
    echo "Error: dashboard directory does not exist: $DASHBOARD_DIR"
    exit 1
fi

# Close existing tmux session if it exists
if tmux has-session -t "$TMUX_SESSION" 2>/dev/null; then
    tmux kill-session -t "$TMUX_SESSION"
fi

# Start monitoring dashboard in a new tmux session
tmux new-session -d -s "$TMUX_SESSION" \
    "cd \"$DASHBOARD_DIR\" && php -S localhost:8080"

euRun -n HidraRunControl &
sleep 1

euLog > "$HOME/temp.eudaq.log" &
sleep 1

euCliMonitor  -n HidraMonitor -t DryHidraMonitor &
euCliCollector -n HidraDataCollector -t HidraDataCollector &
sleep 1

euCliProducer -n HidraDryFERSProducer -t DryFERSProducer &
euCliProducer -n HidraDryXDCProducer -t DryXDCProducer &

