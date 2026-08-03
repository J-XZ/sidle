#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "${SCRIPT_DIR}/../third_party/YCSB-cpp" && pwd)"

OUTPUT_DIR=""
BUILD_DIR="${REPO_ROOT}/build"
WORKLOAD_FILE="${REPO_ROOT}/workloads/workloadc"
RUN_NAME=""
PHASE="both"
NODES=3
THREADS_PER_NODE=3
RECORD_COUNT=30000
OPERATION_COUNT=30000
FIELD_COUNT=10
FIELD_LENGTH=64
REQUEST_DISTRIBUTION="zipfian"
KEY_PREFIX="user"
ZERO_PADDING=16
FORCE=0
EXTRA_PROPS=()
UPDATE_READS_BEFORE_WRITE=0

usage() {
  cat <<'USAGE'
Usage:
  scripts/generate_dsidle_trace.sh --output-dir DIR [options]

Generates D-SIDLE trace files matching doc/trace/README.md:
  DIR/load/worker0.txt ...
  DIR/<run-name>/worker0.txt ...
  DIR/manifest.txt

Options:
  --output-dir DIR             Required trace output root.
  --build-dir DIR              CMake build directory. Default: ./build.
  --workload FILE              YCSB workload property file. Default: workloads/workloadc.
  --run-name NAME              Run phase directory name. Default: derived from workload filename.
  --phase load|run|both        Which trace set to generate. Default: both.
  --nodes N                    VM/node count. Default: 3.
  --threads-per-node N         Worker files per node. Default: 3.
  --record-count N             YCSB recordcount and load op count. Default: 30000.
  --operation-count N          YCSB transaction operationcount. Default: 30000.
  --field-count N              YCSB fieldcount. Default: 10.
  --field-length N             YCSB fieldlength. Default: 64.
  --request-distribution DIST  uniform|zipfian|latest. Default: zipfian.
  --key-prefix PREFIX          Key prefix. Default: user.
  --zero-padding N             Key numeric zero padding. Default: 16.
  --update-read-before-write   Emit UPDATE as GET then PUT for each update record.
  --property K=V               Extra YCSB -p property; may repeat.
  --force                      Remove existing phase output directories first.
  -h, --help                   Show this help.

The total number of generated worker files is nodes * threads-per-node.
Output layout:
  DIR/load/worker0.txt ...              (load phase)
  DIR/<run-name>/worker0.txt ...        (run phase)
  DIR/manifest.txt

For a 3 VM x 3 worker run, e2e_trace should assign:
  node0: trace_first=0, trace_workers=3
  node1: trace_first=3, trace_workers=3
  node2: trace_first=6, trace_workers=3
USAGE
}

is_positive_int() {
  [[ "$1" =~ ^[1-9][0-9]*$ ]]
}

while [[ $# -gt 0 ]]; do
  case "$1" in
    --output-dir)
      OUTPUT_DIR="$2"
      shift 2
      ;;
    --build-dir)
      BUILD_DIR="$2"
      shift 2
      ;;
    --workload)
      WORKLOAD_FILE="$2"
      shift 2
      ;;
    --phase)
      PHASE="$2"
      shift 2
      ;;
    --run-name)
      RUN_NAME="$2"
      shift 2
      ;;
    --nodes)
      NODES="$2"
      shift 2
      ;;
    --threads-per-node)
      THREADS_PER_NODE="$2"
      shift 2
      ;;
    --record-count)
      RECORD_COUNT="$2"
      shift 2
      ;;
    --operation-count)
      OPERATION_COUNT="$2"
      shift 2
      ;;
    --field-count)
      FIELD_COUNT="$2"
      shift 2
      ;;
    --field-length)
      FIELD_LENGTH="$2"
      shift 2
      ;;
    --request-distribution)
      REQUEST_DISTRIBUTION="$2"
      shift 2
      ;;
    --key-prefix)
      KEY_PREFIX="$2"
      shift 2
      ;;
    --zero-padding)
      ZERO_PADDING="$2"
      shift 2
      ;;
    --update-read-before-write)
      UPDATE_READS_BEFORE_WRITE=1
      shift
      ;;
    --property)
      EXTRA_PROPS+=("$2")
      shift 2
      ;;
    --force)
      FORCE=1
      shift
      ;;
    -h|--help)
      usage
      exit 0
      ;;
    *)
      echo "unknown option: $1" >&2
      usage >&2
      exit 2
      ;;
  esac
done

if [[ -z "$OUTPUT_DIR" ]]; then
  echo "--output-dir is required" >&2
  usage >&2
  exit 2
fi
case "$PHASE" in
  load|run|both) ;;
  *)
    echo "--phase must be load, run, or both" >&2
    exit 2
    ;;
esac
for v in "$NODES" "$THREADS_PER_NODE" "$RECORD_COUNT" "$OPERATION_COUNT" \
         "$FIELD_COUNT" "$FIELD_LENGTH" "$ZERO_PADDING"; do
  if ! is_positive_int "$v"; then
    echo "numeric options must be positive integers, got: $v" >&2
    exit 2
  fi
done
if [[ "$KEY_PREFIX" =~ ^[0-9] ]]; then
  echo "--key-prefix must not start with a digit for D-SIDLE trace parsing" >&2
  exit 2
fi
if [[ ! -f "$WORKLOAD_FILE" ]]; then
  echo "workload file not found: $WORKLOAD_FILE" >&2
  exit 2
fi

if [[ -z "$RUN_NAME" ]]; then
  RUN_NAME="$(basename "$WORKLOAD_FILE")"
fi

TOTAL_WORKERS=$((NODES * THREADS_PER_NODE))
YCSB_BIN="${BUILD_DIR}/ycsb"

if [[ ! -x "$YCSB_BIN" ]]; then
  cmake -S "$REPO_ROOT" -B "$BUILD_DIR" \
    -DCMAKE_BUILD_TYPE=Release \
    -DBIND_ROCKSDB=OFF \
    -DBIND_LMDB=OFF \
    -DBIND_LEVELDB=OFF \
    -DBIND_WIREDTIGER=OFF
  cmake --build "$BUILD_DIR" --target ycsb --parallel "${DSIDLE_YCSB_BUILD_JOBS:-4}"
fi

prepare_phase_dir() {
  local dir="$1"
  if [[ -e "$dir" ]]; then
    if [[ "$FORCE" -eq 1 ]]; then
      rm -rf "$dir"
    elif [[ -n "$(find "$dir" -mindepth 1 -maxdepth 1 -print -quit)" ]]; then
      echo "output phase directory already exists and is not empty: $dir" >&2
      echo "pass --force to replace it" >&2
      exit 2
    fi
  fi
  mkdir -p "$dir"
}

run_ycsb_phase() {
  local phase_name="$1"
  local phase_dir="$2"
  local mode_flag="$3"
  local op_count="$4"
  prepare_phase_dir "$phase_dir"

  local cmd=(
    "$YCSB_BIN"
    "$mode_flag"
    -db trace
    -threads "$TOTAL_WORKERS"
    -P "$WORKLOAD_FILE"
    -p "recordcount=${RECORD_COUNT}"
    -p "operationcount=${op_count}"
    -p "fieldcount=${FIELD_COUNT}"
    -p "fieldlength=${FIELD_LENGTH}"
    -p "requestdistribution=${REQUEST_DISTRIBUTION}"
    -p "keyprefix=${KEY_PREFIX}"
    -p "zeropadding=${ZERO_PADDING}"
    -p "trace.output_dir=${phase_dir}"
    -p "trace.first_worker=0"
    -p "trace.header=true"
  )
  if [[ "$UPDATE_READS_BEFORE_WRITE" -eq 1 ]]; then
    cmd+=(-p "trace.update_reads_before_write=true")
  fi
  for prop in "${EXTRA_PROPS[@]}"; do
    cmd+=(-p "$prop")
  done

  echo "generating ${phase_name} trace: workers=${TOTAL_WORKERS} dir=${phase_dir}"
  "${cmd[@]}"
}

mkdir -p "$OUTPUT_DIR"
LOAD_DIR="${OUTPUT_DIR}/load"
RUN_DIR="${OUTPUT_DIR}/${RUN_NAME}"
if [[ "$UPDATE_READS_BEFORE_WRITE" -eq 1 ]]; then
  UPDATE_READS_BEFORE_WRITE_VALUE=true
else
  UPDATE_READS_BEFORE_WRITE_VALUE=false
fi
case "$PHASE" in
  load)
    run_ycsb_phase load "$LOAD_DIR" -load "$RECORD_COUNT"
    ;;
  run)
    run_ycsb_phase "$RUN_NAME" "$RUN_DIR" -run "$OPERATION_COUNT"
    ;;
  both)
    run_ycsb_phase load "$LOAD_DIR" -load "$RECORD_COUNT"
    run_ycsb_phase "$RUN_NAME" "$RUN_DIR" -run "$OPERATION_COUNT"
    ;;
esac

cat >"${OUTPUT_DIR}/manifest.txt" <<MANIFEST
nodes=${NODES}
threads_per_node=${THREADS_PER_NODE}
total_workers=${TOTAL_WORKERS}
record_count=${RECORD_COUNT}
operation_count=${OPERATION_COUNT}
field_count=${FIELD_COUNT}
field_length=${FIELD_LENGTH}
request_distribution=${REQUEST_DISTRIBUTION}
key_prefix=${KEY_PREFIX}
zero_padding=${ZERO_PADDING}
workload_file=${WORKLOAD_FILE}
run_name=${RUN_NAME}
load_dir=${LOAD_DIR}
run_dir=${RUN_DIR}
update_reads_before_write=${UPDATE_READS_BEFORE_WRITE_VALUE}
MANIFEST

echo "trace generation complete: ${OUTPUT_DIR}"
