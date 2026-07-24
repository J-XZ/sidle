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
target_compile_options(dsidle PRIVATE -Wall -Wextra -Wpedantic)
target_link_libraries(dsidle PUBLIC atomic)

# dsidle: Build Masstree from this tree only. The legacy CXL allocator source
# is deliberately excluded; allocation conversion proceeds in the original
# Masstree files rather than through a compatibility backend.
add_library(dsidle_masstree STATIC
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
add_test(NAME dsidle_e2e_trace_runner_smoke
  COMMAND /bin/sh -c
    "truncate -s 128M /tmp/dsidle-e2e-runner.pool && \"$1\" --init-pool --config \"$2\" --node-control-capacity 1024 --max-threads-per-vm 3 && \"$3\" --config \"$2\" --phase smoke --node 0 --bootstrap"
    sh $<TARGET_FILE:dsidle_shared_pool> ${CMAKE_CURRENT_SOURCE_DIR}/tests/data/e2e_runner_config.jsonc $<TARGET_FILE:dsidle_e2e_trace_runner>)
set_tests_properties(dsidle_e2e_trace_runner_smoke PROPERTIES WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR})
add_test(NAME dsidle_e2e_trace_runner_min_duration
  COMMAND /bin/sh -c
    "truncate -s 128M /tmp/dsidle-e2e-runner-duration.pool && \"$1\" --init-pool --config \"$2\" --node-control-capacity 1024 --max-threads-per-vm 3 && \"$3\" --config \"$2\" --phase duration --node 0 --bootstrap --min-duration-sec 1 > /tmp/dsidle-e2e-duration.log && python3 -c 'import re,sys; x=re.search(r\"duration_us=(\\d+)\", open(sys.argv[1]).read()); sys.exit(not x or int(x.group(1)) < 1000000)' /tmp/dsidle-e2e-duration.log"
    sh $<TARGET_FILE:dsidle_shared_pool> ${CMAKE_CURRENT_SOURCE_DIR}/tests/data/e2e_runner_config.jsonc $<TARGET_FILE:dsidle_e2e_trace_runner>)
set_tests_properties(dsidle_e2e_trace_runner_min_duration PROPERTIES WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR})
add_test(NAME dsidle_e2e_trace_runner_replica_activation
  COMMAND /bin/sh -c
    "truncate -s 128M /tmp/dsidle-e2e-runner.pool && \"$1\" --init-pool --config \"$2\" --node-control-capacity 1024 --max-threads-per-vm 3 && \"$3\" --config \"$2\" --phase seed --node 0 --bootstrap && \"$3\" --config \"$4\" --phase replica-read --node 0 --min-duration-sec 1 > /tmp/dsidle-e2e-replica.log && python3 -c 'import re,sys; x=re.search(r\"replica_bytes=(\\d+)\", open(sys.argv[1]).read()); sys.exit(not x or int(x.group(1)) <= 8)' /tmp/dsidle-e2e-replica.log"
    sh $<TARGET_FILE:dsidle_shared_pool> ${CMAKE_CURRENT_SOURCE_DIR}/tests/data/e2e_runner_replica_seed_config.jsonc $<TARGET_FILE:dsidle_e2e_trace_runner> ${CMAKE_CURRENT_SOURCE_DIR}/tests/data/e2e_runner_replica_read_config.jsonc)
set_tests_properties(dsidle_e2e_trace_runner_replica_activation PROPERTIES WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR})
add_test(NAME dsidle_e2e_trace_runner_2vm_smoke
  COMMAND /bin/sh -c
    "truncate -s 128M /tmp/dsidle-e2e-runner-2vm.pool && \"$1\" --init-pool --config \"$2\" --node-control-capacity 1024 --max-threads-per-vm 3 && { \"$3\" --config \"$2\" --phase smoke --node 0 --bootstrap > /tmp/dsidle-e2e-node0.log 2>&1 & p=$!; \"$3\" --config \"$2\" --phase smoke --node 1 > /tmp/dsidle-e2e-node1.log 2>&1 & q=$!; wait $p && wait $q; } && grep -q 'E2E_TRACE_TIME_US phase=smoke node=0' /tmp/dsidle-e2e-node0.log && grep -q 'E2E_TRACE_TIME_US phase=smoke node=1' /tmp/dsidle-e2e-node1.log"
    sh $<TARGET_FILE:dsidle_shared_pool> ${CMAKE_CURRENT_SOURCE_DIR}/tests/data/e2e_runner_2vm_config.jsonc $<TARGET_FILE:dsidle_e2e_trace_runner>)
set_tests_properties(dsidle_e2e_trace_runner_2vm_smoke PROPERTIES WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR})
add_test(NAME dsidle_ycsb_summarizer_tests
  COMMAND /bin/sh -c
    "out=$(mktemp -d /tmp/dsidle-ycsb-summary.XXXXXX); python3 \"$1\" --log-dir \"$2\" --out-dir \"$out\" && test -s \"$out/ycsb_summary.json\" && test -s \"$out/ycsb_summary.csv\" && test -s \"$out/YCSB实验报告.md\" && grep -q 'avg_ops_sum' \"$out/ycsb_summary.csv\" && grep -q 'ops_per_sec_p90' \"$out/ycsb_summary.csv\""
    sh ${CMAKE_CURRENT_SOURCE_DIR}/scripts/summarize_ycsb_experiment.py ${CMAKE_CURRENT_SOURCE_DIR}/tests/data/ycsb_logs)
set_tests_properties(dsidle_ycsb_summarizer_tests PROPERTIES WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR})
add_test(NAME dsidle_ycsb_prepare_only_tests
  COMMAND /bin/sh -c
    "out=$(mktemp -d /tmp/dsidle-ycsb-prepare.XXXXXX); \"$1\" --prepare-only --skip-trace-gen --out-dir \"$out/result\" --workloads a,e --record-count 10 --operation-count 20 --threads-per-node 2 && test -s \"$out/result/run_meta.json\" && python3 -c 'import json,sys; sys.exit(json.load(open(sys.argv[1]))[\"warmup_rounds\"] != 1)' \"$out/result/run_meta.json\" && ! \"$1\" --prepare-only --out-dir \"$out/invalid\" --workloads a, >/dev/null 2>&1 && test ! -e \"$out/invalid\""
    sh ${CMAKE_CURRENT_SOURCE_DIR}/dsidle_run_ycsb_experiment.sh)
set_tests_properties(dsidle_ycsb_prepare_only_tests PROPERTIES WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR})
add_test(NAME dsidle_ycsb_trace_generation_tests
  COMMAND /bin/sh -c
    "out=$(mktemp -d /tmp/dsidle-ycsb-traces.XXXXXX); \"$1\" --prepare-only --out-dir \"$out/result\" --workloads a,e --record-count 10 --operation-count 10 --threads-per-node 1 --shared-size-mb 2048 --no-latency && test -s \"$out/result/traces/load/worker0.txt\" && test -s \"$out/result/traces/workloada/worker0.txt\" && test -s \"$out/result/traces/workloade/worker0.txt\" && test -s \"$out/result/configs/experiment_config_ycsb_workloada.jsonc\" && grep -q \"$out/result/traces/workloada\" \"$out/result/configs/experiment_config_ycsb_workloada.jsonc\" && grep -R -q '^GET ' \"$out/result/traces/workloada\" && grep -R -q '^PUT ' \"$out/result/traces/workloada\""
    sh ${CMAKE_CURRENT_SOURCE_DIR}/dsidle_run_ycsb_experiment.sh)
set_tests_properties(dsidle_ycsb_trace_generation_tests PROPERTIES WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR})
add_test(NAME dsidle_e2e_rounds_script_smoke
  COMMAND /bin/sh -c
    "out=$(mktemp -d /tmp/dsidle-e2e-rounds.XXXXXX); \"$1\" --execute --smoke --rounds 1 --config \"$2\" --phase smoke --out-dir \"$out\" --runner \"$3\" --pool-tool \"$4\" --node-control-capacity 1024 --max-threads-per-vm 3 && test -s \"$out/round_logs/smoke_round_1.meta\" && grep -q 'E2E_TRACE_TIME_US phase=smoke node=1' \"$out/logs/smoke_round_1_run_node1.log\""
    sh ${CMAKE_CURRENT_SOURCE_DIR}/scripts/run_dsidle_e2e_rounds.sh ${CMAKE_CURRENT_SOURCE_DIR}/tests/data/e2e_runner_2vm_config.jsonc $<TARGET_FILE:dsidle_e2e_trace_runner> $<TARGET_FILE:dsidle_shared_pool>)
set_tests_properties(dsidle_e2e_rounds_script_smoke PROPERTIES WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR})
add_test(NAME dsidle_e2e_rounds_script_4vm_smoke
  COMMAND /bin/sh -c
    "out=$(mktemp -d /tmp/dsidle-e2e-rounds-4vm.XXXXXX); \"$1\" --execute --smoke --rounds 1 --config \"$2\" --phase smoke --out-dir \"$out\" --runner \"$3\" --pool-tool \"$4\" --node-control-capacity 1024 --max-threads-per-vm 3 && grep -q 'E2E_TRACE_TIME_US phase=smoke node=3' \"$out/logs/smoke_round_1_run_node3.log\""
    sh ${CMAKE_CURRENT_SOURCE_DIR}/scripts/run_dsidle_e2e_rounds.sh ${CMAKE_CURRENT_SOURCE_DIR}/tests/data/e2e_runner_4vm_config.jsonc $<TARGET_FILE:dsidle_e2e_trace_runner> $<TARGET_FILE:dsidle_shared_pool>)
set_tests_properties(dsidle_e2e_rounds_script_4vm_smoke PROPERTIES WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR})
add_test(NAME dsidle_e2e_concurrency_plan_test
  COMMAND /bin/sh -c
    "\"$1\" --smoke --suite concurrency --rounds 1 --config \"$2\" --runner \"$3\" --pool-tool \"$4\" | grep -q -- '--min-duration-sec 60'"
    sh ${CMAKE_CURRENT_SOURCE_DIR}/scripts/run_dsidle_e2e_rounds.sh ${CMAKE_CURRENT_SOURCE_DIR}/tests/data/e2e_runner_4vm_config.jsonc $<TARGET_FILE:dsidle_e2e_trace_runner> $<TARGET_FILE:dsidle_shared_pool>)
set_tests_properties(dsidle_e2e_concurrency_plan_test PROPERTIES WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR})
add_test(NAME dsidle_e2e_ycsb_suite_smoke
  COMMAND /bin/sh -c
    "out=$(mktemp -d /tmp/dsidle-e2e-ycsb-suite.XXXXXX); \"$1\" --execute --smoke --suite ycsb --rounds 1 --load-config \"$2\" --run-config \"$2\" --out-dir \"$out\" --runner \"$3\" --pool-tool \"$4\" --node-control-capacity 1024 --max-threads-per-vm 3 && grep -q 'E2E_TRACE_TIME_US phase=load node=0' \"$out/logs/load_round_1_load_node0.log\" && grep -q 'E2E_TRACE_TIME_US phase=workloada node=1' \"$out/logs/workloada_round_1_run_node1.log\""
    sh ${CMAKE_CURRENT_SOURCE_DIR}/scripts/run_dsidle_e2e_rounds.sh ${CMAKE_CURRENT_SOURCE_DIR}/tests/data/e2e_runner_2vm_config.jsonc $<TARGET_FILE:dsidle_e2e_trace_runner> $<TARGET_FILE:dsidle_shared_pool>)
set_tests_properties(dsidle_e2e_ycsb_suite_smoke PROPERTIES WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR})
add_test(NAME dsidle_e2e08_suite_smoke
  COMMAND /bin/sh -c
    "out=$(mktemp -d /tmp/dsidle-e2e08-suite.XXXXXX); \"$1\" --execute --smoke --suite 08 --rounds 1 --config \"$2\" --out-dir \"$out\" --runner \"$3\" --pool-tool \"$4\" --node-control-capacity 65536 --max-threads-per-vm 2 && grep -q 'DSIDLE_E2E_SUITE_VERIFY suite=08 phase=e2e08_read node=3 status=ok' \"$out/logs/e2e08_read_round_1_read_node3.log\""
    sh ${CMAKE_CURRENT_SOURCE_DIR}/scripts/run_dsidle_e2e_rounds.sh ${CMAKE_CURRENT_SOURCE_DIR}/tests/data/e2e_runner_4vm_config.jsonc $<TARGET_FILE:dsidle_e2e_suite_runner> $<TARGET_FILE:dsidle_shared_pool>)
set_tests_properties(dsidle_e2e08_suite_smoke PROPERTIES WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR})
add_test(NAME dsidle_e2e09_suite_smoke
  COMMAND /bin/sh -c
    "out=$(mktemp -d /tmp/dsidle-e2e09-suite.XXXXXX); \"$1\" --execute --smoke --suite 09 --rounds 1 --config \"$2\" --out-dir \"$out\" --runner \"$3\" --pool-tool \"$4\" --node-control-capacity 65536 --max-threads-per-vm 2 && grep -q 'DSIDLE_E2E_SUITE_VERIFY suite=09 phase=e2e09_read node=3 status=ok' \"$out/logs/e2e09_read_round_1_read_node3.log\""
    sh ${CMAKE_CURRENT_SOURCE_DIR}/scripts/run_dsidle_e2e_rounds.sh ${CMAKE_CURRENT_SOURCE_DIR}/tests/data/e2e09_4vm_config.jsonc $<TARGET_FILE:dsidle_e2e_suite_runner> $<TARGET_FILE:dsidle_shared_pool>)
set_tests_properties(dsidle_e2e09_suite_smoke PROPERTIES WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR})
add_test(NAME dsidle_vm_scripts_dry_run
  COMMAND /bin/sh -c
    "\"$1\" --dry-run --config \"$2\" && \"$3\" --dry-run --config \"$2\" && \"$4\" --dry-run"
    sh ${CMAKE_CURRENT_SOURCE_DIR}/dsidle_init_vms.sh ${CMAKE_CURRENT_SOURCE_DIR}/experiment_config.jsonc ${CMAKE_CURRENT_SOURCE_DIR}/dsidle_check_vms.sh ${CMAKE_CURRENT_SOURCE_DIR}/dsidle_make_vm_img.sh)
set_tests_properties(dsidle_vm_scripts_dry_run PROPERTIES WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR})

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
