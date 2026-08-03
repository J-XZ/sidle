cmake_minimum_required(VERSION 3.15)

# dsidle: the accepted RelWithDebInfo flags use Clang's full LTO spelling.
# Select the locally installed LLVM toolchain before project() unless callers
# explicitly selected a compiler themselves.
if(NOT DEFINED CMAKE_C_COMPILER)
  find_program(DSIDLE_CLANG_C clang-18)
  if(DSIDLE_CLANG_C)
    set(CMAKE_C_COMPILER "${DSIDLE_CLANG_C}" CACHE FILEPATH "" FORCE)
  endif()
endif()
if(NOT DEFINED CMAKE_CXX_COMPILER)
  find_program(DSIDLE_CLANG_CXX clang++-18)
  if(DSIDLE_CLANG_CXX)
    set(CMAKE_CXX_COMPILER "${DSIDLE_CLANG_CXX}" CACHE FILEPATH "" FORCE)
  endif()
endif()
project(D_SIDLE LANGUAGES C CXX)

option(DSIDLE_BUILD_LEGACY_SIDLE_BENCHMARKS
  "Build the archived single-node SIDLE benchmark facade" OFF)
if(DSIDLE_BUILD_LEGACY_SIDLE_BENCHMARKS)
  message(FATAL_ERROR
    "The upstream benchmark/, src/kv/, and src/helper.* facade is archived "
    "reference code and is not a supported D-SIDLE comparison path. "
    "Use dsidle_e2e_trace_runner or dsidle_e2e_suite_runner.")
endif()

option(DSIDLE_DISABLE_HARDWARE_SIMULATION
  "Compile out all hardware-simulation slow paths for overhead/assembly checks" OFF)

if(NOT CMAKE_CXX_COMPILER_ID STREQUAL "Clang")
  message(FATAL_ERROR "D-SIDLE requires Clang for the mandated -flto=full acceptance build")
endif()

include(CTest)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# dsidle: RelWithDebInfo is the only acceptance build.  Keep these flags local
# to D-SIDLE rather than inheriting the old single-node DAX build options.
if(CMAKE_BUILD_TYPE STREQUAL "RelWithDebInfo")
  add_compile_options(-O3 -g3 -march=native -flto=full)
  add_link_options(-flto=full)
endif()

add_library(dsidle STATIC dsidle/config.cc dsidle/shared_pool.cc dsidle/shard_allocator.cc dsidle/replica_directory.cc dsidle/latency_simulator.cc)
target_include_directories(dsidle PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_compile_definitions(
  dsidle PUBLIC DSIDLE_CMAKE_BUILD_TYPE="$<CONFIG>")
if(DSIDLE_DISABLE_HARDWARE_SIMULATION)
  target_compile_definitions(dsidle PUBLIC DSIDLE_DISABLE_HARDWARE_SIMULATION=1)
endif()
target_compile_options(dsidle PRIVATE -Wall -Wextra -Wpedantic)
target_link_libraries(dsidle PUBLIC atomic)

# dsidle: Build Masstree from this tree only. The legacy CXL allocator source
# is deliberately excluded; allocation conversion proceeds in the original
# Masstree files rather than through a compatibility backend.
add_library(dsidle_masstree STATIC
  dsidle/masstree_runtime.cc
  third_party/masstree-beta/compiler.cc
  third_party/masstree-beta/json.cc
  third_party/masstree-beta/kvthread.cc
  third_party/masstree-beta/log.cc
  third_party/masstree-beta/memdebug.cc
  third_party/masstree-beta/msgpack.cc
  third_party/masstree-beta/query_masstree.cc
  third_party/masstree-beta/string.cc
  third_party/masstree-beta/string_slice.cc
  third_party/masstree-beta/str.cc
  third_party/masstree-beta/straccum.cc
  third_party/masstree-beta/value_array.cc
  third_party/masstree-beta/value_string.cc
  third_party/masstree-beta/value_versioned_array.cc
  third_party/sidle_utils/sidle_frontend.cc
  third_party/sidle_utils/sidle_worker.cc)
target_include_directories(dsidle_masstree PUBLIC
  ${CMAKE_CURRENT_SOURCE_DIR}/third_party/masstree-beta
  ${CMAKE_CURRENT_SOURCE_DIR}/third_party/sidle_utils
  ${CMAKE_CURRENT_SOURCE_DIR}/third_party/art
  ${CMAKE_CURRENT_SOURCE_DIR})
target_compile_options(dsidle_masstree PRIVATE -include ${CMAKE_CURRENT_SOURCE_DIR}/third_party/masstree-beta/config.h -Wall -Wextra -Wpedantic)
target_link_libraries(dsidle_masstree PUBLIC dsidle pthread)

add_executable(dsidle_shared_pool dsidle/shared_pool_main.cc)
target_link_libraries(dsidle_shared_pool PRIVATE dsidle)

add_executable(dsidle_e2e_trace_runner dsidle/e2e_trace_runner.cc)
target_link_libraries(dsidle_e2e_trace_runner PRIVATE dsidle_masstree)
target_compile_options(dsidle_e2e_trace_runner PRIVATE -include ${CMAKE_CURRENT_SOURCE_DIR}/third_party/masstree-beta/config.h)
add_executable(dsidle_e2e_suite_runner dsidle/e2e_suite_runner.cc)
target_link_libraries(dsidle_e2e_suite_runner PRIVATE dsidle_masstree)
target_compile_options(dsidle_e2e_suite_runner PRIVATE -include ${CMAKE_CURRENT_SOURCE_DIR}/third_party/masstree-beta/config.h)

add_executable(dsidle_disabled_overhead_benchmark
  tests/disabled_overhead_benchmark.cc)
target_link_libraries(dsidle_disabled_overhead_benchmark PRIVATE dsidle)
target_compile_options(dsidle_disabled_overhead_benchmark PRIVATE -UNDEBUG)
add_test(NAME dsidle_disabled_overhead_benchmark
  COMMAND dsidle_disabled_overhead_benchmark)
set_tests_properties(dsidle_disabled_overhead_benchmark PROPERTIES
  WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR})
add_test(NAME dsidle_e2e_trace_runner_smoke
  COMMAND /bin/sh -c
    "truncate -s 128M /tmp/dsidle-e2e-runner.pool && \"$1\" --init-pool --config \"$2\" --node-control-capacity 1024 --max-threads-per-vm 5 && \"$3\" --config \"$2\" --phase smoke --node 0 --bootstrap"
    sh $<TARGET_FILE:dsidle_shared_pool> ${CMAKE_CURRENT_SOURCE_DIR}/tests/data/e2e_runner_config.jsonc $<TARGET_FILE:dsidle_e2e_trace_runner>)
set_tests_properties(dsidle_e2e_trace_runner_smoke PROPERTIES WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR})
add_test(NAME dsidle_e2e_trace_runner_min_duration
  COMMAND /bin/sh -c
    "truncate -s 128M /tmp/dsidle-e2e-runner-duration.pool && \"$1\" --init-pool --config \"$2\" --node-control-capacity 1024 --max-threads-per-vm 5 && \"$3\" --config \"$2\" --phase duration --node 0 --bootstrap --min-duration-sec 1 > /tmp/dsidle-e2e-duration.log && python3 -c 'import re,sys; x=re.search(r\"^E2E_TRACE_TIME_US .* duration_us=(\\d+)\", open(sys.argv[1]).read(), re.M); sys.exit(not x or int(x.group(1)) < 1000000)' /tmp/dsidle-e2e-duration.log"
    sh $<TARGET_FILE:dsidle_shared_pool> ${CMAKE_CURRENT_SOURCE_DIR}/tests/data/e2e_runner_config.jsonc $<TARGET_FILE:dsidle_e2e_trace_runner>)
set_tests_properties(dsidle_e2e_trace_runner_min_duration PROPERTIES WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR})
add_test(NAME dsidle_e2e_trace_runner_replica_activation
  COMMAND /bin/sh -c
    "truncate -s 128M /tmp/dsidle-e2e-runner.pool && \"$1\" --init-pool --config \"$2\" --node-control-capacity 1024 --max-threads-per-vm 5 && \"$3\" --config \"$2\" --phase seed --node 0 --bootstrap && \"$3\" --config \"$4\" --phase replica-read --node 0 --min-duration-sec 1 > /tmp/dsidle-e2e-replica.log && python3 -c 'import re,sys; x=re.search(r\"replica_bytes=(\\d+)\", open(sys.argv[1]).read()); sys.exit(not x or int(x.group(1)) <= 8)' /tmp/dsidle-e2e-replica.log"
    sh $<TARGET_FILE:dsidle_shared_pool> ${CMAKE_CURRENT_SOURCE_DIR}/tests/data/e2e_runner_replica_seed_config.jsonc $<TARGET_FILE:dsidle_e2e_trace_runner> ${CMAKE_CURRENT_SOURCE_DIR}/tests/data/e2e_runner_replica_read_config.jsonc)
set_tests_properties(dsidle_e2e_trace_runner_replica_activation PROPERTIES WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR})
add_test(NAME dsidle_e2e_trace_runner_2vm_smoke
  COMMAND /bin/sh -c
    "truncate -s 128M /tmp/dsidle-e2e-runner-2vm.pool && \"$1\" --init-pool --config \"$2\" --node-control-capacity 1024 --max-threads-per-vm 5 && { \"$3\" --config \"$2\" --phase smoke --node 0 --bootstrap > /tmp/dsidle-e2e-node0.log 2>&1 & p=$!; \"$3\" --config \"$2\" --phase smoke --node 1 > /tmp/dsidle-e2e-node1.log 2>&1 & q=$!; wait $p && wait $q; } && grep -q 'E2E_TRACE_TIME_US phase=smoke node=0' /tmp/dsidle-e2e-node0.log && grep -q 'E2E_TRACE_TIME_US phase=smoke node=1' /tmp/dsidle-e2e-node1.log"
    sh $<TARGET_FILE:dsidle_shared_pool> ${CMAKE_CURRENT_SOURCE_DIR}/tests/data/e2e_runner_2vm_config.jsonc $<TARGET_FILE:dsidle_e2e_trace_runner>)
set_tests_properties(dsidle_e2e_trace_runner_2vm_smoke PROPERTIES WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR})
add_test(NAME dsidle_ycsb_summarizer_tests
  COMMAND /bin/sh -c
    "out=$(mktemp -d /tmp/dsidle-ycsb-summary.XXXXXX); python3 \"$1\" --log-dir \"$2\" --out-dir \"$out\" && test -s \"$out/ycsb_summary.json\" && test -s \"$out/ycsb_summary.csv\" && test -s \"$out/YCSB实验报告.md\" && grep -q 'avg_ops_sum' \"$out/ycsb_summary.csv\" && grep -q 'ops_per_sec_p90' \"$out/ycsb_summary.csv\""
    sh ${CMAKE_CURRENT_SOURCE_DIR}/scripts/summarize_ycsb_experiment.py ${CMAKE_CURRENT_SOURCE_DIR}/tests/data/ycsb_logs)
set_tests_properties(dsidle_ycsb_summarizer_tests PROPERTIES WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR})
add_test(NAME dsidle_vm_e2e_harness_tests
  COMMAND python3 ${CMAKE_CURRENT_SOURCE_DIR}/tests/test_vm_e2e_harness.py
    ${CMAKE_CURRENT_SOURCE_DIR}/experiment_config.jsonc
    ${CMAKE_CURRENT_SOURCE_DIR}/scripts/summarize_vm_e2e.py
    ${CMAKE_CURRENT_SOURCE_DIR}/scripts/clear_dsidle_caches.py)
set_tests_properties(dsidle_vm_e2e_harness_tests PROPERTIES WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR})
add_test(NAME dsidle_acceptance_evidence_tests
  COMMAND python3 ${CMAKE_CURRENT_SOURCE_DIR}/tests/test_acceptance_evidence.py
    ${CMAKE_CURRENT_SOURCE_DIR}/scripts/acceptance_evidence.py
    ${CMAKE_CURRENT_SOURCE_DIR}/experiment_config.jsonc
    ${CMAKE_CURRENT_SOURCE_DIR}/scripts/run_dsidle_vm_e2e_rounds.sh
    ${CMAKE_CURRENT_SOURCE_DIR}/dsidle_run_ycsb_experiment.sh)
set_tests_properties(dsidle_acceptance_evidence_tests PROPERTIES WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR})
add_test(NAME dsidle_latency_access_audit_tests
  COMMAND python3 ${CMAKE_CURRENT_SOURCE_DIR}/tests/test_latency_access_audit.py
    ${CMAKE_CURRENT_SOURCE_DIR})
set_tests_properties(dsidle_latency_access_audit_tests PROPERTIES WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR})
add_test(NAME dsidle_ycsb_prepare_only_tests
  COMMAND /bin/sh -c
    "out=$(mktemp -d /tmp/dsidle-ycsb-prepare.XXXXXX); \"$1\" --prepare-only --out-dir \"$out/result\" --workloads a,e --record-count 10 --operation-count 20 --threads-per-node 1 --vm-count 1 --shared-size-mb 2048 --no-latency && \"$1\" --prepare-only --skip-trace-gen --out-dir \"$out/result\" --workloads a,e --record-count 10 --operation-count 20 --threads-per-node 1 --vm-count 1 --shared-size-mb 2048 --no-latency && test -s \"$out/result/run_meta.json\" && test -s \"$out/result/trace_manifest.json\" && python3 -c 'import json,sys; m=json.load(open(sys.argv[1])); t=json.load(open(sys.argv[2])); sys.exit(m[\"warmup_rounds\"] != 1 or m[\"value_seed\"] != 4851300051586183745 or m[\"sidle_background_roles_per_node\"] != 5 or m[\"background_epoch_slots_per_node\"] != 4 or m[\"epoch_slots_per_node\"] != 5 or m[\"runnable_threads_per_node\"] != 7 or m[\"shared_size_mb\"] != 2048 or m[\"requested_shared_size_mb\"] != 2048 or m[\"shared_memory_size_source\"] != \"explicit\" or len(t[\"trace_set_sha256\"]) != 64 or any(len(w[\"sha256\"]) != 64 for p in t[\"phases\"].values() for w in p[\"workers\"].values()))' \"$out/result/run_meta.json\" \"$out/result/trace_manifest.json\" && ! \"$1\" --prepare-only --out-dir \"$out/invalid\" --workloads a, >/dev/null 2>&1 && test ! -e \"$out/invalid\" && ! \"$1\" --prepare-only --out-dir \"$out/reserve\" --shared-reserve-mb 4096 >/dev/null 2>&1 && test ! -e \"$out/reserve\""
    sh ${CMAKE_CURRENT_SOURCE_DIR}/dsidle_run_ycsb_experiment.sh)
set_tests_properties(dsidle_ycsb_prepare_only_tests PROPERTIES WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR})
add_test(NAME dsidle_ycsb_trace_generation_tests
  COMMAND /bin/sh -c
    "out=$(mktemp -d /tmp/dsidle-ycsb-traces.XXXXXX); \"$1\" --prepare-only --out-dir \"$out/result\" --workloads a,d,e --record-count 10 --operation-count 10 --threads-per-node 1 --vm-count 1 --shared-size-mb 2048 --no-latency && test -s \"$out/result/traces/load/worker0.txt\" && test -s \"$out/result/traces/workloada/worker0.txt\" && test -s \"$out/result/traces/workloadd/worker0.txt\" && test -s \"$out/result/traces/workloade/worker0.txt\" && test -s \"$out/result/configs/experiment_config_ycsb_workloada.jsonc\" && grep -q \"$out/result/traces/workloada\" \"$out/result/configs/experiment_config_ycsb_workloada.jsonc\" && grep -R -q '^GET ' \"$out/result/traces/workloada\" && grep -R -q '^PUT ' \"$out/result/traces/workloada\" && grep -q '^request_distribution=latest$' \"$out/result/logs/manifest_workloadd.txt\" && python3 -c 'import json,sys; m=json.load(open(sys.argv[1])); sys.exit(m[\"phases\"][\"workloadd\"][\"request_distribution\"] != \"latest\" or m[\"fixed_key_size_bytes\"] != 32 or m[\"fixed_value_size_bytes\"] != 32)' \"$out/result/trace_manifest.json\""
    sh ${CMAKE_CURRENT_SOURCE_DIR}/dsidle_run_ycsb_experiment.sh)
set_tests_properties(dsidle_ycsb_trace_generation_tests PROPERTIES WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR})
add_test(NAME dsidle_e2e_rounds_script_smoke
  COMMAND /bin/sh -c
    "out=$(mktemp -d /tmp/dsidle-e2e-rounds.XXXXXX); \"$1\" --execute --smoke --rounds 1 --config \"$2\" --phase smoke --out-dir \"$out\" --runner \"$3\" --pool-tool \"$4\" --node-control-capacity 1024 --max-threads-per-vm 5 && test -s \"$out/round_logs/smoke_round_1.meta\" && grep -q 'E2E_TRACE_TIME_US phase=smoke node=1' \"$out/logs/smoke_round_1_run_node1.log\""
    sh ${CMAKE_CURRENT_SOURCE_DIR}/scripts/run_dsidle_e2e_rounds.sh ${CMAKE_CURRENT_SOURCE_DIR}/tests/data/e2e_runner_2vm_config.jsonc $<TARGET_FILE:dsidle_e2e_trace_runner> $<TARGET_FILE:dsidle_shared_pool>)
set_tests_properties(dsidle_e2e_rounds_script_smoke PROPERTIES WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR})
add_test(NAME dsidle_e2e_rounds_script_4vm_smoke
  COMMAND /bin/sh -c
    "out=$(mktemp -d /tmp/dsidle-e2e-rounds-4vm.XXXXXX); \"$1\" --execute --smoke --rounds 1 --config \"$2\" --phase smoke --out-dir \"$out\" --runner \"$3\" --pool-tool \"$4\" --node-control-capacity 1024 --max-threads-per-vm 5 && grep -q 'E2E_TRACE_TIME_US phase=smoke node=3' \"$out/logs/smoke_round_1_run_node3.log\""
    sh ${CMAKE_CURRENT_SOURCE_DIR}/scripts/run_dsidle_e2e_rounds.sh ${CMAKE_CURRENT_SOURCE_DIR}/tests/data/e2e_runner_4vm_config.jsonc $<TARGET_FILE:dsidle_e2e_trace_runner> $<TARGET_FILE:dsidle_shared_pool>)
set_tests_properties(dsidle_e2e_rounds_script_4vm_smoke PROPERTIES WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR})
add_test(NAME dsidle_e2e_ycsb_suite_smoke
  COMMAND /bin/sh -c
    "out=$(mktemp -d /tmp/dsidle-e2e-ycsb-suite.XXXXXX); \"$1\" --execute --smoke --suite ycsb --rounds 1 --load-config \"$2\" --run-config \"$2\" --out-dir \"$out\" --runner \"$3\" --pool-tool \"$4\" --node-control-capacity 1024 --max-threads-per-vm 5 && grep -q 'E2E_TRACE_TIME_US phase=load node=0' \"$out/logs/load_round_1_load_node0.log\" && grep -q 'E2E_TRACE_TIME_US phase=workloada node=1' \"$out/logs/workloada_round_1_run_node1.log\""
    sh ${CMAKE_CURRENT_SOURCE_DIR}/scripts/run_dsidle_e2e_rounds.sh ${CMAKE_CURRENT_SOURCE_DIR}/tests/data/e2e_runner_2vm_config.jsonc $<TARGET_FILE:dsidle_e2e_trace_runner> $<TARGET_FILE:dsidle_shared_pool>)
set_tests_properties(dsidle_e2e_ycsb_suite_smoke PROPERTIES WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR})
add_test(NAME dsidle_e2e08_suite_smoke
  COMMAND /bin/sh -c
    "out=$(mktemp -d /tmp/dsidle-e2e08-suite.XXXXXX); \"$1\" --execute --smoke --suite 08 --rounds 1 --config \"$2\" --out-dir \"$out\" --runner \"$3\" --pool-tool \"$4\" --node-control-capacity 65536 --max-threads-per-vm 5 && grep -q 'DSIDLE_E2E_SUITE_VERIFY suite=08 phase=e2e08_read node=3 status=ok' \"$out/logs/e2e08_read_round_1_read_node3.log\""
    sh ${CMAKE_CURRENT_SOURCE_DIR}/scripts/run_dsidle_e2e_rounds.sh ${CMAKE_CURRENT_SOURCE_DIR}/tests/data/e2e_runner_4vm_config.jsonc $<TARGET_FILE:dsidle_e2e_suite_runner> $<TARGET_FILE:dsidle_shared_pool>)
set_tests_properties(dsidle_e2e08_suite_smoke PROPERTIES WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR})
add_test(NAME dsidle_e2e09_suite_smoke
  COMMAND /bin/sh -c
    "out=$(mktemp -d /tmp/dsidle-e2e09-suite.XXXXXX); \"$1\" --execute --smoke --suite 09 --rounds 1 --config \"$2\" --out-dir \"$out\" --runner \"$3\" --pool-tool \"$4\" --node-control-capacity 65536 --max-threads-per-vm 5 && grep -q 'DSIDLE_E2E_SUITE_VERIFY suite=09 phase=e2e09_read node=3 status=ok' \"$out/logs/e2e09_read_round_1_read_node3.log\""
    sh ${CMAKE_CURRENT_SOURCE_DIR}/scripts/run_dsidle_e2e_rounds.sh ${CMAKE_CURRENT_SOURCE_DIR}/tests/data/e2e09_4vm_config.jsonc $<TARGET_FILE:dsidle_e2e_suite_runner> $<TARGET_FILE:dsidle_shared_pool>)
set_tests_properties(dsidle_e2e09_suite_smoke PROPERTIES WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR})
set_tests_properties(
  dsidle_e2e_trace_runner_smoke
  dsidle_e2e_trace_runner_min_duration
  dsidle_e2e_trace_runner_replica_activation
  dsidle_e2e_trace_runner_2vm_smoke
  dsidle_e2e_rounds_script_smoke
  dsidle_e2e_rounds_script_4vm_smoke
  dsidle_e2e_ycsb_suite_smoke
  dsidle_e2e08_suite_smoke
  dsidle_e2e09_suite_smoke
  PROPERTIES RESOURCE_LOCK dsidle_e2e_pool)
add_test(NAME dsidle_vm_scripts_dry_run
  COMMAND /bin/sh -c
    "\"$1\" --dry-run --apply-host-tuning --config \"$2\" && \"$3\" --dry-run --config \"$2\" && \"$4\" --dry-run --config \"$2\" && \"$5\" --dry-run"
    sh ${CMAKE_CURRENT_SOURCE_DIR}/dsidle_init_vms.sh ${CMAKE_CURRENT_SOURCE_DIR}/experiment_config.jsonc ${CMAKE_CURRENT_SOURCE_DIR}/dsidle_check_vms.sh ${CMAKE_CURRENT_SOURCE_DIR}/dsidle_kill_vms.sh ${CMAKE_CURRENT_SOURCE_DIR}/dsidle_make_vm_img.sh)
set_tests_properties(dsidle_vm_scripts_dry_run PROPERTIES WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR})
add_test(NAME dsidle_init_vm_topology_tests
  COMMAND python3 ${CMAKE_CURRENT_SOURCE_DIR}/tests/test_dsidle_init_vm.py)
set_tests_properties(dsidle_init_vm_topology_tests PROPERTIES WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR})

add_executable(dsidle_config_tests tests/config_tests.cc)
target_link_libraries(dsidle_config_tests PRIVATE dsidle)
target_compile_options(dsidle_config_tests PRIVATE -UNDEBUG)
add_test(NAME dsidle_config_tests COMMAND dsidle_config_tests)
set_tests_properties(dsidle_config_tests PROPERTIES WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR})

add_executable(dsidle_shared_pool_tests tests/shared_pool_tests.cc)
target_link_libraries(dsidle_shared_pool_tests PRIVATE dsidle)
target_compile_options(dsidle_shared_pool_tests PRIVATE -UNDEBUG)
add_test(NAME dsidle_shared_pool_tests COMMAND dsidle_shared_pool_tests)

add_executable(dsidle_shard_allocator_tests tests/shard_allocator_tests.cc)
target_link_libraries(dsidle_shard_allocator_tests PRIVATE dsidle)
target_compile_options(dsidle_shard_allocator_tests PRIVATE -UNDEBUG)
add_test(NAME dsidle_shard_allocator_tests COMMAND dsidle_shard_allocator_tests)

add_executable(dsidle_epoch_tests tests/epoch_tests.cc)
target_link_libraries(dsidle_epoch_tests PRIVATE dsidle)
target_compile_options(dsidle_epoch_tests PRIVATE -UNDEBUG)
add_test(NAME dsidle_epoch_tests COMMAND dsidle_epoch_tests)

add_executable(dsidle_node_control_tests tests/node_control_tests.cc)
target_link_libraries(dsidle_node_control_tests PRIVATE dsidle)
target_compile_options(dsidle_node_control_tests PRIVATE -UNDEBUG)
add_test(NAME dsidle_node_control_tests COMMAND dsidle_node_control_tests)

add_executable(dsidle_replica_directory_tests tests/replica_directory_tests.cc)
target_link_libraries(dsidle_replica_directory_tests PRIVATE dsidle)
target_compile_options(dsidle_replica_directory_tests PRIVATE -UNDEBUG)
add_test(NAME dsidle_replica_directory_tests COMMAND dsidle_replica_directory_tests)

add_executable(dsidle_latency_simulator_tests tests/latency_simulator_tests.cc)
target_link_libraries(dsidle_latency_simulator_tests PRIVATE dsidle)
target_compile_options(dsidle_latency_simulator_tests PRIVATE -UNDEBUG)
add_test(NAME dsidle_latency_simulator_tests COMMAND dsidle_latency_simulator_tests)

add_executable(dsidle_masstree_single_process_tests tests/masstree_single_process_tests.cc)
target_link_libraries(dsidle_masstree_single_process_tests PRIVATE dsidle_masstree)
target_compile_options(dsidle_masstree_single_process_tests PRIVATE -UNDEBUG -include ${CMAKE_CURRENT_SOURCE_DIR}/third_party/masstree-beta/config.h)
add_test(NAME dsidle_masstree_single_process_tests COMMAND dsidle_masstree_single_process_tests)
