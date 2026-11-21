cmake_minimum_required(VERSION 3.20)
file(STRINGS "${CMAKE_CURRENT_SOURCE_DIR}/VERSION" FILESYSTEM_VERSION LIMIT_COUNT 1)
project(FilesystemNTFS VERSION ${FILESYSTEM_VERSION} LANGUAGES C CXX)
include(CTest)
include(FetchContent)
find_package(OpenSSL REQUIRED)
find_package(SQLite3 REQUIRED)
find_package(Threads REQUIRED)
find_path(NTFS3G_INCLUDE_DIR NAMES ntfs-3g/device.h REQUIRED)
find_library(NTFS3G_LIBRARY NAMES ntfs-3g REQUIRED)
set(CMAKE_C_STANDARD 11)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)
set(INFILTRATR_COMMON_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(INFILTRATR_COMMON_BUILD_SHARED OFF CACHE BOOL "" FORCE)
FetchContent_Declare(infiltratr_common
 GIT_REPOSITORY https://github.com/Infiltrator-Projects/Libraries.git
 GIT_TAG 489388567c436f70754db938ebdc4a07c10b7da0)
FetchContent_MakeAvailable(infiltratr_common)
# Common is a link-time dependency; its development files must not be installed
# by this standalone filesystem package.
set_property(DIRECTORY "${infiltratr_common_SOURCE_DIR}" PROPERTY EXCLUDE_FROM_ALL TRUE)
file(WRITE "${CMAKE_CURRENT_BINARY_DIR}/version.h" "#define LD_VERSION \"${PROJECT_VERSION}\"\n")
add_library(ntfs-formatter OBJECT formatter/mkntfs.c formatter/format_device.c
 formatter/boot.c formatter/attrdef.c formatter/sd.c)
target_include_directories(ntfs-formatter SYSTEM PRIVATE "${NTFS3G_INCLUDE_DIR}/ntfs-3g")
target_compile_definitions(ntfs-formatter PRIVATE
 _GNU_SOURCE _FILE_OFFSET_BITS=64 HAVE_STDDEF_H HAVE_SYS_TYPES_H HAVE_STDARG_H
 HAVE_TIME_H HAVE_SYS_STAT_H HAVE_STDIO_H HAVE_FFS)
target_compile_options(ntfs-formatter PRIVATE -Wall -Wextra -Werror -fvisibility=hidden)
# The imported on-disk structures use packed little-endian fields on x86-64.
set_source_files_properties(formatter/mkntfs.c formatter/sd.c PROPERTIES
 COMPILE_OPTIONS "-Wno-address-of-packed-member")
target_link_libraries(ntfs-formatter PRIVATE InfiltratrCommon::Common)
# The installed module contains the source-owned NTFS parser, Common V2
# and a hosted read-only namespace. Retired V1 workers are test-only.
set(NTFS_FIRMWARE_ELF "${CMAKE_CURRENT_BINARY_DIR}/firmware/NTFS.so")
add_custom_command(OUTPUT "${NTFS_FIRMWARE_ELF}"
 COMMAND "${CMAKE_COMMAND}" -S "${CMAKE_CURRENT_SOURCE_DIR}/module/firmware"
   -B "${CMAKE_CURRENT_BINARY_DIR}/firmware" -DCMAKE_BUILD_TYPE=Release
   "-DCMAKE_C_COMPILER=${CMAKE_C_COMPILER}" "-DCOMMON_INCLUDE_DIR=${infiltratr_common_SOURCE_DIR}/include"
 COMMAND "${CMAKE_COMMAND}" --build "${CMAKE_CURRENT_BINARY_DIR}/firmware"
 DEPENDS module/firmware/CMakeLists.txt module/firmware/runtime.c module/firmware/descriptor.c
   native/ntfs_disk.c native/ntfs_disk.h
   "${infiltratr_common_SOURCE_DIR}/include/infiltratr/filesystem_module_v2.h"
 VERBATIM)
add_custom_target(ntfs-firmware DEPENDS "${NTFS_FIRMWARE_ELF}")
add_library(ntfs-driver SHARED
 native/ntfs_disk.c
 native/ntfs_common.c native/ntfs_catalog.c
 module/ntfs_v2_probe.c module/ntfs_v2_map.c
 $<TARGET_OBJECTS:ntfs-formatter>)
add_dependencies(ntfs-driver ntfs-firmware)
set_property(TARGET ntfs-driver APPEND PROPERTY LINK_DEPENDS "${NTFS_FIRMWARE_ELF}")
add_custom_command(TARGET ntfs-driver POST_BUILD
 COMMAND "${CMAKE_OBJCOPY}" --add-section ".infiltrator_boot=${NTFS_FIRMWARE_ELF}"
   --set-section-flags .infiltrator_boot=readonly "$<TARGET_FILE:ntfs-driver>" VERBATIM)
set_target_properties(ntfs-driver PROPERTIES PREFIX "" OUTPUT_NAME "NTFS" SUFFIX ".so")
target_compile_definitions(ntfs-driver PRIVATE _GNU_SOURCE _FILE_OFFSET_BITS=64)
target_compile_options(ntfs-driver PRIVATE -Wall -Wextra -Wpedantic -Werror
 -ffunction-sections -fdata-sections -fvisibility=hidden)
target_include_directories(ntfs-driver SYSTEM PRIVATE "${NTFS3G_INCLUDE_DIR}")
target_link_options(ntfs-driver PRIVATE -Wl,-z,defs -Wl,--gc-sections
 "-Wl,--version-script=${CMAKE_CURRENT_SOURCE_DIR}/module/v2-only.map")
target_include_directories(ntfs-driver PRIVATE
 "${CMAKE_CURRENT_SOURCE_DIR}/native" "${CMAKE_CURRENT_SOURCE_DIR}/compat"
 "${CMAKE_CURRENT_SOURCE_DIR}/include" "${CMAKE_CURRENT_BINARY_DIR}"
 "${infiltratr_common_SOURCE_DIR}/include")
target_link_libraries(ntfs-driver PRIVATE InfiltratrCommon::Common OpenSSL::Crypto
 Threads::Threads "${NTFS3G_LIBRARY}")

if(BUILD_TESTING)
 # Historical native worker, Defragmenter adapter and fixture API are
 # retained for regression only; none is linked into shipped NTFS.so.
 add_library(ntfs-legacy-test-support SHARED
  native/ntfs_disk.c
  native/ntfs_common.c native/ntfs_catalog.c
  native/ntfs_plan.c native/ntfs_plan_db.cpp native/ntfs_transaction.c
  native/ntfs_worker.c compat/legacy_services.c compat/runtime_alloc.c
  module/ntfs_module.c fixtures/ntfs_codec.c module/ntfs_fixture_bridge.c)
 set_source_files_properties(native/ntfs_worker.c
  PROPERTIES COMPILE_DEFINITIONS "main=ifs_ntfs_worker_main")
 set_source_files_properties(fixtures/ntfs_codec.c
  PROPERTIES COMPILE_DEFINITIONS "ldtm_raw_ntfs=ifs_ntfs_raw_fixture")
 target_compile_definitions(ntfs-legacy-test-support PRIVATE _GNU_SOURCE _FILE_OFFSET_BITS=64)
 target_include_directories(ntfs-legacy-test-support PRIVATE
  "${CMAKE_CURRENT_SOURCE_DIR}/native" "${CMAKE_CURRENT_SOURCE_DIR}/compat"
  "${CMAKE_CURRENT_SOURCE_DIR}/include" "${CMAKE_CURRENT_SOURCE_DIR}/fixtures/include"
  "${CMAKE_CURRENT_BINARY_DIR}" "${infiltratr_common_SOURCE_DIR}/include")
 target_link_libraries(ntfs-legacy-test-support PRIVATE ntfs-driver
  InfiltratrCommon::Common OpenSSL::Crypto SQLite::SQLite3 Threads::Threads
  "${NTFS3G_LIBRARY}")
endif()

if(BUILD_TESTING)
 # Qualify V2 directly against bounded host I/O, without publishing a
 # probe-only binary as a fully functional NTFS filesystem.
 add_executable(ntfs-v2-probe-test
   tests/test_ntfs_v2_probe.c)
 target_link_libraries(ntfs-v2-probe-test PRIVATE
   ntfs-driver ntfs-legacy-test-support InfiltratrCommon::Common)
 target_include_directories(ntfs-v2-probe-test PRIVATE
   "${CMAKE_CURRENT_SOURCE_DIR}/native")
 target_include_directories(ntfs-v2-probe-test PRIVATE "${infiltratr_common_SOURCE_DIR}/include")
 target_compile_options(ntfs-v2-probe-test PRIVATE
   -Wall -Wextra -Wpedantic -Werror)
 add_test(NAME ntfs-native-v2-probe COMMAND ntfs-v2-probe-test)
 add_executable(ntfs-v2-format-test tests/test_ntfs_v2_format.c)
 target_compile_definitions(ntfs-v2-format-test PRIVATE _GNU_SOURCE _FILE_OFFSET_BITS=64)
 target_compile_options(ntfs-v2-format-test PRIVATE -Wall -Wextra -Wpedantic -Werror)
 target_include_directories(ntfs-v2-format-test PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/native")
 target_link_libraries(ntfs-v2-format-test PRIVATE ntfs-driver ntfs-legacy-test-support InfiltratrCommon::Common SQLite::SQLite3 OpenSSL::Crypto)
 find_program(NTFS_LABEL_TOOL ntfslabel REQUIRED)
 find_program(NTFS_FIX_TOOL ntfsfix REQUIRED)
 find_program(NTFS_RESIZE_TOOL ntfsresize REQUIRED)
 find_program(NTFS_COPY_TOOL ntfscp REQUIRED)
 find_program(NTFS_MKFS_TOOL NAMES mkntfs REQUIRED)
 add_test(NAME ntfs-native-v2-format
  COMMAND /bin/sh "${CMAKE_CURRENT_SOURCE_DIR}/tests/check_ntfs_format.sh"
    "$<TARGET_FILE:ntfs-v2-format-test>" "${NTFS_LABEL_TOOL}" "${NTFS_FIX_TOOL}" "${NTFS_RESIZE_TOOL}" "${NTFS_COPY_TOOL}")
 # Test relocation bounds and recovery failures on a temporary regular file.
 # Only the workspace code is linked; target access is supplied by the fixture.
 add_executable(ntfs-workspace-safety-test tests/test_ntfs_workspace_safety.c native/ntfs_plan.c)
 target_compile_definitions(ntfs-workspace-safety-test PRIVATE _GNU_SOURCE _FILE_OFFSET_BITS=64)
 target_compile_options(ntfs-workspace-safety-test PRIVATE -Wall -Wextra -Wpedantic -Werror
   -ffunction-sections -fdata-sections)
 target_link_options(ntfs-workspace-safety-test PRIVATE -Wl,--gc-sections)
 target_include_directories(ntfs-workspace-safety-test PRIVATE
   "${CMAKE_CURRENT_SOURCE_DIR}/native" "${CMAKE_CURRENT_SOURCE_DIR}/compat"
   "${CMAKE_CURRENT_SOURCE_DIR}/include")
 target_link_libraries(ntfs-workspace-safety-test PRIVATE InfiltratrCommon::Common
   SQLite::SQLite3 OpenSSL::Crypto)
 add_test(NAME ntfs-workspace-safety COMMAND ntfs-workspace-safety-test)
 # Preserve historical V1 fixture tests outside the installed module.
 add_executable(ntfs-legacy-compat-test tests/test_module.c)
 target_include_directories(ntfs-legacy-compat-test PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/include" "${CMAKE_CURRENT_SOURCE_DIR}/compat")
 target_link_libraries(ntfs-legacy-compat-test PRIVATE ntfs-legacy-test-support)
 add_test(NAME ntfs-v1-consumer-compat COMMAND ntfs-legacy-compat-test)
 add_executable(ntfs-stream-index-test tests/test_ntfs_stream_index.c)
 target_include_directories(ntfs-stream-index-test PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/native" "${infiltratr_common_SOURCE_DIR}/include")
 target_link_libraries(ntfs-stream-index-test PRIVATE ntfs-legacy-test-support SQLite::SQLite3)
 add_test(NAME ntfs-native-stream-index COMMAND ntfs-stream-index-test)

 add_executable(ntfs-v2-namespace-test tests/test_ntfs_v2_namespace.c)
 target_compile_definitions(ntfs-v2-namespace-test PRIVATE _GNU_SOURCE _FILE_OFFSET_BITS=64)
 target_compile_options(ntfs-v2-namespace-test PRIVATE -Wall -Wextra -Wpedantic -Werror)
 target_include_directories(ntfs-v2-namespace-test PRIVATE
  "${CMAKE_CURRENT_SOURCE_DIR}/include"
  "${infiltratr_common_SOURCE_DIR}/include")
 target_link_libraries(ntfs-v2-namespace-test PRIVATE ntfs-driver InfiltratrCommon::Common)
 add_test(NAME ntfs-v2-real-media-namespace
  COMMAND bash "${CMAKE_CURRENT_SOURCE_DIR}/tests/ntfs-v2-namespace.sh"
   "$<TARGET_FILE:ntfs-v2-namespace-test>" "${NTFS_COPY_TOOL}" "${NTFS_MKFS_TOOL}")
 add_executable(ntfs-disk-media tests/test_disk_media.c)
 target_compile_options(ntfs-disk-media PRIVATE -Wall -Wextra -Wpedantic -Werror)
 target_include_directories(ntfs-disk-media PRIVATE "${infiltratr_common_SOURCE_DIR}/include")
 target_link_libraries(ntfs-disk-media PRIVATE ${CMAKE_DL_LIBS})
 add_test(NAME ntfs-native-disk-sectors
  COMMAND bash "${CMAKE_CURRENT_SOURCE_DIR}/tests/disk-media.sh"
    "$<TARGET_FILE:ntfs-disk-media>" "$<TARGET_FILE:ntfs-v2-namespace-test>" "$<TARGET_FILE:ntfs-driver>")
 add_test(NAME ntfs-production-v2-only
  COMMAND bash "${CMAKE_CURRENT_SOURCE_DIR}/tests/check_production_v2.sh"
    "$<TARGET_FILE:ntfs-driver>")

 add_executable(ntfs-journal-test tests/test_ntfs_journal.c)
 target_include_directories(ntfs-journal-test PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/native")
 target_link_libraries(ntfs-journal-test PRIVATE ntfs-legacy-test-support)
 add_test(NAME ntfs-journal-argument-validation COMMAND ntfs-journal-test)

endif()
install(TARGETS ntfs-driver LIBRARY DESTINATION /System/Filesystems)
# Runtime documentation: ship one PDF with this independent shared object.
install(FILES "${CMAKE_CURRENT_SOURCE_DIR}/docs/NTFS.pdf"
        DESTINATION /System/Resources/Documentation RENAME Filesystem-NTFS.pdf)
