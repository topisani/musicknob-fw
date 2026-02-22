#!/bin/env -S just --justfile

[private]
default:
    just --justfile {{justfile()}} --list

# DEVELOPMENT TOOL COMMANDS
# Generate MCUBOOT key. Must be run before the system can build, unless mcuboot is disabled
[no-exit-message]
mcuboot-generate-key:
    #!/bin/bash
    key_path=app/sysbuild/mcuboot_key.pem
    if [ -e $key_path ]; then
        echo "ERROR: MCUBOOT key already exists at '$key_path'. If you want to replace it, delete the old one"
        exit 1
    fi
    openssl ecparam -name prime256v1 -genkey -noout -out app/sysbuild/mcuboot_key.pem

# List all zephyr boards
board-list:
    @just west boards

# Clone a board to a new one
# [no-exit-message]
board-clone from to:
    #!/bin/bash
    set -exuo pipefail

    export from_lower={{ snakecase(from) }}
    export from_upper={{ shoutysnakecase(from) }}
    export to_lower={{ snakecase(to) }}
    export to_upper={{ shoutysnakecase(to) }}

    board_str=$(just west boards -f "{name}:{vendor}:{dir}" 2> /dev/null | grep "^{{ from }}:")
    board_str=${board_str//$'\r'}
    if [ -z "$board_str" ]; then
        echo "ERROR: Board '{{ from }}' Was not found. Call 'just board-list' to see available boards"
        exit 1
    fi

    # board_vendor=$(echo $board_str | cut -d: -f2)
    board_dir=$(echo $board_str | cut -d: -f3)

    to_dir=project/boards/$to_lower
    if [ -e "$to_dir" ]; then
        echo "ERROR: Board already exists at $to_dir. If you want to overwrite it, delete it first."
        exit 1
    fi
    mkdir -p project/boards
    cp -r $board_dir $to_dir

    grep -rl "$from_lower" $to_dir | xargs sed -i "s/${from_lower}/${to_lower}/g" || true
    grep -rl "$from_upper" $to_dir | xargs sed -i "s/${from_upper}/${to_upper}/g" || true

    cmds=$(find $to_dir -name "*${from_lower}*" -exec bash -c 'echo mv "$1" "${1/${from_lower}/${to_lower}}"' _ {} \;)
    echo "$cmds" | sh
    cat <<EOF
    Cloned board $from_lower to '$to_dir'.
    Make sure to manually clean up copied files, remove unneeded documentation, etc.
    Also, set the BOARD variable, either when building or the default in the justfile to '$to_lower'
    EOF

board-rm board:
    rm -rf project/boards/*/{{ board }}

