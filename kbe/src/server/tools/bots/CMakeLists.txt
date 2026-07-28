kbe_add_server_executable(bots KBE_BOTS
    NO_SERVER
    SOURCES
        bots.cpp
        bots_active_report_handler.cpp
        bots_interface.cpp
        clientobject.cpp
        create_and_login_handler.cpp
        kcp_packet_receiver_ex.cpp
        kcp_packet_sender_ex.cpp
        main.cpp
        profile.cpp
        pybots.cpp
        tcp_packet_receiver_ex.cpp
        tcp_packet_sender_ex.cpp
    DEFINITIONS
        USE_PYTHON
    LIBRARIES
        # client_lib 必须先提供客户端全局状态，server 随后只拉入 Bots 使用的服务端辅助对象。
        # client_lib must provide client globals first; server then contributes only the server-side helpers used by Bots.
        client_lib
        server
)
