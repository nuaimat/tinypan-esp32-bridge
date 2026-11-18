idf_component_register(
    SRCS
        "src/tinypan.c"
        "src/tinypan_bnep.c"
        "src/tinypan_bnep_transport.c"
        "src/tinypan_lwip_netif.c"
        "src/tinypan_slip_transport.c"
        "src/tinypan_supervisor.c"
        "src/tinypan_transport.c"
        "ports/esp32_classic/tinypan_hal_esp32.c"
    INCLUDE_DIRS
        "include"
    REQUIRES
        bt
        lwip
)

target_compile_definitions(${COMPONENT_LIB} PRIVATE
    TINYPAN_ENABLE_LWIP=1
    TINYPAN_HOST_PROVIDES_SYS_NOW=1
    # Each TX queue entry pins a full WiFi RX pbuf (~1.6 KB) until the
    # BT link drains it. 16 entries can pin ~26 KB of heap under WiFi->BT
    # forwarding load; 8 halves that while still keeping the BT pipe full.
    TINYPAN_TX_QUEUE_LEN=8
    # esp32_classic HAL's send_iovec() copies the payload into the L2CAP VFS
    # queue synchronously, so the transport can free pbufs immediately instead
    # of waiting for the deferred TX_COMPLETE (removes the 200pps ceiling).
    TINYPAN_HAL_SYNCHRONOUS_TX=1
)
