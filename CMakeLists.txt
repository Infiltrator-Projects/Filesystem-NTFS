cmake_minimum_required(VERSION 3.20)
file(STRINGS "${CMAKE_CURRENT_SOURCE_DIR}/VERSION" FILESYSTEM_VERSION LIMIT_COUNT 1)
project(FilesystemNTFS VERSION ${FILESYSTEM_VERSION} LANGUAGES C CXX)
include(CTest)
include(FetchContent)
find_package(OpenSSL REQUIRED)
find_package(SQLite3 REQUIRED)
find_package(Threads REQUIRED)
set(CMAKE_C_STANDARD 11)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)
set(INFILTRATR_COMMON_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(INFILTRATR_COMMON_BUILD_SHARED OFF CACHE BOOL "" FORCE)
FetchContent_Declare(infiltratr_common
 GIT_REPOSITORY https://github.com/Infiltrator-Projects/Libraries.git
 GIT_TAG 5b5fb95988c8d0b53ed29fdecfb0b70274182fd6)
FetchContent_MakeAvailable(infiltratr_common)
# Common is a link-time dependency; its development files must not be installed
# by this standalone filesystem package.
set_property(DIRECTORY "${infiltratr_common_SOURCE_DIR}" PROPERTY EXCLUDE_FROM_ALL TRUE)
file(WRITE "${CMAKE_CURRENT_BINARY_DIR}/version.h" "#define LD_VERSION \"${PROJECT_VERSION}\"\n")
add_library(ntfs-driver SHARED
 native/ntfs_catalog.c native/ntfs_common.c native/ntfs_plan.c
 native/ntfs_plan_db.cpp native/ntfs_transaction.c native/ntfs_worker.c
 compat/legacy_services.c compat/runtime_alloc.c module/ntfs_v2_probe.c
 fixtures/ntfs_codec.c module/ntfs_fixture_bridge.c)
set_source_files_properties(native/ntfs_worker.c
 PROPERTIES COMPILE_DEFINITIONS "main=ifs_ntfs_worker_main")
set_source_files_properties(fixtures/ntfs_codec.c PROPERTIES
 COMPILE_DEFINITIONS "ldtm_raw_ntfs=ifs_ntfs_raw_fixture")
set_target_properties(ntfs-driver PROPERTIES PREFIX "" OUTPUT_NAME "NTFS" SUFFIX ".so")
target_compile_definitions(ntfs-driver PRIVATE _GNU_SOURCE _FILE_OFFSET_BITS=64)
target_compile_options(ntfs-driver PRIVATE -Wall -Wextra -Wpedantic -Werror)
target_link_options(ntfs-driver PRIVATE -Wl,-z,defs)
target_include_directories(ntfs-driver PRIVATE
 "${CMAKE_CURRENT_SOURCE_DIR}/native" "${CMAKE_CURRENT_SOURCE_DIR}/compat"
 "${CMAKE_CURRENT_SOURCE_DIR}/include" "${CMAKE_CURRENT_SOURCE_DIR}/fixtures/include"
 "${CMAKE_CURRENT_BINARY_DIR}" "${infiltratr_common_SOURCE_DIR}/include")
target_link_libraries(ntfs-driver PRIVATE InfiltratrCommon::Common OpenSSL::Crypto SQLite::SQLite3 Threads::Threads)
if(BUILD_TESTING)
 # Qualify V2 directly against bounded host I/O, without publishing a
 # probe-only binary as a fully functional NTFS filesystem.
 add_executable(ntfs-v2-probe-test
   tests/test_ntfs_v2_probe.c)
 target_link_libraries(ntfs-v2-probe-test PRIVATE
   ntfs-driver InfiltratrCommon::Common)
 target_include_directories(ntfs-v2-probe-test PRIVATE
   "${CMAKE_CURRENT_SOURCE_DIR}/native")
 target_include_directories(ntfs-v2-probe-test PRIVATE "${infiltratr_common_SOURCE_DIR}/include")
 target_compile_options(ntfs-v2-probe-test PRIVATE
   -Wall -Wextra -Wpedantic -Werror)
 add_test(NAME ntfs-native-v2-probe COMMAND ntfs-v2-probe-test)
 add_executable(ntfs-stream-index-test tests/test_ntfs_stream_index.c)
 target_include_directories(ntfs-stream-index-test PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/native" "${infiltratr_common_SOURCE_DIR}/include")
 target_link_libraries(ntfs-stream-index-test PRIVATE ntfs-driver)
 add_test(NAME ntfs-native-stream-index COMMAND ntfs-stream-index-test)
 add_executable(ntfs-journal-test tests/test_ntfs_journal.c)
 target_include_directories(ntfs-journal-test PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/native")
 target_link_libraries(ntfs-journal-test PRIVATE ntfs-driver)
 add_test(NAME ntfs-journal-argument-validation COMMAND ntfs-journal-test)

endif()
install(TARGETS ntfs-driver LIBRARY DESTINATION /System/Filesystems)
# Runtime documentation: ship one PDF with this independent shared object.
install(FILES "${CMAKE_CURRENT_SOURCE_DIR}/docs/NTFS.pdf"
        DESTINATION /System/Resources/Documentation RENAME Filesystem-NTFS.pdf)
