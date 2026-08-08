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
)
