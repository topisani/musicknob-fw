set positional-arguments := true
set dotenv-load := true

# These need to be set in a local .env file. see example.env
export APP := env("APP", "project/app")
export BOARD := env("BOARD", "esp32c6_supermini/esp32c6/hpcore")
export BUILD_TYPE := env("BUILD_TYPE", "Debug")

export SYSBUILD := env("SYSBUILD", "false")
export WEST_RUNNER := env("WEST_RUNNER", "")
export WEST_RUNNER_ARGS := env("WEST_RUNNER_ARGS", "")
export BUILD_DIR := env("BUILD_DIR", "build/" + BOARD)
export BUILD_ARGS := env("BUILD_ARGS", "")

runner_args := if WEST_RUNNER != "" { "-r " + WEST_RUNNER } else { "" }

[private]
@default:
    just --list

# Build the project. Set the BUILD_TYPE variable to `Debug` or `Release`. Defaults to `Debug`
build *args: generate-version west-setup
    just west build {{APP}} "$@"

# Open the menuconfig tool
menuconfig:
    just build -t menuconfig

# Clean all build directories
clean:
    rm -rf .cache build compile_commands.json

# Flash from within the docker image
flash *args:
    just west flash -d {{BUILD_DIR}} {{ runner_args }} {{ WEST_RUNNER_ARGS }} "$@"

# Run west from the virtual environment
west *args:
    #!/bin/sh
    [ -e .venv/bin/activate ] || (echo "venv not found, run just init" && exit 1)
    . .venv/bin/activate && west "$@"

debug *args:
    just west debug -d {{BUILD_DIR}} {{ runner_args }} {{ WEST_RUNNER_ARGS }} "$@"


rtt *args:
    just west rtt {{ runner_args }} {{ WEST_RUNNER_ARGS }} "$@"

# Initialize the zephyr workspace
init:
    #!/bin/sh
    set -ex
    python -m venv --system-site-packages .venv
    . .venv/bin/activate
    pip install west

    [ -e .west ] || west init -l project
    west update

    west packages pip --install


[private]
west-setup:
    just west config build.board {{BOARD}}
    just west config build.sysbuild {{SYSBUILD}}
    just west config build.pristine auto
    just west config build.dir-fmt "{{BUILD_DIR}}"
    just west config build.guess-dir runners
    just west config build.cmake-args -- " \
        -DCMAKE_EXPORT_COMPILE_COMMANDS=ON \
        -DCMAKE_BUILD_TYPE={{BUILD_TYPE}} \
        -DNO_BUILD_TYPE_WARNING=ON \
        {{BUILD_ARGS}} \
    "

# Compute the rev and version using either CI_COMMIT_TAG which is set by GitLab in CI, or `git describe` for local builds
# Rev: the full version string with possible commit or -dirty suffix. Example: v0.4.0-rc1-dirty or v1.0.0
[private]
generate-version:
    #!/bin/bash
    set -e
    rev="${FW_REV:-${CI_COMMIT_TAG:-}}";
    [ "$rev" ] || rev=$(git describe --dirty 2> /dev/null) || rev="v0.0.0"
    rev=${rev#v}
    [ "$BUILD_TYPE" = "release" ] || rev="$rev-dev"
    echo "Version determined: $rev" >&2
    version_file={{APP}}/VERSION
    if ! [ -e "$version_file" ]; then
        echo "$version_file does not exist. Not updating" >&2
        exit 0
    fi
    echo "Updating version file $version_file:" >&2
    version=${rev%%-*}
    version_suffix=${rev#*-}
    version_file_contents=$(cat <<EOF
    VERSION_MAJOR = $(echo $version | cut -d. -f1)
    VERSION_MINOR = $(echo $version | cut -d. -f2)
    PATCHLEVEL = $(echo $version | cut -d. -f3)
    VERSION_TWEAK = 0
    EXTRAVERSION = $(echo $version_suffix | tr -d [\\-+_])
    EOF
    )
    [[ "$version_file_contents" != $(cat "$version_file") ]] && echo "$version_file_contents" | tee "$version_file" || exit 0


# Generate detailed overview of memory ressources in your project
ram-report:
    just west build -t ram_report

# Web browser for overview of project memory usage
puncover:
    #!/bin/sh
    ELF_PATH="{{BUILD_DIR}}/zephyr/zephyr.elf"
    puncover --elf_file=\"$ELF_PATH\" --host=0.0.0.0

docker-build:
    docker build -t zephyr-builder . -f Dockerfile

# Run command in docker container. Ex: `just docker-run just build`
docker-run *args: docker-build
    docker run --privileged -it zephyr-builder "$@"

# # Build a full release package in docker
# build-release: docker-build
#     #!/bin/sh
#     set -xe
#     id=$(docker run --rm -d zephyr-builder sleep infinity)
#     # trap "docker stop $id" EXIT INT HUP TERM
#     docker exec  $id ./build-release.sh
#     docker cp $id:/workdir/release .

# Subcommands for development only
dev *args:
    @just --justfile dev.just "$@"
