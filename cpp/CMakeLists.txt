CMAKE_MINIMUM_REQUIRED (VERSION 3.8)

set(CHIPSET_NAME "s3l")
set(CMAKE_SYSTEM_NAME Linux)
set(CMAKE_SYSTEM_PROCESSOR ARM)

if (${CHIPSET_NAME} STREQUAL s3l)
  set(TOOLS               /usr/local/linaro-armv7ahf-2015.11-gcc5.2)
  set(CMAKE_C_COMPILER    ${TOOLS}/bin/arm-linux-gnueabihf-gcc)
  set(CMAKE_CXX_COMPILER  ${TOOLS}/bin/arm-linux-gnueabihf-g++)
  set(CMAKE_STRIP         ${TOOLS}/bin/arm-linux-gnueabihf-strip)
  set(CMAKE_AR 		 ${TOOLS}/bin/arm-linux-gnueabihf-ar)
  set(CMAKE_RANLIB	 ${TOOLS}/bin/arm-linux-gnueabihf-ranlib)
  set(CMAKE_LD		 ${TOOLS}/bin/arm-linux-gnueabihf-gnu-ld)
  include_directories(${TOOLS}/include)
endif(${CHIPSET_NAME} STREQUAL s3l )

if(${CHIPSET_NAME} STREQUAL s5l)
  set(TOOLS               /usr/local/linaro-aarch64-2017.08-gcc7.1)
  set(CMAKE_C_COMPILER    ${TOOLS}/bin/aarch64-linux-gnu-gcc)
  set(CMAKE_CXX_COMPILER  ${TOOLS}/bin/aarch64-linux-gnu-g++)
  set(CMAKE_STRIP         ${TOOLS}/bin/aarch64-linux-gnu-strip)
  set(CMAKE_AR 		 ${TOOLS}/bin/aarch64-linux-gnu-ar)
  set(CMAKE_RANLIB	 ${TOOLS}/bin/aarch64-linux-gnu-ranlib)
  set(CMAKE_LD		 ${TOOLS}/bin/aarch64-linux-gnu-ld)
endif(${CHIPSET_NAME} STREQUAL s5l)

add_definitions(-O3 -g -fdata-sections -ffunction-sections)
add_definitions(-march=armv7 -DSOCKLEN_T=socklen_t -DNO_SSTREAM=1 -D_LARGEFILE_SOURCE=1 -D_FILE_OFFSET_BITS=64)

set(LIVE555_PATH ${CMAKE_CURRENT_SOURCE_DIR}/addons/live)

include_directories(${CMAKE_CURRENT_SOURCE_DIR}/src)
include_directories(${LIVE555_PATH}/include)
include_directories(${LIVE555_PATH}/liveMedia/include)
include_directories(${LIVE555_PATH}/groupsock/include)
include_directories(${LIVE555_PATH}/UsageEnvironment/include)
include_directories(${LIVE555_PATH}/BasicUsageEnvironment/include)
include_directories(${LIVE555_PATH}/testProgs/include)

link_directories(${LIVE555_PATH}/liveMedia)
link_directories(${LIVE555_PATH}/groupsock)
link_directories(${LIVE555_PATH}/UsageEnvironment)
link_directories(${LIVE555_PATH}/BasicUsageEnvironment)
link_directories(${LIVE555_PATH}/testProgs)

file(
    GLOB
    LIVE_MEDIA_HEADERS
    "${LIVE555_PATH}/liveMedia/include/*.h"
    "${LIVE555_PATH}/liveMedia/include/*.hh"
    "${LIVE555_PATH}/groupsock/include/*.h"
    "${LIVE555_PATH}/groupsock/include/*.hh"
    "${LIVE555_PATH}/UsageEnvironment/include/*.hh"
    "${LIVE555_PATH}/BasicUsageEnvironment/include/*.hh"
)

file(
    GLOB
    LIVE_MEDIA_SRC
    "${LIVE555_PATH}/liveMedia/*.c"
    "${LIVE555_PATH}/liveMedia/*.cpp"
    "${LIVE555_PATH}/groupsock/*.c"
    "${LIVE555_PATH}/groupsock/*.cpp"
    "${LIVE555_PATH}/BasicUsageEnvironment/*.cpp"
    "${LIVE555_PATH}/UsageEnvironment/*.cpp"
)

add_library(liveMedia STATIC ${LIVE_MEDIA_HEADERS} ${LIVE_MEDIA_SRC})

find_package(OpenSSL REQUIRED)

add_executable(
    webrtc
    ${CMAKE_CURRENT_SOURCE_DIR}/src/webrtc.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/MemorySink.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/WebRTCRTSPClient.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/StreamClientState.cpp
)

target_link_libraries(
    webrtc
    liveMedia
    ${OPENSSL_SSL_LIBRARY}
    ${OPENSSL_CRYPTO_LIBRARY}
)
