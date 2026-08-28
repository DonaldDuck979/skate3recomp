# Vendored ENet (https://github.com/lsalzman/enet), v1.3.18, MIT license.
# Fan-made online-play transport for the Skate 3 recomp. See LICENSE in this
# directory. Kept as a plain static library so the single-player build links it
# with negligible cost and no external fetch.

add_library(enet STATIC
    callbacks.c
    compress.c
    host.c
    list.c
    packet.c
    peer.c
    protocol.c
    unix.c
    win32.c
)

target_include_directories(enet PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)

if(WIN32)
    # ENet's win32.c uses Winsock2 and the multimedia timer.
    target_link_libraries(enet PUBLIC ws2_32 winmm)
else()
    # Feature probes ENet's autotools build would normally define. These hold
    # on modern Linux and macOS toolchains (the recomp targets both).
    target_compile_definitions(enet PRIVATE
        HAS_SOCKLEN_T=1
        HAS_FCNTL=1
        HAS_POLL=1
        HAS_GETADDRINFO=1
        HAS_GETNAMEINFO=1
        HAS_GETHOSTBYNAME_R=1
        HAS_GETHOSTBYADDR_R=1
        HAS_INET_PTON=1
        HAS_INET_NTOP=1
        HAS_MSGHDR_FLAGS=1
    )
endif()

# ENet is C and not our code to lint; silence its warnings under -Wall builds.
if(NOT MSVC)
    target_compile_options(enet PRIVATE -w)
endif()
