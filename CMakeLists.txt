cmake_minimum_required(VERSION 3.10)

project(lanbox VERSION 0.1 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED True)

# Fix Windows byte conflict globally
if (WIN32)
    add_compile_definitions(_HAS_STD_BYTE=0 WIN32_LEAN_AND_MEAN NOMINMAX)
else()
    # Linux also needs this for OpenSSL + C++17
    add_compile_definitions(_HAS_STD_BYTE=0)
endif()

include_directories(include)

file(GLOB SOURCES "src/*.cpp")
add_executable(lanbox ${SOURCES})

# Link Windows socket library
if (WIN32)
    target_link_libraries(lanbox PRIVATE ws2_32 iphlpapi)
    
    # OpenSSL paths for Windows
    set(OPENSSL_ROOT_DIR "C:/Program Files/OpenSSL-Win64")
    set(OPENSSL_INCLUDE_DIR "${OPENSSL_ROOT_DIR}/include")
    set(OPENSSL_LIB_DIR "${OPENSSL_ROOT_DIR}/lib/VC/x64/MT")
    
    target_include_directories(lanbox PRIVATE ${OPENSSL_INCLUDE_DIR})
    target_link_libraries(lanbox PRIVATE 
        "${OPENSSL_LIB_DIR}/libssl.lib"
        "${OPENSSL_LIB_DIR}/libcrypto.lib"
        ws2_32
        iphlpapi
        crypt32
    )
else()
    target_link_libraries(lanbox PRIVATE pthread ssl crypto)
endif()

add_executable(test_step1 src/test_step1.cpp src/protocol.cpp src/crypto.cpp)
target_include_directories(test_step1 PRIVATE include)

if (WIN32)
    target_compile_definitions(test_step1 PRIVATE _HAS_STD_BYTE=0 WIN32_LEAN_AND_MEAN NOMINMAX)
    set(OPENSSL_ROOT_DIR "C:/Program Files/OpenSSL-Win64")
    set(OPENSSL_LIB_DIR "${OPENSSL_ROOT_DIR}/lib/VC/x64/MT")
    target_include_directories(test_step1 PRIVATE "${OPENSSL_ROOT_DIR}/include")
    target_link_libraries(test_step1 PRIVATE
        "${OPENSSL_LIB_DIR}/libssl.lib"
        "${OPENSSL_LIB_DIR}/libcrypto.lib"
        ws2_32 iphlpapi crypt32)
else()
    target_link_libraries(test_step1 PRIVATE ssl crypto pthread)
endif()

add_executable(test_step2
    src/test_step2.cpp
    src/group_manager.cpp)
target_include_directories(test_step2 PRIVATE include)

if (WIN32)
    target_compile_definitions(test_step2 PRIVATE _HAS_STD_BYTE=0 WIN32_LEAN_AND_MEAN NOMINMAX)
    set(OPENSSL_ROOT_DIR "C:/Program Files/OpenSSL-Win64")
    set(OPENSSL_LIB_DIR "${OPENSSL_ROOT_DIR}/lib/VC/x64/MT")
    target_include_directories(test_step2 PRIVATE "${OPENSSL_ROOT_DIR}/include")
    target_link_libraries(test_step2 PRIVATE
        "${OPENSSL_LIB_DIR}/libssl.lib"
        "${OPENSSL_LIB_DIR}/libcrypto.lib"
        ws2_32 iphlpapi crypt32)
else()
    target_link_libraries(test_step2 PRIVATE ssl crypto pthread)
endif()

add_executable(test_step3
    src/test_step3.cpp
    src/folder_watcher.cpp
    src/group_manager.cpp)
target_include_directories(test_step3 PRIVATE include)

if (WIN32)
    target_compile_definitions(test_step3 PRIVATE _HAS_STD_BYTE=0 WIN32_LEAN_AND_MEAN NOMINMAX)
    set(OPENSSL_ROOT_DIR "C:/Program Files/OpenSSL-Win64")
    set(OPENSSL_LIB_DIR "${OPENSSL_ROOT_DIR}/lib/VC/x64/MT")
    target_include_directories(test_step3 PRIVATE "${OPENSSL_ROOT_DIR}/include")
    target_link_libraries(test_step3 PRIVATE
        "${OPENSSL_LIB_DIR}/libssl.lib"
        "${OPENSSL_LIB_DIR}/libcrypto.lib"
        ws2_32 iphlpapi crypt32)
else()
    target_link_libraries(test_step3 PRIVATE ssl crypto pthread)
endif()

add_executable(test_step4a
    src/test_step4a.cpp
    src/sync_manager.cpp
    src/folder_watcher.cpp
    src/group_manager.cpp
    src/config.cpp
    src/peer.cpp
    src/crypto.cpp
    src/protocol.cpp)
target_include_directories(test_step4a PRIVATE include)

if (WIN32)
    target_compile_definitions(test_step4a PRIVATE _HAS_STD_BYTE=0 WIN32_LEAN_AND_MEAN NOMINMAX)
    set(OPENSSL_ROOT_DIR "C:/Program Files/OpenSSL-Win64")
    set(OPENSSL_LIB_DIR "${OPENSSL_ROOT_DIR}/lib/VC/x64/MT")
    target_include_directories(test_step4a PRIVATE "${OPENSSL_ROOT_DIR}/include")
    target_link_libraries(test_step4a PRIVATE
        "${OPENSSL_LIB_DIR}/libssl.lib"
        "${OPENSSL_LIB_DIR}/libcrypto.lib"
        ws2_32 iphlpapi crypt32)
else()
    target_link_libraries(test_step4a PRIVATE ssl crypto pthread)
endif()

add_executable(test_step4b
    src/test_step4b.cpp
    src/sync_manager.cpp
    src/folder_watcher.cpp
    src/group_manager.cpp
    src/config.cpp
    src/peer.cpp
    src/crypto.cpp
    src/protocol.cpp)
target_include_directories(test_step4b PRIVATE include)

if (WIN32)
    target_compile_definitions(test_step4b PRIVATE _HAS_STD_BYTE=0 WIN32_LEAN_AND_MEAN NOMINMAX)
    set(OPENSSL_ROOT_DIR "C:/Program Files/OpenSSL-Win64")
    set(OPENSSL_LIB_DIR "${OPENSSL_ROOT_DIR}/lib/VC/x64/MT")
    target_include_directories(test_step4b PRIVATE "${OPENSSL_ROOT_DIR}/include")
    target_link_libraries(test_step4b PRIVATE
        "${OPENSSL_LIB_DIR}/libssl.lib"
        "${OPENSSL_LIB_DIR}/libcrypto.lib"
        ws2_32 iphlpapi crypt32)
else()
    target_link_libraries(test_step4b PRIVATE ssl crypto pthread)
endif()

add_executable(test_step4b_sender
    src/test_step4b_sender.cpp
    src/protocol.cpp
    src/crypto.cpp
    src/sync_manager.cpp
    src/folder_watcher.cpp
    src/group_manager.cpp
    src/config.cpp
    src/peer.cpp)
target_include_directories(test_step4b_sender PRIVATE include)

if (WIN32)
    target_compile_definitions(test_step4b_sender PRIVATE _HAS_STD_BYTE=0 WIN32_LEAN_AND_MEAN NOMINMAX)
    set(OPENSSL_ROOT_DIR "C:/Program Files/OpenSSL-Win64")
    set(OPENSSL_LIB_DIR "${OPENSSL_ROOT_DIR}/lib/VC/x64/MT")
    target_include_directories(test_step4b_sender PRIVATE "${OPENSSL_ROOT_DIR}/include")
    target_link_libraries(test_step4b_sender PRIVATE
        "${OPENSSL_LIB_DIR}/libssl.lib"
        "${OPENSSL_LIB_DIR}/libcrypto.lib"
        ws2_32 iphlpapi crypt32)
else()
    target_link_libraries(test_step4b_sender PRIVATE ssl crypto pthread)
endif()