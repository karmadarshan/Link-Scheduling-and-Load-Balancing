#!/usr/bin/env bash
#
# run_experiments.sh — reproduces the six A28 experiment runs:
#
#   fcfs, sjf, rr, drr        -- one run each at the reference config
#   fcfs, rr                  -- one run each at server_threads = 1
#
# Each run: starts ./server with the right --sched/--quantum/--config,
# waits for it to come up, runs `./client load` for --requests requests
# (closed-loop, per A4), sends SIGINT so the server drains and writes
# its CSV (A26), and collects server/client logs alongside the CSV in
# the output directory.
#
# Usage:
#   ./run_experiments.sh                 # all six runs
#   REQUESTS=200 ./run_experiments.sh    # quick smoke test (A28 needs >= 1000)
#
# Prerequisites:
#   - `make` has been run (./server and ./client exist in this directory)
#   - workload/ exists (python3 generate_workload.py --outdir workload --quantum 4096)
#   - config.json (reference) and config_1thread.json (server_threads=1)
#   - run from a native Linux filesystem (under WSL: ~/ , NOT /mnt/c/...);
#     the Windows drive makes file I/O 10-100x slower and distorts results
#
set -u

SERVER=./server
CLIENT=./client
WORKLOAD_DIR=workload
OUT_DIR=${OUT_DIR:-experiments}
CONFIG_REF=config.json
CONFIG_1THREAD=config_1thread.json
QUANTUM=4096                    # A27: kept in the required 2-16KB range
REQUESTS=${REQUESTS:-1500}      # A28: at least 1000 requests per run

if [[ "$(pwd -P)" == /mnt/* ]]; then
    echo "WARNING: running from $(pwd -P) (Windows drive under WSL)." >&2
    echo "         File I/O is very slow there and will distort the results." >&2
    echo "         Copy the project to ~/ and run it from there." >&2
    sleep 3
fi

if [[ ! -x "$SERVER" || ! -x "$CLIENT" ]]; then
    echo "error: $SERVER / $CLIENT not found or not executable — run 'make' first" >&2
    exit 1
fi

if [[ ! -d "$WORKLOAD_DIR" ]]; then
    echo "error: workload directory '$WORKLOAD_DIR' not found — run generate_workload.py first" >&2
    exit 1
fi

for c in "$CONFIG_REF" "$CONFIG_1THREAD"; do
    [[ -f "$c" ]] || { echo "error: $c not found" >&2; exit 1; }
done

mkdir -p "$OUT_DIR"

# Absolute paths, so the client can run from a scratch directory.
ABS_CLIENT=$(realpath "$CLIENT")
ABS_WORKLOAD=$(realpath "$WORKLOAD_DIR")
NWORK=$(find "$ABS_WORKLOAD" -maxdepth 1 -type f | wc -l)

SERVER_PID=""
SCRATCH=""
cleanup() {
    [[ -n "$SERVER_PID" ]] && kill -9 "$SERVER_PID" 2>/dev/null
    [[ -n "$SCRATCH" ]] && rm -rf "$SCRATCH"
}
trap cleanup EXIT

# wait_for_port <host> <port> <timeout_seconds>
# Polls until the server's listening socket accepts a connection.
wait_for_port() {
    local host="$1" port="$2" timeout="${3:-5}"
    local tries=$(( timeout * 10 )) i
    for (( i = 0; i < tries; i++ )); do
        if bash -c "exec 3<>/dev/tcp/${host}/${port}" 2>/dev/null; then
            return 0
        fi
        sleep 0.1
    done
    echo "error: server never came up on ${host}:${port}" >&2
    return 1
}

# run_one <name> <sched> <config> [quantum]
run_one() {
    local name="$1" sched="$2" config="$3" quantum="${4:-}"
    local metrics_csv="${OUT_DIR}/${name}.csv"
    local server_log="${OUT_DIR}/${name}_server.log"
    local client_log="${OUT_DIR}/${name}_client.log"

    echo "=== ${name} (sched=${sched}, config=${config}${quantum:+, quantum=$quantum}) ==="

    # Scratch space (server storage + client working dir), removed after the
    # run so nothing accumulates in the project folder.
    SCRATCH=$(mktemp -d)
    mkdir -p "$SCRATCH/server_files" "$SCRATCH/client_cwd"
    rm -f "$metrics_csv"

    local -a sched_args=(--sched "$sched" --file "$SCRATCH/server_files"
                          --config "$config" --metrics-out "$metrics_csv")
    if [[ -n "$quantum" ]]; then
        sched_args+=(--quantum "$quantum")
    fi

    "$SERVER" "${sched_args[@]}" > "$server_log" 2>&1 &
    SERVER_PID=$!

    local ip port
    ip=$(python3 -c "import json;print(json.load(open('${config}'))['server']['ip'])")
    port=$(python3 -c "import json;print(json.load(open('${config}'))['server']['port'])")

    if ! wait_for_port "$ip" "$port" 5; then
        return 1
    fi

    # Client runs in the scratch dir: `get` writes downloaded files into the
    # current directory (A3), which must not be the project folder.
    ( cd "$SCRATCH/client_cwd" && \
      "$ABS_CLIENT" load "$ABS_WORKLOAD" --requests "$REQUESTS" \
          --config "$(realpath "$OLDPWD/$config")" ) > "$client_log" 2>&1

    kill -INT "$SERVER_PID"
    wait "$SERVER_PID" 2>/dev/null
    SERVER_PID=""

    rm -rf "$SCRATCH"
    SCRATCH=""

    # Sanity checks: seed PUTs + REQUESTS rows, and no failed seeds.
    local rows expected=$(( REQUESTS + NWORK ))
    rows=$(( $(wc -l < "$metrics_csv" 2>/dev/null || echo 1) - 1 ))
    if (( rows != expected )); then
        echo "    WARNING: ${metrics_csv} has ${rows} rows, expected ${expected}" >&2
    fi
    if grep -q "seed PUT failed" "$client_log"; then
        echo "    WARNING: a seed PUT failed, see ${client_log}" >&2
    fi

    echo "    -> ${metrics_csv} (${rows} rows), ${server_log}, ${client_log}"
    sleep 1     # let the listening port settle between runs
}

# Reference configuration: fcfs, sjf, rr, drr
run_one fcfs_ref fcfs "$CONFIG_REF"
run_one sjf_ref  sjf  "$CONFIG_REF"
run_one rr_ref   rr   "$CONFIG_REF" "$QUANTUM"
run_one drr_ref  drr  "$CONFIG_REF" "$QUANTUM"

# server_threads = 1: fcfs and rr only (A28)
run_one fcfs_1thread fcfs "$CONFIG_1THREAD"
run_one rr_1thread   rr   "$CONFIG_1THREAD" "$QUANTUM"

echo "All six runs complete. CSVs and logs are in ${OUT_DIR}/."
