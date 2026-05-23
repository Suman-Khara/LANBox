cmake_minimum_required(VERSION 3.10)

project(lanbox VERSION 0.1 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED True)

# Fix Windows byte conflict globally
if (WIN32)
    add_compile_definitions(_HAS_STD_BYTE=0 WIN32_LEAN_AND_MEAN NOMINMAX)
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
    set(OPENSSL_LIB_DIR "${OPENSSL_ROOT_DIR}/lib/VC/x64/MT")  # MT = Static
    
    target_include_directories(lanbox PRIVATE ${OPENSSL_INCLUDE_DIR})
    target_link_libraries(lanbox PRIVATE 
        "${OPENSSL_LIB_DIR}/libssl.lib"
        "${OPENSSL_LIB_DIR}/libcrypto.lib"
        ws2_32
        iphlpapi
        crypt32  # Needed for static OpenSSL
    )
else()
    target_link_libraries(lanbox PRIVATE pthread ssl crypto)
endif()