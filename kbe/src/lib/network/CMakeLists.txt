add_library(network STATIC
    address.cpp
    bundle.cpp
    bundle_broadcast.cpp
    channel.cpp
    common.cpp
    completion_rearm_queue.cpp
    completion_tcp_send_queue.cpp
    delayed_channels.cpp
    encryption_filter.cpp
    endpoint.cpp
    error_reporter.cpp
    event_dispatcher.cpp
    event_poller.cpp
    fixed_messages.cpp
    http_utility.cpp
    interface_defs.cpp
    ikcp.c
    kcp_update_queue.cpp
    kcp_update_scheduler.cpp
    kcp_packet_reader.cpp
    kcp_packet_receiver.cpp
    kcp_packet_sender.cpp
    listener_receiver.cpp
    listener_udp_receiver.cpp
    message_handler.cpp
    network_interface.cpp
    network_stats.cpp
    packet_filter.cpp
    packet_reader.cpp
    packet_receiver.cpp
    packet_sender.cpp
    poller_completion.cpp
    poller_epoll.cpp
    poller_iocp.cpp
    poller_io_uring.cpp
    poller_kqueue.cpp
    tcp_packet.cpp
    tcp_packet_receiver.cpp
    tcp_packet_sender.cpp
    udp_packet.cpp
    udp_packet_receiver.cpp
    udp_packet_sender.cpp
    udp_send_backpressure.cpp
    websocket_packet_filter.cpp
    websocket_packet_reader.cpp
    websocket_protocol.cpp
)

kbe_configure_target(network)
target_compile_definitions(network PRIVATE CURL_STATICLIB KBE_USE_VCPKG_OPENSSL)
target_link_libraries(network PUBLIC common helper resmgr thread xml)
target_link_libraries(network PRIVATE CURL::libcurl OpenSSL::SSL OpenSSL::Crypto)

if(WIN32)
    target_link_libraries(network PRIVATE iphlpapi ws2_32 mswsock)
endif()
