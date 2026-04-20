#!/bin/bash

# Determina la directory dello script e la root della repo dinamicamente
SCRIPT_DIR=$(dirname "$(realpath "${BASH_SOURCE[0]}")")
REPO_ROOT=$(realpath "$SCRIPT_DIR/../../../")

# Helper per creare la cartella build in automatico se non presente,
# eseguire cmake con le opzioni desiderate, e tornare alla cartella in cui si era quando viene chiamato
cmake_config() {
    local original_dir=$(pwd)
    cd "$REPO_ROOT"

    if [[ ! -f "$REPO_ROOT/CMakeUserPresets.json" ]]; then
        setup_vscode_hidra
    fi

    cmake --preset hidra-configure --fresh

    cd "$original_dir"
}

# Helper per buildare il codice senza dover tornare a mano in build.
# Si sposta nella cartella eudaq_hidra/build, esegue make -j 10, e ritorna nella cartella di esecuzione

hidra_build() {
    local original_dir=$(pwd)
    cd "$REPO_ROOT"

    if [[ ! -f "$REPO_ROOT/CMakeUserPresets.json" ]]; then
        setup_vscode_hidra
    fi

    cmake --build --preset hidra-build
    cmake --build --preset hidra-install

    cd "$original_dir"
}

build_hidra() {
    hidra_build
}

# Crea un file locale CMakeUserPresets.json in root repo per usare i preset HiDRA in VSCode/CMake Tools.
# Il file e' pensato per uso locale e non deve essere committato.
setup_vscode_hidra() {
    local settings_path="$REPO_ROOT/.vscode/settings.json"

    mkdir -p "$REPO_ROOT/.vscode"

    cat > "$REPO_ROOT/CMakeUserPresets.json" << 'EOF'
{
    "version": 8,
    "include": [
        "user/hidra/misc/CMakePresets.hidra.json"
    ]
}
EOF

    if [[ -f "$settings_path" ]]; then
        if command -v jq >/dev/null 2>&1; then
            local tmp_settings
            tmp_settings=$(mktemp)
            if jq '. + {
                "cmake.useCMakePresets": "always",
                "cmake.defaultConfigurePreset": "hidra-configure",
                "cmake.defaultBuildPreset": "hidra-build",
                "cmake.configureOnOpen": false
            }' "$settings_path" > "$tmp_settings"; then
                mv "$tmp_settings" "$settings_path"
                echo "Merged CMake keys into $settings_path"
            else
                rm -f "$tmp_settings"
                echo "Warning: $settings_path is not valid JSON, skipping merge"
                echo "Please fix JSON or remove file and run setup_vscode_hidra again"
            fi
        else
            echo "Warning: jq not found, cannot merge into existing $settings_path"
            echo "Install jq or update VSCode CMake settings manually"
        fi
    else
        cat > "$settings_path" << 'EOF'
{
    "cmake.useCMakePresets": "always",
    "cmake.defaultConfigurePreset": "hidra-configure",
    "cmake.defaultBuildPreset": "hidra-build",
    "cmake.configureOnOpen": false
}
EOF
        echo "Created $settings_path"
    fi

    echo "Created $REPO_ROOT/CMakeUserPresets.json"
    echo "Inside VSCode you can now select the presets: hidra-configure / hidra-build / hidra-install / hidra-full"
}

# Rimuove il file locale dei preset utente per tornare allo stato iniziale.
clean_vscode_hidra() {
    local settings_path="$REPO_ROOT/.vscode/settings.json"

    rm -f "$REPO_ROOT/CMakeUserPresets.json"

    if [[ -f "$settings_path" ]]; then
        if command -v jq >/dev/null 2>&1; then
            local tmp_settings
            tmp_settings=$(mktemp)
            if jq 'del(
                ."cmake.useCMakePresets",
                ."cmake.defaultConfigurePreset",
                ."cmake.defaultBuildPreset",
                ."cmake.configureOnOpen"
            )' "$settings_path" > "$tmp_settings"; then
                mv "$tmp_settings" "$settings_path"
                echo "Removed HiDRA CMake keys from $settings_path"
            else
                rm -f "$tmp_settings"
                echo "Warning: $settings_path is not valid JSON, skipping cleanup of CMake keys"
            fi
        else
            echo "Warning: jq not found, cannot clean CMake keys from $settings_path"
        fi
    fi

    echo "Removed $REPO_ROOT/CMakeUserPresets.json"
}

# Verifica lo stato della configurazione locale VSCode/CMake per HiDRA.
check_vscode_hidra() {
    local user_presets_path="$REPO_ROOT/CMakeUserPresets.json"
    local settings_path="$REPO_ROOT/.vscode/settings.json"
    local status=0

    echo "[HiDRA VSCode setup check]"

    if command -v jq >/dev/null 2>&1; then
        echo "- jq: OK"
    else
        echo "- jq: MISSING (merge/cleanup non-distruttivo disabilitato)"
        status=1
    fi

    if [[ -f "$user_presets_path" ]]; then
        if grep -q '"user/hidra/misc/CMakePresets.hidra.json"' "$user_presets_path"; then
            echo "- CMakeUserPresets.json: OK"
        else
            echo "- CMakeUserPresets.json: PRESENT but missing HiDRA include"
            status=1
        fi
    else
        echo "- CMakeUserPresets.json: MISSING"
        status=1
    fi

    if [[ -f "$settings_path" ]]; then
        if command -v jq >/dev/null 2>&1; then
            local use_presets
            local configure_preset
            local build_preset
            local configure_on_open

            use_presets=$(jq -r '."cmake.useCMakePresets" // empty' "$settings_path" 2>/dev/null)
            configure_preset=$(jq -r '."cmake.defaultConfigurePreset" // empty' "$settings_path" 2>/dev/null)
            build_preset=$(jq -r '."cmake.defaultBuildPreset" // empty' "$settings_path" 2>/dev/null)
            configure_on_open=$(jq -r '."cmake.configureOnOpen" // empty' "$settings_path" 2>/dev/null)

            if [[ "$use_presets" == "always" && "$configure_preset" == "hidra-configure" && "$build_preset" == "hidra-build" && "$configure_on_open" == "false" ]]; then
                echo "- .vscode/settings.json CMake keys: OK"
            else
                echo "- .vscode/settings.json CMake keys: NOT matching HiDRA defaults"
                echo "  current: usePresets=$use_presets, configure=$configure_preset, build=$build_preset, configureOnOpen=$configure_on_open"
                status=1
            fi
        else
            echo "- .vscode/settings.json: PRESENT (cannot validate CMake keys without jq)"
        fi
    else
        echo "- .vscode/settings.json: MISSING"
        status=1
    fi

    if [[ $status -eq 0 ]]; then
        echo "Result: OK"
    else
        echo "Result: NOT READY (run setup_vscode_hidra)"
    fi

    return $status
}

# Helper per pulire la cartella di build e relative altre cartelle (lib, etc.).
# Si sposta nella cartella eudaq_hidra, esegue make_clean, e ritorna nella cartella di esecuzione
cmake_clean() {
    local original_dir=$(pwd)
    cd "$REPO_ROOT"
    sh "$REPO_ROOT/make_clean.sh"
    cd "$original_dir"
}

# Crea alias per tornare velocemente alle cartelle
alias build_dir="cd $REPO_ROOT/build"
alias hidra_run="cd $REPO_ROOT/user/hidra/run"
alias hidra_dir="cd $REPO_ROOT/user/hidra"
