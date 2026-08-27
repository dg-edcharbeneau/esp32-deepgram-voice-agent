# Overrides ESP-IDF's `tcp_transport` component by name so two spots in
# transport_ws.c can be changed -- see the LOCAL PATCH comments in that file:
#
#   1. The Host header, because agent.deepgram.com 404s on "Host: host:443".
#   2. _ws_write()'s poll_write timeout, because the WebSocket client treats a
#      full send queue as a broken socket and tears down a live session over it.
#
# Only transport_ws.c and Kconfig are copied here. Every other source and both
# include directories are referenced straight out of $IDF_PATH, so this override
# stays a single-file diff instead of a fork of the whole component.
#
# FORKED FROM ESP-IDF 5.5.5. That matters more than it looks: because the
# siblings and private_include come from $IDF_PATH, an IDF upgrade moves
# everything around transport_ws.c while transport_ws.c stays where it is, and it
# uses the private esp_transport_internal.h. Two things guard that --
#
#   local.patch     the diff against upstream, checked in
#   check-patch.sh  regenerates it and fails if it no longer matches
#
# -- plus an #error in transport_ws.c gated on MAJOR.MINOR. The guard catches a
# version bump; local.patch catches upstream editing the file in place within
# 5.5.x, which the guard cannot see. Run check-patch.sh after any IDF change.

set(IDF_TCP_TRANSPORT "$ENV{IDF_PATH}/components/tcp_transport")

set(srcs
    "${IDF_TCP_TRANSPORT}/transport.c"
    "${IDF_TCP_TRANSPORT}/transport_ssl.c"
    "${IDF_TCP_TRANSPORT}/transport_internal.c")

if(CONFIG_LWIP_IPV4)
    list(APPEND srcs "${IDF_TCP_TRANSPORT}/transport_socks_proxy.c")
endif()

if(CONFIG_WS_TRANSPORT)
    # The patched copy, from this directory.
    list(APPEND srcs "transport_ws.c")
endif()

set(req esp-tls)
if(NOT ${IDF_TARGET} STREQUAL "linux")
    list(APPEND req lwip esp_timer)
endif()

# "include" is this directory's, and holds exactly one header: the local
# patches' public surface. Everything else still comes from $IDF_PATH.
idf_component_register(SRCS "${srcs}"
                    INCLUDE_DIRS "${IDF_TCP_TRANSPORT}/include" "include"
                    PRIV_INCLUDE_DIRS "${IDF_TCP_TRANSPORT}/private_include"
                    REQUIRES ${req})

# Verbatim from upstream, and restored rather than kept out: dropping it made
# this override differ from the component it replaces in a way nothing recorded.
# Irrelevant on the S3, but the point of the override is to be a single-file
# diff, and silently losing a target's link dependency is not that.
if(${IDF_TARGET} STREQUAL "linux")
    # Check if LWIP in the build for linux target to add esp_timer to the dependencies
    # since socks_proxy transport needs it and lwip & linux build could use it
    idf_build_get_property(build_components BUILD_COMPONENTS)
    if("lwip" IN_LIST build_components)
        idf_component_get_property(esp_timer esp_timer COMPONENT_LIB)
        target_link_libraries(${COMPONENT_LIB} PUBLIC ${esp_timer})
    endif()
endif()
