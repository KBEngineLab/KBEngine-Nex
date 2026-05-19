# ---------------- 源文件 ----------------
set(SRCS
	address.cpp
	bundle_broadcast.cpp
	bundle.cpp
	channel.cpp
	common.cpp
	delayed_channels.cpp
	error_reporter.cpp
	event_dispatcher.cpp
	event_poller.cpp
	encryption_filter.cpp
	fixed_messages.cpp
	http_utility.cpp
	interface_defs.cpp
	message_handler.cpp
	listener_receiver.cpp
	listener_tcp_receiver.cpp
	listener_udp_receiver.cpp
	network_stats.cpp
	network_interface.cpp
	packet_filter.cpp
	packet_reader.cpp
	packet_sender.cpp
	packet_receiver.cpp
	poller_completion.cpp
	poller_io_uring.cpp
	poller_kqueue.cpp
	endpoint.cpp
	tcp_packet.cpp
	tcp_packet_receiver.cpp
	tcp_packet_sender.cpp
	udp_packet.cpp
	udp_packet_receiver.cpp
	udp_packet_sender.cpp
	ikcp.c
	kcp_packet_reader.cpp
	kcp_packet_receiver.cpp
	kcp_packet_sender.cpp
	websocket_packet_reader.cpp
	websocket_packet_filter.cpp
	websocket_protocol.cpp
)
# ---------------- 创建静态库 ----------------
add_library(network STATIC ${SRCS})

# ---------------- OpenSSL ----------------
target_link_libraries(network PRIVATE KBEOpenSSL::SSL)


target_link_libraries(network PRIVATE CURL::libcurl)


target_link_libraries(network PUBLIC 
	fmt::fmt 
	xml
)
