cmake_minimum_required (VERSION 3.5)
project (Sidle-benchmark)

message(FATAL_ERROR
  "benchmark/ is archived upstream SIDLE reference source. Its retired "
  "src/kv and src/helper facade is intentionally isolated from D-SIDLE. "
  "Configure the repository root and use the dsidle runners instead.")

###############################################################################
# read_latest
###############################################################################
add_executable(read_latest_lat
    ${CMAKE_CURRENT_SOURCE_DIR}/../benchmark/synthetic/read_latest.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/../src/helper.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/../third_party/cxl_utils/cxl_allocator.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../third_party/masstree-beta/masstree.cc
    ${CMAKE_CURRENT_SOURCE_DIR}/../third_party/art/art_node.cc
    ${CMAKE_CURRENT_SOURCE_DIR}/../third_party/art/art.cc
    ${CMAKE_CURRENT_SOURCE_DIR}/../third_party/sidle_utils/sidle_worker.cc
    ${CMAKE_CURRENT_SOURCE_DIR}/../third_party/sidle_utils/sidle_frontend.cc
)
target_compile_definitions(read_latest_lat
    PRIVATE
        ${NDEBUG}
        ${NDEBUGGING}
        ${CORRECTNESS}
        TF_GRAPH_DIR="${CMAKE_CURRENT_SOURCE_DIR}/../asset"
        ORIGIN_YCSB
        READ_LATEST
        ANALYZE_LATENCY
)
target_link_libraries(read_latest_lat
    PRIVATE
        -lpthread
        LIBJSON
        memkind
)
target_link_directories(read_latest_lat PRIVATE 
    /usr/local/lib
)

###############################################################################
# read_latest_origin
###############################################################################
add_executable(read_latest_origin
    ${CMAKE_CURRENT_SOURCE_DIR}/../benchmark/synthetic/read_latest.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/../src/helper.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/../third_party/cxl_utils/cxl_allocator.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../third_party/masstree-beta/masstree.cc
    ${CMAKE_CURRENT_SOURCE_DIR}/../third_party/art/art.cc
    ${CMAKE_CURRENT_SOURCE_DIR}/../third_party/sidle_utils/sidle_worker.cc
    ${CMAKE_CURRENT_SOURCE_DIR}/../third_party/art/art_node.cc
    ${CMAKE_CURRENT_SOURCE_DIR}/../third_party/sidle_utils/sidle_frontend.cc
)
target_compile_definitions(read_latest_origin
    PRIVATE
        ${NDEBUG}
        ${NDEBUGGING}
        ${CORRECTNESS}
        ORIGIN_YCSB
        READ_LATEST
        TF_GRAPH_DIR="${CMAKE_CURRENT_SOURCE_DIR}/../asset"
)
target_link_libraries(read_latest_origin
    PRIVATE
        -lpthread
        LIBJSON
        memkind
)
target_link_directories(read_latest_origin PRIVATE /usr/local/lib)



###############################################################################
# short_range
###############################################################################
add_executable(short_range_lat
    ${CMAKE_CURRENT_SOURCE_DIR}/../benchmark/synthetic/short_range.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/../src/helper.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/../third_party/art/art.cc
    ${CMAKE_CURRENT_SOURCE_DIR}/../third_party/art/art_node.cc
    ${CMAKE_CURRENT_SOURCE_DIR}/../third_party/sidle_utils/sidle_worker.cc
    ${CMAKE_CURRENT_SOURCE_DIR}/../third_party/cxl_utils/cxl_allocator.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../third_party/masstree-beta/masstree.cc
    ${CMAKE_CURRENT_SOURCE_DIR}/../third_party/sidle_utils/sidle_frontend.cc
)
target_compile_definitions(short_range_lat
    PRIVATE
        ${NDEBUG}
        ${NDEBUGGING}
        ${CORRECTNESS}
        ORIGIN_YCSB
        SHORT_RANGE
        TF_GRAPH_DIR="${CMAKE_CURRENT_SOURCE_DIR}/../asset"
        ANALYZE_LATENCY
)
target_link_libraries(short_range_lat
    PRIVATE
        -lpthread
        LIBJSON
        memkind
)
target_link_directories(short_range_lat PRIVATE /usr/local/lib)

###############################################################################
# short_range_origin
###############################################################################
add_executable(short_range_origin
    ${CMAKE_CURRENT_SOURCE_DIR}/../benchmark/synthetic/short_range.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/../src/helper.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/../third_party/art/art.cc
    ${CMAKE_CURRENT_SOURCE_DIR}/../third_party/art/art_node.cc
    ${CMAKE_CURRENT_SOURCE_DIR}/../third_party/sidle_utils/sidle_worker.cc
    ${CMAKE_CURRENT_SOURCE_DIR}/../third_party/cxl_utils/cxl_allocator.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../third_party/masstree-beta/masstree.cc
    ${CMAKE_CURRENT_SOURCE_DIR}/../third_party/sidle_utils/sidle_frontend.cc
)
target_compile_definitions(short_range_origin
    PRIVATE
        ${NDEBUG}
        ${NDEBUGGING}
        ${CORRECTNESS}
        ORIGIN_YCSB
        SHORT_RANGE
        TF_GRAPH_DIR="${CMAKE_CURRENT_SOURCE_DIR}/../asset"
)
target_link_libraries(short_range_origin
    PRIVATE
        -lpthread
        LIBJSON
        memkind
)
target_link_directories(short_range_origin PRIVATE /usr/local/lib)

###############################################################################
# rw_ycsb
###############################################################################
add_executable(rw_ycsb_lat
    ${CMAKE_CURRENT_SOURCE_DIR}/../benchmark/synthetic/rw_ycsb.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/../src/helper.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/../third_party/art/art.cc
    ${CMAKE_CURRENT_SOURCE_DIR}/../third_party/art/art_node.cc
    ${CMAKE_CURRENT_SOURCE_DIR}/../third_party/sidle_utils/sidle_worker.cc
    ${CMAKE_CURRENT_SOURCE_DIR}/../third_party/cxl_utils/cxl_allocator.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../third_party/masstree-beta/masstree.cc
    ${CMAKE_CURRENT_SOURCE_DIR}/../third_party/sidle_utils/sidle_frontend.cc
)
target_compile_definitions(rw_ycsb_lat
    PRIVATE
        ${NDEBUG}
        ${NDEBUGGING}
        ${CORRECTNESS}
        TF_GRAPH_DIR="${CMAKE_CURRENT_SOURCE_DIR}/../asset"
        ORIGIN_YCSB
        ANALYZE_LATENCY
)
target_link_libraries(rw_ycsb_lat
    PRIVATE
        -lpthread
        LIBJSON
        memkind
)
target_link_directories(rw_ycsb_lat PRIVATE /usr/local/lib)

# enable gperf
# target_link_libraries(rw_ycsb PRIVATE -ltcmalloc -lprofiler)

###############################################################################
# rw_ycsb_origin
###############################################################################
add_executable(rw_ycsb_origin
    ${CMAKE_CURRENT_SOURCE_DIR}/../benchmark/synthetic/rw_ycsb.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/../src/helper.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/../third_party/art/art.cc
    ${CMAKE_CURRENT_SOURCE_DIR}/../third_party/art/art_node.cc
    ${CMAKE_CURRENT_SOURCE_DIR}/../third_party/sidle_utils/sidle_worker.cc
    ${CMAKE_CURRENT_SOURCE_DIR}/../third_party/cxl_utils/cxl_allocator.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../third_party/masstree-beta/masstree.cc
    ${CMAKE_CURRENT_SOURCE_DIR}/../third_party/sidle_utils/sidle_frontend.cc
)
target_compile_definitions(rw_ycsb_origin
    PRIVATE
        ${NDEBUG}
        ${NDEBUGGING}
        ${CORRECTNESS}
        ORIGIN_YCSB
        TF_GRAPH_DIR="${CMAKE_CURRENT_SOURCE_DIR}/../asset"
)
target_link_libraries(rw_ycsb_origin
    PRIVATE
        -lpthread
        LIBJSON
        memkind
)
target_link_directories(rw_ycsb_origin PRIVATE /usr/local/lib)

###############################################################################
# read_modify_write
###############################################################################
add_executable(read_modify_write_lat 
    ${CMAKE_CURRENT_SOURCE_DIR}/../benchmark/synthetic/read_modify_write.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/../src/helper.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/../third_party/art/art.cc
    ${CMAKE_CURRENT_SOURCE_DIR}/../third_party/art/art_node.cc
    ${CMAKE_CURRENT_SOURCE_DIR}/../third_party/sidle_utils/sidle_worker.cc
    ${CMAKE_CURRENT_SOURCE_DIR}/../third_party/cxl_utils/cxl_allocator.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../third_party/masstree-beta/masstree.cc
    ${CMAKE_CURRENT_SOURCE_DIR}/../third_party/sidle_utils/sidle_frontend.cc
)
target_compile_definitions(read_modify_write_lat
    PRIVATE
        ${NDEBUG}
        ${NDEBUGGING}
        ${CORRECTNESS}
        TF_GRAPH_DIR="${CMAKE_CURRENT_SOURCE_DIR}/../asset"
        ORIGIN_YCSB
        ANALYZE_LATENCY
)
target_link_libraries(read_modify_write_lat
    PRIVATE
        -lpthread
        LIBJSON
        memkind
)
target_link_directories(read_modify_write_lat PRIVATE /usr/local/lib)

###############################################################################
# read_modify_write_origin
###############################################################################
add_executable(read_modify_write_origin
    ${CMAKE_CURRENT_SOURCE_DIR}/../benchmark/synthetic/read_modify_write.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/../src/helper.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/../third_party/art/art.cc
    ${CMAKE_CURRENT_SOURCE_DIR}/../third_party/art/art_node.cc
    ${CMAKE_CURRENT_SOURCE_DIR}/../third_party/sidle_utils/sidle_worker.cc
    ${CMAKE_CURRENT_SOURCE_DIR}/../third_party/cxl_utils/cxl_allocator.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../third_party/masstree-beta/masstree.cc
    ${CMAKE_CURRENT_SOURCE_DIR}/../third_party/sidle_utils/sidle_frontend.cc
)
target_compile_definitions(read_modify_write_origin
    PRIVATE
        ${NDEBUG}
        ${NDEBUGGING}
        ${CORRECTNESS}
        ORIGIN_YCSB
        TF_GRAPH_DIR="${CMAKE_CURRENT_SOURCE_DIR}/../asset"
)
target_link_libraries(read_modify_write_origin
    PRIVATE
        -lpthread
        LIBJSON
        memkind
)
target_link_directories(read_modify_write_origin PRIVATE /usr/local/lib)


###############################################################################
# skewed_partition
###############################################################################
add_executable(skewed_partition
    ${CMAKE_CURRENT_SOURCE_DIR}/../benchmark/synthetic/skewed_partition.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/../src/helper.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/../third_party/art/art.cc
    ${CMAKE_CURRENT_SOURCE_DIR}/../third_party/art/art_node.cc
    ${CMAKE_CURRENT_SOURCE_DIR}/../third_party/sidle_utils/sidle_worker.cc
    ${CMAKE_CURRENT_SOURCE_DIR}/../third_party/cxl_utils/cxl_allocator.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../third_party/sidle_utils/sidle_frontend.cc
)
target_compile_definitions(skewed_partition
    PRIVATE
        ${NDEBUG}
        ${NDEBUGGING}
        ${CORRECTNESS}
        # ORDERED
        SKEW
        TF_GRAPH_DIR="${CMAKE_CURRENT_SOURCE_DIR}/../asset"
)
target_link_libraries(skewed_partition
    PRIVATE
        -lpthread
        LIBJSON
        memkind
)
target_link_directories(skewed_partition PRIVATE /usr/local/lib)


###############################################################################
# skewed_partition_watermark
###############################################################################
add_executable(skewed_partition_watermark
    ${CMAKE_CURRENT_SOURCE_DIR}/../benchmark/synthetic/skewed_partition.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/../src/helper.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/../third_party/art/art.cc
    ${CMAKE_CURRENT_SOURCE_DIR}/../third_party/art/art_node.cc
    ${CMAKE_CURRENT_SOURCE_DIR}/../third_party/sidle_utils/sidle_worker.cc
    ${CMAKE_CURRENT_SOURCE_DIR}/../third_party/cxl_utils/cxl_allocator.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../third_party/sidle_utils/sidle_frontend.cc
)
target_compile_definitions(skewed_partition_watermark
    PRIVATE
        ${NDEBUG}
        ${NDEBUGGING}
        ${CORRECTNESS}
        WATERMARK_RECORD
        SKEW
        TF_GRAPH_DIR="${CMAKE_CURRENT_SOURCE_DIR}/../asset"
)
target_link_libraries(skewed_partition_watermark
    PRIVATE
        -lpthread
        LIBJSON
        memkind
)
target_link_directories(skewed_partition_watermark PRIVATE /usr/local/lib)

###############################################################################
# skewed_parition_dynamic
###############################################################################
add_executable(skewed_partition_dynamic 
    ${CMAKE_CURRENT_SOURCE_DIR}/../benchmark/synthetic/skewed_partition_dynamic.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/../src/helper.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/../third_party/art/art.cc
    ${CMAKE_CURRENT_SOURCE_DIR}/../third_party/art/art_node.cc
    ${CMAKE_CURRENT_SOURCE_DIR}/../third_party/sidle_utils/sidle_worker.cc
    ${CMAKE_CURRENT_SOURCE_DIR}/../third_party/cxl_utils/cxl_allocator.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../third_party/sidle_utils/sidle_frontend.cc
)
target_compile_definitions(skewed_partition_dynamic 
    PRIVATE
        ${NDEBUG}
        ${NDEBUGGING}
        ${CORRECTNESS}
        # ORDERED
        SKEW_DYNAMIC
)
target_link_libraries(skewed_partition_dynamic 
    PRIVATE
        -lpthread
        LIBJSON
        memkind
)
target_link_directories(skewed_partition_dynamic PRIVATE /usr/local/lib)

###############################################################################
# skewed_parition_dynamic
###############################################################################
add_executable(skewed_partition_dynamic_watermark
    ${CMAKE_CURRENT_SOURCE_DIR}/../benchmark/synthetic/skewed_partition_dynamic.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/../src/helper.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/../third_party/art/art.cc
    ${CMAKE_CURRENT_SOURCE_DIR}/../third_party/art/art_node.cc
    ${CMAKE_CURRENT_SOURCE_DIR}/../third_party/sidle_utils/sidle_worker.cc
    ${CMAKE_CURRENT_SOURCE_DIR}/../third_party/cxl_utils/cxl_allocator.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../third_party/sidle_utils/sidle_frontend.cc
)
target_compile_definitions(skewed_partition_dynamic_watermark
    PRIVATE
        ${NDEBUG}
        ${NDEBUGGING}
        ${CORRECTNESS}
        WATERMARK_RECORD
        SKEW_DYNAMIC
)
target_link_libraries(skewed_partition_dynamic_watermark
    PRIVATE
        -lpthread
        LIBJSON
        memkind
)
target_link_directories(skewed_partition_dynamic_watermark PRIVATE /usr/local/lib)

###############################################################################
# dynamic_hot_range
###############################################################################
add_executable(dynamic_hot_range
    ${CMAKE_CURRENT_SOURCE_DIR}/../benchmark/synthetic/dynamic_hot_range.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/../src/helper.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/../third_party/art/art.cc
    ${CMAKE_CURRENT_SOURCE_DIR}/../third_party/art/art_node.cc
    ${CMAKE_CURRENT_SOURCE_DIR}/../third_party/sidle_utils/sidle_worker.cc
    ${CMAKE_CURRENT_SOURCE_DIR}/../third_party/cxl_utils/cxl_allocator.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../third_party/sidle_utils/sidle_frontend.cc
)
target_link_libraries(dynamic_hot_range 
    PRIVATE
        -lpthread
        LIBJSON
        memkind
)
target_link_directories(dynamic_hot_range PRIVATE /usr/local/lib)

###############################################################################
# dynamic_hot_range_dynamic
###############################################################################
add_executable(dynamic_hot_range_dynamic
    ${CMAKE_CURRENT_SOURCE_DIR}/../benchmark/synthetic/dynamic_hot_range_dynamic.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/../src/helper.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/../third_party/art/art.cc
    ${CMAKE_CURRENT_SOURCE_DIR}/../third_party/art/art_node.cc
    ${CMAKE_CURRENT_SOURCE_DIR}/../third_party/sidle_utils/sidle_worker.cc
    ${CMAKE_CURRENT_SOURCE_DIR}/../third_party/cxl_utils/cxl_allocator.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../third_party/sidle_utils/sidle_frontend.cc
)
target_link_libraries(dynamic_hot_range_dynamic 
    PRIVATE
        -lpthread
        LIBJSON
        memkind
)
target_link_directories(dynamic_hot_range_dynamic PRIVATE /usr/local/lib)

###############################################################################
# real world trace
###############################################################################
add_executable(real_world_trace
    ${CMAKE_CURRENT_SOURCE_DIR}/../benchmark/real_world/real_world_trace.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/../src/helper.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/../third_party/art/art.cc
    ${CMAKE_CURRENT_SOURCE_DIR}/../third_party/art/art_node.cc
    ${CMAKE_CURRENT_SOURCE_DIR}/../third_party/sidle_utils/sidle_worker.cc
    ${CMAKE_CURRENT_SOURCE_DIR}/../third_party/cxl_utils/cxl_allocator.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../third_party/sidle_utils/sidle_frontend.cc
)
target_compile_definitions(real_world_trace
    PRIVATE
        ${NDEBUG}
        ${NDEBUGGING}
        ${CORRECTNESS}
        USE_MT
        TF_GRAPH_DIR="${CMAKE_CURRENT_SOURCE_DIR}/../asset"
)
target_link_libraries(real_world_trace
    PRIVATE
        -lpthread
        LIBJSON
        memkind
)
target_link_directories(real_world_trace PRIVATE /usr/local/lib)

###############################################################################
# real world trace_lat
###############################################################################
add_executable(real_world_trace_lat
    ${CMAKE_CURRENT_SOURCE_DIR}/../benchmark/real_world/real_world_trace.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/../src/helper.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/../third_party/art/art.cc
    ${CMAKE_CURRENT_SOURCE_DIR}/../third_party/art/art_node.cc
    ${CMAKE_CURRENT_SOURCE_DIR}/../third_party/sidle_utils/sidle_worker.cc
    ${CMAKE_CURRENT_SOURCE_DIR}/../third_party/cxl_utils/cxl_allocator.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../third_party/sidle_utils/sidle_frontend.cc
)
target_compile_definitions(real_world_trace_lat
    PRIVATE
        ${NDEBUG}
        ${NDEBUGGING}
        ${CORRECTNESS}
        USE_MT
        ANALYZE_LATENCY
        TF_GRAPH_DIR="${CMAKE_CURRENT_SOURCE_DIR}/../asset"
)
target_link_libraries(real_world_trace_lat
    PRIVATE
        -lpthread
        LIBJSON
        memkind
)
target_link_directories(real_world_trace_lat PRIVATE /usr/local/lib)

###############################################################################
# rw_mix_str8
###############################################################################
add_executable(rw_mix_str8
    ${CMAKE_CURRENT_SOURCE_DIR}/../benchmark/synthetic/rw_mix.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/../src/helper.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/../third_party/art/art.cc
    ${CMAKE_CURRENT_SOURCE_DIR}/../third_party/art/art_node.cc
    ${CMAKE_CURRENT_SOURCE_DIR}/../third_party/sidle_utils/sidle_worker.cc
    ${CMAKE_CURRENT_SOURCE_DIR}/../third_party/cxl_utils/cxl_allocator.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../third_party/masstree-beta/masstree.cc
    ${CMAKE_CURRENT_SOURCE_DIR}/../third_party/sidle_utils/sidle_frontend.cc
)
target_compile_definitions(rw_mix_str8
    PRIVATE
        ${NDEBUG}
        ${NDEBUGGING}
        ${CORRECTNESS}
        STR_8
        STR_KEY
        TF_GRAPH_DIR="${CMAKE_CURRENT_SOURCE_DIR}/../asset"
)
target_link_libraries(rw_mix_str8
    PRIVATE
        -lpthread
        LIBJSON
        memkind
)
target_link_directories(rw_mix_str8 PRIVATE /usr/local/lib)

###############################################################################
# rw_mix_str16
###############################################################################
add_executable(rw_mix_str16
    ${CMAKE_CURRENT_SOURCE_DIR}/../benchmark/synthetic/rw_mix.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/../src/helper.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/../third_party/art/art.cc
    ${CMAKE_CURRENT_SOURCE_DIR}/../third_party/art/art_node.cc
    ${CMAKE_CURRENT_SOURCE_DIR}/../third_party/sidle_utils/sidle_worker.cc
    ${CMAKE_CURRENT_SOURCE_DIR}/../third_party/cxl_utils/cxl_allocator.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../third_party/masstree-beta/masstree.cc
    ${CMAKE_CURRENT_SOURCE_DIR}/../third_party/sidle_utils/sidle_frontend.cc
)
target_compile_definitions(rw_mix_str16
    PRIVATE
        ${NDEBUG}
        ${NDEBUGGING}
        ${CORRECTNESS}
        STR_16
        STR_KEY
        TF_GRAPH_DIR="${CMAKE_CURRENT_SOURCE_DIR}/../asset"
)
target_link_libraries(rw_mix_str16
    PRIVATE
        -lpthread
        LIBJSON
        memkind
)
target_link_directories(rw_mix_str16 PRIVATE /usr/local/lib)

###############################################################################
# rw_mix_str20
###############################################################################
add_executable(rw_mix_str20
    ${CMAKE_CURRENT_SOURCE_DIR}/../benchmark/synthetic/rw_mix.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/../src/helper.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/../third_party/art/art.cc
    ${CMAKE_CURRENT_SOURCE_DIR}/../third_party/art/art_node.cc
    ${CMAKE_CURRENT_SOURCE_DIR}/../third_party/sidle_utils/sidle_worker.cc
    ${CMAKE_CURRENT_SOURCE_DIR}/../third_party/cxl_utils/cxl_allocator.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../third_party/masstree-beta/masstree.cc
    ${CMAKE_CURRENT_SOURCE_DIR}/../third_party/sidle_utils/sidle_frontend.cc
)
target_compile_definitions(rw_mix_str20
    PRIVATE
        ${NDEBUG}
        ${NDEBUGGING}
        ${CORRECTNESS}
        STR_20
        STR_KEY
        TF_GRAPH_DIR="${CMAKE_CURRENT_SOURCE_DIR}/../asset"
)
target_link_libraries(rw_mix_str20
    PRIVATE
        -lpthread
        LIBJSON
        memkind
)
target_link_directories(rw_mix_str20 PRIVATE /usr/local/lib)

###############################################################################
# rw_mix_str24
###############################################################################
add_executable(rw_mix_str24
    ${CMAKE_CURRENT_SOURCE_DIR}/../benchmark/synthetic/rw_mix.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/../src/helper.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/../third_party/art/art.cc
    ${CMAKE_CURRENT_SOURCE_DIR}/../third_party/art/art_node.cc
    ${CMAKE_CURRENT_SOURCE_DIR}/../third_party/sidle_utils/sidle_worker.cc
    ${CMAKE_CURRENT_SOURCE_DIR}/../third_party/cxl_utils/cxl_allocator.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../third_party/masstree-beta/masstree.cc
    ${CMAKE_CURRENT_SOURCE_DIR}/../third_party/sidle_utils/sidle_frontend.cc
)
target_compile_definitions(rw_mix_str24
    PRIVATE
        ${NDEBUG}
        ${NDEBUGGING}
        ${CORRECTNESS}
        STR_24
        STR_KEY
        TF_GRAPH_DIR="${CMAKE_CURRENT_SOURCE_DIR}/../asset"
)
target_link_libraries(rw_mix_str24
    PRIVATE
        -lpthread
        LIBJSON
        memkind
)
target_link_directories(rw_mix_str24 PRIVATE /usr/local/lib)

###############################################################################
# rw_mix_str32
###############################################################################
add_executable(rw_mix_str32
    ${CMAKE_CURRENT_SOURCE_DIR}/../benchmark/synthetic/rw_mix.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/../src/helper.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/../third_party/art/art.cc
    ${CMAKE_CURRENT_SOURCE_DIR}/../third_party/art/art_node.cc
    ${CMAKE_CURRENT_SOURCE_DIR}/../third_party/sidle_utils/sidle_worker.cc
    ${CMAKE_CURRENT_SOURCE_DIR}/../third_party/cxl_utils/cxl_allocator.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../third_party/masstree-beta/masstree.cc
    ${CMAKE_CURRENT_SOURCE_DIR}/../third_party/sidle_utils/sidle_frontend.cc
)
target_compile_definitions(rw_mix_str32
    PRIVATE
        ${NDEBUG}
        ${NDEBUGGING}
        ${CORRECTNESS}
        STR_32
        STR_KEY
        TF_GRAPH_DIR="${CMAKE_CURRENT_SOURCE_DIR}/../asset"
)
target_link_libraries(rw_mix_str32
    PRIVATE
        -lpthread
        LIBJSON
        memkind
)
target_link_directories(rw_mix_str32 PRIVATE /usr/local/lib)

###############################################################################
# rw_mix_str40
###############################################################################
add_executable(rw_mix_str40
    ${CMAKE_CURRENT_SOURCE_DIR}/../benchmark/synthetic/rw_mix.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/../src/helper.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/../third_party/art/art.cc
    ${CMAKE_CURRENT_SOURCE_DIR}/../third_party/art/art_node.cc
    ${CMAKE_CURRENT_SOURCE_DIR}/../third_party/sidle_utils/sidle_worker.cc
    ${CMAKE_CURRENT_SOURCE_DIR}/../third_party/cxl_utils/cxl_allocator.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../third_party/masstree-beta/masstree.cc
    ${CMAKE_CURRENT_SOURCE_DIR}/../third_party/sidle_utils/sidle_frontend.cc
)
target_compile_definitions(rw_mix_str40
    PRIVATE
        ${NDEBUG}
        ${NDEBUGGING}
        ${CORRECTNESS}
        STR_40
        STR_KEY
        TF_GRAPH_DIR="${CMAKE_CURRENT_SOURCE_DIR}/../asset"
)
target_link_libraries(rw_mix_str40
    PRIVATE
        -lpthread
        LIBJSON
        memkind
)
target_link_directories(rw_mix_str40 PRIVATE /usr/local/lib)

###############################################################################
# rw_mix_str64
###############################################################################
add_executable(rw_mix_str64
    ${CMAKE_CURRENT_SOURCE_DIR}/../benchmark/synthetic/rw_mix.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/../src/helper.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/../third_party/art/art.cc
    ${CMAKE_CURRENT_SOURCE_DIR}/../third_party/art/art_node.cc
    ${CMAKE_CURRENT_SOURCE_DIR}/../third_party/sidle_utils/sidle_worker.cc
    ${CMAKE_CURRENT_SOURCE_DIR}/../third_party/cxl_utils/cxl_allocator.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../third_party/masstree-beta/masstree.cc
    ${CMAKE_CURRENT_SOURCE_DIR}/../third_party/sidle_utils/sidle_frontend.cc
)
target_compile_definitions(rw_mix_str64
    PRIVATE
        ${NDEBUG}
        ${NDEBUGGING}
        ${CORRECTNESS}
        STR_64
        STR_KEY
        TF_GRAPH_DIR="${CMAKE_CURRENT_SOURCE_DIR}/../asset"
)
target_link_libraries(rw_mix_str64
    PRIVATE
        -lpthread
        LIBJSON
        memkind
)
target_link_directories(rw_mix_str64 PRIVATE /usr/local/lib)

###############################################################################
# rw_mix_str128
###############################################################################
add_executable(rw_mix_str128
    ${CMAKE_CURRENT_SOURCE_DIR}/../benchmark/synthetic/rw_mix.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/../src/helper.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/../third_party/art/art.cc
    ${CMAKE_CURRENT_SOURCE_DIR}/../third_party/art/art_node.cc
    ${CMAKE_CURRENT_SOURCE_DIR}/../third_party/sidle_utils/sidle_worker.cc
    ${CMAKE_CURRENT_SOURCE_DIR}/../third_party/cxl_utils/cxl_allocator.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../third_party/masstree-beta/masstree.cc
    ${CMAKE_CURRENT_SOURCE_DIR}/../third_party/sidle_utils/sidle_frontend.cc
)
target_compile_definitions(rw_mix_str128
    PRIVATE
        ${NDEBUG}
        ${NDEBUGGING}
        ${CORRECTNESS}
        STR_128
        STR_KEY
)
target_link_libraries(rw_mix_str128
    PRIVATE
        -lpthread
        LIBJSON
        memkind
)
target_link_directories(rw_mix_str128 PRIVATE /usr/local/lib)

###############################################################################
# rw_mix_val8
###############################################################################
add_executable(rw_mix_val8
    ${CMAKE_CURRENT_SOURCE_DIR}/../benchmark/synthetic/rw_mix.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/../src/helper.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/../third_party/art/art.cc
    ${CMAKE_CURRENT_SOURCE_DIR}/../third_party/art/art_node.cc
    ${CMAKE_CURRENT_SOURCE_DIR}/../third_party/sidle_utils/sidle_worker.cc
    ${CMAKE_CURRENT_SOURCE_DIR}/../third_party/cxl_utils/cxl_allocator.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../third_party/masstree-beta/masstree.cc
    ${CMAKE_CURRENT_SOURCE_DIR}/../third_party/sidle_utils/sidle_frontend.cc
)
target_compile_definitions(rw_mix_val8
    PRIVATE
        ${NDEBUG}
        ${NDEBUGGING}
        ${CORRECTNESS}
        VAL_8
)
target_link_libraries(rw_mix_val8
    PRIVATE
        -lpthread
        LIBJSON
        memkind
)
target_link_directories(rw_mix_val8 PRIVATE /usr/local/lib)

###############################################################################
# rw_mix_val16
###############################################################################
add_executable(rw_mix_val16
    ${CMAKE_CURRENT_SOURCE_DIR}/../benchmark/synthetic/rw_mix.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/../src/helper.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/../third_party/art/art.cc
    ${CMAKE_CURRENT_SOURCE_DIR}/../third_party/art/art_node.cc
    ${CMAKE_CURRENT_SOURCE_DIR}/../third_party/sidle_utils/sidle_worker.cc
    ${CMAKE_CURRENT_SOURCE_DIR}/../third_party/cxl_utils/cxl_allocator.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../third_party/masstree-beta/masstree.cc
    ${CMAKE_CURRENT_SOURCE_DIR}/../third_party/sidle_utils/sidle_frontend.cc
)
target_compile_definitions(rw_mix_val16
    PRIVATE
        ${NDEBUG}
        ${NDEBUGGING}
        ${CORRECTNESS}
        VAL_16
)
target_link_libraries(rw_mix_val16
    PRIVATE
        -lpthread
        LIBJSON
        memkind
)
target_link_directories(rw_mix_val16 PRIVATE /usr/local/lib)

###############################################################################
# rw_mix_val24
###############################################################################
add_executable(rw_mix_val24
    ${CMAKE_CURRENT_SOURCE_DIR}/../benchmark/synthetic/rw_mix.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/../src/helper.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/../third_party/art/art.cc
    ${CMAKE_CURRENT_SOURCE_DIR}/../third_party/art/art_node.cc
    ${CMAKE_CURRENT_SOURCE_DIR}/../third_party/sidle_utils/sidle_worker.cc
    ${CMAKE_CURRENT_SOURCE_DIR}/../third_party/cxl_utils/cxl_allocator.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../third_party/masstree-beta/masstree.cc
    ${CMAKE_CURRENT_SOURCE_DIR}/../third_party/sidle_utils/sidle_frontend.cc
)
target_compile_definitions(rw_mix_val24
    PRIVATE
        ${NDEBUG}
        ${NDEBUGGING}
        ${CORRECTNESS}
        VAL_24
)
target_link_libraries(rw_mix_val24
    PRIVATE
        -lpthread
        LIBJSON
        memkind
)
target_link_directories(rw_mix_val24 PRIVATE /usr/local/lib)

###############################################################################
# rw_mix_val32
###############################################################################
add_executable(rw_mix_val32
    ${CMAKE_CURRENT_SOURCE_DIR}/../benchmark/synthetic/rw_mix.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/../src/helper.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/../third_party/art/art.cc
    ${CMAKE_CURRENT_SOURCE_DIR}/../third_party/art/art_node.cc
    ${CMAKE_CURRENT_SOURCE_DIR}/../third_party/sidle_utils/sidle_worker.cc
    ${CMAKE_CURRENT_SOURCE_DIR}/../third_party/cxl_utils/cxl_allocator.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../third_party/masstree-beta/masstree.cc
    ${CMAKE_CURRENT_SOURCE_DIR}/../third_party/sidle_utils/sidle_frontend.cc
)
target_compile_definitions(rw_mix_val32
    PRIVATE
        ${NDEBUG}
        ${NDEBUGGING}
        ${CORRECTNESS}
        VAL_32
)
target_link_libraries(rw_mix_val32
    PRIVATE
        -lpthread
        LIBJSON
        memkind
)
target_link_directories(rw_mix_val32 PRIVATE /usr/local/lib)

###############################################################################
# rw_mix_val40
###############################################################################
add_executable(rw_mix_val40
    ${CMAKE_CURRENT_SOURCE_DIR}/../benchmark/synthetic/rw_mix.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/../src/helper.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/../third_party/art/art.cc
    ${CMAKE_CURRENT_SOURCE_DIR}/../third_party/art/art_node.cc
    ${CMAKE_CURRENT_SOURCE_DIR}/../third_party/sidle_utils/sidle_worker.cc
    ${CMAKE_CURRENT_SOURCE_DIR}/../third_party/cxl_utils/cxl_allocator.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../third_party/masstree-beta/masstree.cc
    ${CMAKE_CURRENT_SOURCE_DIR}/../third_party/sidle_utils/sidle_frontend.cc
)
target_compile_definitions(rw_mix_val40
    PRIVATE
        ${NDEBUG}
        ${NDEBUGGING}
        ${CORRECTNESS}
        VAL_40
)
target_link_libraries(rw_mix_val40
    PRIVATE
        -lpthread
        LIBJSON
        memkind
)
target_link_directories(rw_mix_val40 PRIVATE /usr/local/lib)

###############################################################################
# rw_mix_val64
###############################################################################
add_executable(rw_mix_val64
    ${CMAKE_CURRENT_SOURCE_DIR}/../benchmark/synthetic/rw_mix.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/../src/helper.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/../third_party/art/art.cc
    ${CMAKE_CURRENT_SOURCE_DIR}/../third_party/art/art_node.cc
    ${CMAKE_CURRENT_SOURCE_DIR}/../third_party/sidle_utils/sidle_worker.cc
    ${CMAKE_CURRENT_SOURCE_DIR}/../third_party/cxl_utils/cxl_allocator.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../third_party/masstree-beta/masstree.cc
    ${CMAKE_CURRENT_SOURCE_DIR}/../third_party/sidle_utils/sidle_frontend.cc
)
target_compile_definitions(rw_mix_val64
    PRIVATE
        ${NDEBUG}
        ${NDEBUGGING}
        ${CORRECTNESS}
        VAL_64
)
target_link_libraries(rw_mix_val64
    PRIVATE
        -lpthread
        LIBJSON
        memkind
)
target_link_directories(rw_mix_val64 PRIVATE /usr/local/lib)

###############################################################################
# rw_mix_val128
###############################################################################
add_executable(rw_mix_val128
    ${CMAKE_CURRENT_SOURCE_DIR}/../benchmark/synthetic/rw_mix.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/../src/helper.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/../third_party/art/art.cc
    ${CMAKE_CURRENT_SOURCE_DIR}/../third_party/art/art_node.cc
    ${CMAKE_CURRENT_SOURCE_DIR}/../third_party/sidle_utils/sidle_worker.cc
    ${CMAKE_CURRENT_SOURCE_DIR}/../third_party/cxl_utils/cxl_allocator.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../third_party/masstree-beta/masstree.cc
    ${CMAKE_CURRENT_SOURCE_DIR}/../third_party/sidle_utils/sidle_frontend.cc
)
target_compile_definitions(rw_mix_val128
    PRIVATE
        ${NDEBUG}
        ${NDEBUGGING}
        ${CORRECTNESS}
        VAL_128
)
target_link_libraries(rw_mix_val128
    PRIVATE
        -lpthread
        LIBJSON
        memkind
)
target_link_directories(rw_mix_val128 PRIVATE /usr/local/lib)

##############################################################################
# rw_mix_val256
##############################################################################
add_executable(rw_mix_val256
    ${CMAKE_CURRENT_SOURCE_DIR}/../benchmark/synthetic/rw_mix.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/../src/helper.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/../third_party/art/art.cc
    ${CMAKE_CURRENT_SOURCE_DIR}/../third_party/art/art_node.cc
    ${CMAKE_CURRENT_SOURCE_DIR}/../third_party/sidle_utils/sidle_worker.cc
    ${CMAKE_CURRENT_SOURCE_DIR}/../third_party/cxl_utils/cxl_allocator.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../third_party/masstree-beta/masstree.cc
    ${CMAKE_CURRENT_SOURCE_DIR}/../third_party/sidle_utils/sidle_frontend.cc
)
target_compile_definitions(rw_mix_val256
    PRIVATE
        ${NDEBUG}
        ${NDEBUGGING}
        ${CORRECTNESS}
        VAL_256
)
target_link_libraries(rw_mix_val256
    PRIVATE
        -lpthread
        LIBJSON
        memkind
)
target_link_directories(rw_mix_val256 PRIVATE /usr/local/lib)

##############################################################################
# rw_mix_val512
##############################################################################
add_executable(rw_mix_val512
    ${CMAKE_CURRENT_SOURCE_DIR}/../benchmark/synthetic/rw_mix.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/../src/helper.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/../third_party/art/art.cc
    ${CMAKE_CURRENT_SOURCE_DIR}/../third_party/art/art_node.cc
    ${CMAKE_CURRENT_SOURCE_DIR}/../third_party/sidle_utils/sidle_worker.cc
    ${CMAKE_CURRENT_SOURCE_DIR}/../third_party/cxl_utils/cxl_allocator.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../third_party/masstree-beta/masstree.cc
    ${CMAKE_CURRENT_SOURCE_DIR}/../third_party/sidle_utils/sidle_frontend.cc
)
target_compile_definitions(rw_mix_val512
    PRIVATE
        ${NDEBUG}
        ${NDEBUGGING}
        ${CORRECTNESS}
        VAL_512
)
target_link_libraries(rw_mix_val512
    PRIVATE
        -lpthread
        LIBJSON
        memkind
)
target_link_directories(rw_mix_val512 PRIVATE /usr/local/lib)


##############################################################################
# rw_mix_val1024
##############################################################################
add_executable(rw_mix_val1024
    ${CMAKE_CURRENT_SOURCE_DIR}/../benchmark/synthetic/rw_mix.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/../src/helper.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/../third_party/art/art.cc
    ${CMAKE_CURRENT_SOURCE_DIR}/../third_party/art/art_node.cc
    ${CMAKE_CURRENT_SOURCE_DIR}/../third_party/sidle_utils/sidle_worker.cc
    ${CMAKE_CURRENT_SOURCE_DIR}/../third_party/cxl_utils/cxl_allocator.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../third_party/masstree-beta/masstree.cc
    ${CMAKE_CURRENT_SOURCE_DIR}/../third_party/sidle_utils/sidle_frontend.cc
)
target_compile_definitions(rw_mix_val1024
    PRIVATE
        ${NDEBUG}
        ${NDEBUGGING}
        ${CORRECTNESS}
        VAL_1024
)
target_link_libraries(rw_mix_val1024
    PRIVATE
        -lpthread
        LIBJSON
        memkind
)
target_link_directories(rw_mix_val1024 PRIVATE /usr/local/lib)


##############################################################################
# rw_mix_val2048
##############################################################################
add_executable(rw_mix_val2048
    ${CMAKE_CURRENT_SOURCE_DIR}/../benchmark/synthetic/rw_mix.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/../src/helper.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/../third_party/art/art.cc
    ${CMAKE_CURRENT_SOURCE_DIR}/../third_party/art/art_node.cc
    ${CMAKE_CURRENT_SOURCE_DIR}/../third_party/sidle_utils/sidle_worker.cc
    ${CMAKE_CURRENT_SOURCE_DIR}/../third_party/cxl_utils/cxl_allocator.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../third_party/masstree-beta/masstree.cc
    ${CMAKE_CURRENT_SOURCE_DIR}/../third_party/sidle_utils/sidle_frontend.cc
)
target_compile_definitions(rw_mix_val2048
    PRIVATE
        ${NDEBUG}
        ${NDEBUGGING}
        ${CORRECTNESS}
        VAL_2048
)
target_link_libraries(rw_mix_val2048
    PRIVATE
        -lpthread
        LIBJSON
        memkind
)
target_link_directories(rw_mix_val2048 PRIVATE /usr/local/lib)


###############################################################################
# tpc-c
###############################################################################
add_executable(tpcc
    ${CMAKE_CURRENT_SOURCE_DIR}/../benchmark/real_world/tpcc.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/../src/helper.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/../third_party/cxl_utils/cxl_allocator.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../third_party/art/art_node.cc
    ${CMAKE_CURRENT_SOURCE_DIR}/../third_party/art/art.cc
    ${CMAKE_CURRENT_SOURCE_DIR}/../third_party/sidle_utils/sidle_worker.cc
    ${CMAKE_CURRENT_SOURCE_DIR}/../third_party/masstree-beta/masstree.cc
    ${CMAKE_CURRENT_SOURCE_DIR}/../third_party/sidle_utils/sidle_frontend.cc
)
target_compile_definitions(tpcc
    PRIVATE
        ${NDEBUG}
        ${NDEBUGGING}
        ${CORRECTNESS}
        USE_MT
        TF_GRAPH_DIR="${CMAKE_CURRENT_SOURCE_DIR}/../asset"
)
target_link_libraries(tpcc
    PRIVATE
        -lpthread
        LIBJSON
        memkind
)
target_link_directories(tpcc PRIVATE /usr/local/lib)

###############################################################################
# art long key trace
###############################################################################
add_executable(art_long_key
    ${CMAKE_CURRENT_SOURCE_DIR}/../benchmark/real_world/art_long_key.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/../src/helper.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/../third_party/art/art.cc
    ${CMAKE_CURRENT_SOURCE_DIR}/../third_party/art/art_node.cc
    ${CMAKE_CURRENT_SOURCE_DIR}/../third_party/sidle_utils/sidle_worker.cc
    ${CMAKE_CURRENT_SOURCE_DIR}/../third_party/cxl_utils/cxl_allocator.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../third_party/masstree-beta/masstree.cc
    ${CMAKE_CURRENT_SOURCE_DIR}/../third_party/sidle_utils/sidle_frontend.cc
)
target_compile_definitions(art_long_key
    PRIVATE
        ${NDEBUG}
        ${NDEBUGGING}
        ${CORRECTNESS}
        USE_MT
        REAL_WORLD
        LONG_KEY
        TF_GRAPH_DIR="${CMAKE_CURRENT_SOURCE_DIR}/../asset"
)
target_link_libraries(art_long_key
    PRIVATE
        -lpthread
        LIBJSON
        memkind
)
target_link_directories(art_long_key PRIVATE /usr/local/lib)

###############################################################################
# art long key trace with latency
###############################################################################
add_executable(art_long_key_lat
    ${CMAKE_CURRENT_SOURCE_DIR}/../benchmark/real_world/art_long_key.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/../src/helper.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/../third_party/art/art.cc
    ${CMAKE_CURRENT_SOURCE_DIR}/../third_party/art/art_node.cc
    ${CMAKE_CURRENT_SOURCE_DIR}/../third_party/sidle_utils/sidle_worker.cc
    ${CMAKE_CURRENT_SOURCE_DIR}/../third_party/cxl_utils/cxl_allocator.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../third_party/masstree-beta/masstree.cc
    ${CMAKE_CURRENT_SOURCE_DIR}/../third_party/sidle_utils/sidle_frontend.cc
)
target_compile_definitions(art_long_key_lat
    PRIVATE
        ${NDEBUG}
        ${NDEBUGGING}
        ${CORRECTNESS}
        USE_MT
        ANALYZE_LATENCY
        REAL_WORLD
        LONG_KEY
        TF_GRAPH_DIR="${CMAKE_CURRENT_SOURCE_DIR}/../asset"
)
target_link_libraries(art_long_key_lat
    PRIVATE
        -lpthread
        LIBJSON
        memkind
)
target_link_directories(art_long_key_lat PRIVATE /usr/local/lib)
