# Overrides ESP-IDF's `tcp_transport` component by name so one line of
# transport_ws.c can be changed -- see the LOCAL PATCH comment in that file for
# why (agent.deepgram.com 404s on "Host: host:443").
#
# Only transport_ws.c and Kconfig are copied here. Every other source and both
# include directories are referenced straight out of $IDF_PATH, so this override
# stays a single-file diff instead of a fork of the whole component.

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

idf_component_register(SRCS "${srcs}"
                    INCLUDE_DIRS "${IDF_TCP_TRANSPORT}/include"
                    PRIV_INCLUDE_DIRS "${IDF_TCP_TRANSPORT}/private_include"
                    REQUIRES ${req})
