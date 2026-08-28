#!/bin/sh
set -eu
script_dir=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
bin="$script_dir/.agent_protocol_test"
trap 'rm -f "$bin"' EXIT
cc -std=c11 -Wall -Wextra -Werror "$script_dir/agent_protocol_test.c" "$script_dir/../drivers/velawear_agent_protocol.c" -o "$bin"
"$bin"
