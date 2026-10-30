cmake_minimum_required(VERSION 3.20)
project(FilesystemNTFS VERSION 1.0.0 LANGUAGES C CXX)
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
 GIT_TAG c76ec95ef66fc728df6a906ad75a6a4dd63b419b)
FetchContent_MakeAvailable(infiltratr_common)
file(WRITE "${CMAKE_CURRENT_BINARY_DIR}/version.h" "#define LD_VERSION \"1.0.0\"\n")
add_library(ntfs-driver SHARED
 native/ntfs_catalog.c native/ntfs_common.c native/ntfs_plan.c
 native/ntfs_plan_db.cpp native/ntfs_transaction.c native/ntfs_worker.c
 compat/legacy_services.c compat/runtime_alloc.c module/ntfs_module.c
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
 "${CMAKE_CURRENT_BINARY_DIR}")
target_link_libraries(ntfs-driver PRIVATE InfiltratrCommon::Common OpenSSL::Crypto SQLite::SQLite3 Threads::Threads)
if(BUILD_TESTING)
 add_executable(ntfs-module-test tests/test_module.c)
 target_include_directories(ntfs-module-test PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/include" "${CMAKE_CURRENT_SOURCE_DIR}/compat" "${CMAKE_CURRENT_SOURCE_DIR}/fixtures/include")
 target_link_libraries(ntfs-module-test PRIVATE ntfs-driver)
 add_test(NAME ntfs-module-contract COMMAND ntfs-module-test)
endif()
install(TARGETS ntfs-driver LIBRARY DESTINATION /System/Filesystems)
