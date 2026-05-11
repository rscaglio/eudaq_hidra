#!/bin/bash

# Resolve the script directory and repository root dynamically.
SCRIPT_DIR=$(dirname "$(realpath "${BASH_SOURCE[0]}")")
REPO_ROOT=$(realpath "$SCRIPT_DIR/../../../")
REPO_RUN=$(realpath "$SCRIPT_DIR/../run/")

HIDRA_REQUIRED_CMAKE_VERSION="3.25.1"

# Compare two semantic versions: return 0 if $1 >= $2, otherwise 1.
version_ge() {
    local current="$1"
    local required="$2"
    local i
    local current_parts required_parts

    IFS='.' read -r -a current_parts <<< "$current"
    IFS='.' read -r -a required_parts <<< "$required"

    for ((i=0; i<3; i++)); do
        local c="${current_parts[i]:-0}"
        local r="${required_parts[i]:-0}"

        if ((10#$c > 10#$r)); then
            return 0
        fi
        if ((10#$c < 10#$r)); then
            return 1
        fi
    done

    return 0
}

get_cmake_version() {
    if ! command -v cmake >/dev/null 2>&1; then
        echo ""
        return 1
    fi

    cmake --version 2>/dev/null | head -n1 | awk '{print $3}'
}

supports_hidra_presets() {
    local cmake_version
    cmake_version=$(get_cmake_version)

    if [ -z "$cmake_version" ]; then
        return 1
    fi

    version_ge "$cmake_version" "$HIDRA_REQUIRED_CMAKE_VERSION"
}

# Helper to create the build directory if needed, run CMake with the expected
# options, and return to the caller's original directory.
cmake_config() {
    local original_dir=$(pwd)
    cd "$REPO_ROOT"

    if supports_hidra_presets; then
        if [ ! -f "$REPO_ROOT/CMakeUserPresets.json" ]; then
            setup_vscode_hidra
        fi

        cmake --preset hidra-configure --fresh
    else
        local cmake_version
        cmake_version=$(get_cmake_version)
        echo "CMake $cmake_version does not support HiDRA presets (required >= $HIDRA_REQUIRED_CMAKE_VERSION)."
        echo "Using the classic CMake configuration fallback."

        cmake --fresh -S "$REPO_ROOT" -B "$REPO_ROOT/build" -G "Unix Makefiles" \
            -DEUDAQ_BUILD_ONLINE_ROOT_MONITOR=OFF \
            -DEUDAQ_LIBRARY_BUILD_TTREE=OFF \
            -DUSER_HIDRA_BUILD=ON \
            -DUSER_HIDRA_DC_ROOT_OUTPUT=ON \
            -DCMAKE_EXPORT_COMPILE_COMMANDS=ON 
    fi

    cd "$original_dir"
}

# Helper to build the code without manually jumping into the build directory.
# It runs the HiDRA workflow when supported, otherwise it falls back to a
# regular build and install sequence.

hidra_build() {
    local original_dir=$(pwd)
    cd "$REPO_ROOT"

    if supports_hidra_presets; then
        if [ ! -f "$REPO_ROOT/CMakeUserPresets.json" ]; then
            setup_vscode_hidra
        fi

        cmake --workflow --preset hidra-full
    else
        local cmake_version
        cmake_version=$(get_cmake_version)
        echo "CMake $cmake_version does not support HiDRA presets/workflow (required >= $HIDRA_REQUIRED_CMAKE_VERSION)."
        echo "Using the classic build/install fallback."

        cmake_config
        cmake --build "$REPO_ROOT/build" -j 10
        cmake --build "$REPO_ROOT/build" --target install -j 10
    fi

    cd "$original_dir"
}

build_hidra() {
    hidra_build
}

runhidra(){
    cd "$REPO_RUN"
    ./hidra_startrun_dry.sh
}
   

# Create a local CMakeUserPresets.json file in the repository root so VSCode/CMake Tools
# can use the HiDRA presets. This file is meant for local use and should not be committed.
setup_vscode_hidra() {
    local settings_path="$REPO_ROOT/.vscode/settings.json"

    mkdir -p "$REPO_ROOT/.vscode"

    cat > "$REPO_ROOT/CMakeUserPresets.json" << 'EOF'
{
        "version": 6,
    "include": [
        "user/hidra/misc/CMakePresets.hidra.json"
    ]
}
EOF

    if [ -f "$settings_path" ]; then
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

# Remove the local user presets file and restore the initial state.
clean_vscode_hidra() {
    local settings_path="$REPO_ROOT/.vscode/settings.json"

    rm -f "$REPO_ROOT/CMakeUserPresets.json"

    if [ -f "$settings_path" ]; then
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

# Check the state of the local VSCode/CMake configuration for HiDRA.
check_vscode_hidra() {
    local user_presets_path="$REPO_ROOT/CMakeUserPresets.json"
    local settings_path="$REPO_ROOT/.vscode/settings.json"
    local status=0

    echo "[HiDRA VSCode setup check]"

    if command -v jq >/dev/null 2>&1; then
        echo "- jq: OK"
    else
        echo "- jq: MISSING (non-destructive merge/cleanup disabled)"
        status=2
    fi

    if [ -f "$user_presets_path" ]; then
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

    if [ -f "$settings_path" ]; then
        if command -v jq >/dev/null 2>&1; then
            local use_presets
            local configure_preset
            local build_preset
            local configure_on_open

            use_presets=$(jq -r 'if has("cmake.useCMakePresets") then ."cmake.useCMakePresets" else "__missing__" end' "$settings_path" 2>/dev/null)
            configure_preset=$(jq -r 'if has("cmake.defaultConfigurePreset") then ."cmake.defaultConfigurePreset" else "__missing__" end' "$settings_path" 2>/dev/null)
            build_preset=$(jq -r 'if has("cmake.defaultBuildPreset") then ."cmake.defaultBuildPreset" else "__missing__" end' "$settings_path" 2>/dev/null)
            configure_on_open=$(jq -r 'if has("cmake.configureOnOpen") then (."cmake.configureOnOpen"|tostring) else "__missing__" end' "$settings_path" 2>/dev/null)

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

    if [ $status -eq 0 ]; then
        echo "Result: OK"
    else 
        if [ $status -eq 1 ]; then
            echo "Result: NOT READY (run setup_vscode_hidra)"
        else
            echo "Result: MISSING DEPENDENCIES (run sudo apt install jq)"
        fi
    fi

    return $status
}

# Helper to clean the build directory and related output folders (lib, etc.).
# It runs make_clean.sh from the repository root and returns to the caller's directory.
cmake_clean() {
    local original_dir=$(pwd)
    cd "$REPO_ROOT"
    sh "$REPO_ROOT/make_clean.sh"
    cd "$original_dir"
}

# Convenience aliases for quickly jumping to common directories.
alias build_dir="cd $REPO_ROOT/build"
alias hidra_run="cd $REPO_ROOT/user/hidra/run"
alias hidra_dir="cd $REPO_ROOT/user/hidra"
