if(NOT DEFINED KBE_SOURCE_ROOT OR NOT IS_DIRECTORY "${KBE_SOURCE_ROOT}")
    message(FATAL_ERROR "KBE_SOURCE_ROOT must identify the kbe/src directory")
endif()

file(READ "${KBE_SOURCE_ROOT}/lib/network/packet_receiver.cpp" _packet_receiver)
file(READ "${KBE_SOURCE_ROOT}/lib/network/channel.cpp" _channel)
file(READ "${KBE_SOURCE_ROOT}/lib/network/poller_io_uring.cpp" _io_uring)
file(READ "${KBE_SOURCE_ROOT}/lib/network/poller_iocp.cpp" _iocp)
file(READ "${KBE_SOURCE_ROOT}/lib/network/poller_completion.cpp" _completion)
file(READ "${KBE_SOURCE_ROOT}/lib/network/poller_completion.h" _completion_header)
file(READ "${KBE_SOURCE_ROOT}/lib/network/completion_udp_receive_depth.h" _udp_receive_depth)
file(READ "${KBE_SOURCE_ROOT}/lib/network/common.cpp" _network_common)
file(READ "${KBE_SOURCE_ROOT}/../res/server/kbengine_defaults.xml" _defaults)

# completion 后端每次通知只能消费一个报文；readiness 后端仍可 drain 到 EAGAIN。
# Completion backends consume one packet per notification while readiness backends retain EAGAIN draining.
foreach(_required
        "pPoller->supportsCompletion()"
        "this->processRecv(true);"
        "while (this->processRecv(false))")
    string(FIND "${_packet_receiver}" "${_required}" _required_pos)
    if(_required_pos EQUAL -1)
        message(FATAL_ERROR "PacketReceiver completion fairness contract is missing: ${_required}")
    endif()
endforeach()

# 生产端背压必须同时统计 native send context 的在途字节，并使用控制消息可接受的低水位。
# Producer backpressure counts native in-flight send bytes and uses watermarks sized for control-message latency.
foreach(_required
		"size_t pendingTcpWriteBytes;"
		"pendingTcpSends.pendingBytes() +")
	string(FIND "${_completion_header}${_completion}" "${_required}" _required_pos)
	if(_required_pos EQUAL -1)
		message(FATAL_ERROR "Completion TCP in-flight backlog accounting is missing: ${_required}")
	endif()
endforeach()
foreach(_source IN ITEMS _io_uring _iocp)
	foreach(_required
			"pendingTcpWriteBytes ="
			"pendingTcpWriteBytes = 0")
		string(FIND "${${_source}}" "${_required}" _required_pos)
		if(_required_pos EQUAL -1)
			message(FATAL_ERROR "${_source} TCP in-flight lifecycle accounting is missing: ${_required}")
		endif()
	endforeach()
endforeach()

# io_uring may synchronously drain UDP datagrams after a recv CQE, but both fresh CQE
# dequeue and burst expansion must stay behind the same local fairness watermark.
foreach(_required
        "completionPendingLocalCount() < completionDequeueWatermark"
        "completionPendingLimit = completionDequeueWatermark"
        "prepareUdpCompletions(completionPendingLimit)"
        "drainUdpReceiveBurst(fd, drainLimit)"
        "remainingSlots, remainingBurstSize")
    string(FIND "${_io_uring}" "${_required}" _required_pos)
    if(_required_pos EQUAL -1)
        message(FATAL_ERROR "io_uring local completion watermark contract is missing: ${_required}")
    endif()
endforeach()
foreach(_required
		"IO_URING_UDP_KERNEL_RECEIVE_DEPTH = 1"
		"return iocpUdpReceiveDepth(connected)")
	string(FIND "${_udp_receive_depth}" "${_required}" _required_pos)
	if(_required_pos EQUAL -1)
		message(FATAL_ERROR "io_uring UDP single-waiter receive contract is missing: ${_required}")
	endif()
endforeach()
foreach(_required
		"g_completionIntTcpSendBackpressureHighBytes = 128 * 1024"
		"g_completionIntTcpSendBackpressureLowBytes = 32 * 1024")
	string(FIND "${_network_common}" "${_required}" _required_pos)
	if(_required_pos EQUAL -1)
		message(FATAL_ERROR "Completion producer backpressure default is missing: ${_required}")
	endif()
endforeach()
foreach(_required
		"<internalHighBytes> 131072 </internalHighBytes>"
		"<internalLowBytes> 32768 </internalLowBytes>")
	string(FIND "${_defaults}" "${_required}" _required_pos)
	if(_required_pos EQUAL -1)
		message(FATAL_ERROR "Completion producer backpressure XML default is missing: ${_required}")
	endif()
endforeach()

# Channel 已经处于 sending 时，新 Bundle 仍须进入 completion 后端；否则长回调会在
# Channel 层积压数千条小消息，后端的发送推进机制根本没有机会工作。
# New Bundles must still enter a completion backend while the Channel is already sending;
# otherwise long callbacks accumulate thousands of small messages above the native send queue.
string(FIND "${_channel}" "void Channel::send(Bundle * pBundle)" _channel_send_begin)
string(FIND "${_channel}" "void Channel::sendTo(bool reliable" _channel_send_end)
math(EXPR _channel_send_length "${_channel_send_end} - ${_channel_send_begin}")
string(SUBSTRING "${_channel}" ${_channel_send_begin} ${_channel_send_length} _channel_send)
foreach(_required
        "else if (pPoller != NULL && pPoller->supportsCompletion())"
        "KBE_ASSERT(pPacketSender_ != NULL);"
        "pPacketSender_->processSend(this);")
    string(FIND "${_channel_send}" "${_required}" _required_pos)
    if(_required_pos EQUAL -1)
        message(FATAL_ERROR "Channel completion send-through contract is missing: ${_required}")
    endif()
endforeach()

# io_uring 的同步 UDP burst 必须展开为逐 datagram 的逻辑 completion，不能把整批隐藏在一个 CQE 后面。
# A synchronous io_uring UDP burst must expand into one logical completion per datagram.
foreach(_required
        "uint32 drainedDatagrams = 0;"
        "drainedDatagrams = drainUdpReceiveBurst(fd, drainLimit);"
		"if (pending.notifyRead)"
        "index < drainedDatagrams"
        "drainedCompletion.notifyRead = true"
        "pendingUdpCompletions_.push_back(drainedCompletion)"
		"pendingTcpSendCompletions_"
		"IO_URING_TCP_SEND_PRIORITY_BURST_SIZE"
		"consecutiveTcpSendCompletionCount_ < IO_URING_TCP_SEND_PRIORITY_BURST_SIZE"
		"IO_URING_NON_UDP_PRIORITY_BURST_SIZE"
		"consecutiveNonUdpCompletionCount_ < IO_URING_NON_UDP_PRIORITY_BURST_SIZE"
		"consecutiveNonUdpCompletionCount_ = 0")
    string(FIND "${_io_uring}" "${_required}" _required_pos)
    if(_required_pos EQUAL -1)
        message(FATAL_ERROR "io_uring UDP completion accounting is missing: ${_required}")
    endif()
endforeach()

# 已从有效 CQE 搬入用户态的 UDP 通知必须随 fd 注销一起清理，不能在 Channel 同步关闭后误计为 stale。
# UDP notifications staged from valid CQEs must be discarded with fd deregistration and must not
# be reported as stale when a Channel closes synchronously from an earlier callback.
foreach(_required
		"discardPreparedUdpCompletions(fd, state.socket, state.generation);"
		"void IoUringPoller::discardPreparedUdpCompletions"
		"normal lifetime race, not a stale kernel CQE")
	string(FIND "${_io_uring}" "${_required}" _required_pos)
	if(_required_pos EQUAL -1)
		message(FATAL_ERROR "io_uring prepared UDP close-race contract is missing: ${_required}")
	endif()
endforeach()

# TCP send 的内联推进可以像 IOCP 一样批量取 CQE，但不能在业务回调中展开 UDP burst。
# Inline TCP send progress may dequeue CQEs like IOCP, but it must not expand UDP bursts
# from inside a business callback where the completion fairness budget is not active.
string(FIND "${_io_uring}" "bool IoUringPoller::progressTcpSend" _progress_begin)
string(FIND "${_io_uring}" "bool IoUringPoller::armUdpSend" _progress_end)
math(EXPR _progress_length "${_progress_end} - ${_progress_begin}")
string(SUBSTRING "${_io_uring}" ${_progress_begin} ${_progress_length} _progress_source)
string(FIND "${_progress_source}" "prepareUdpCompletions();" _inline_udp_prepare)
if(NOT _inline_udp_prepare EQUAL -1)
	message(FATAL_ERROR "io_uring inline TCP progress must not expand UDP completions")
endif()

# 原生 completion 后端必须立即投递空闲 TCP socket 的首个发送；积压达到阈值后，
# 只内联收割当前 socket 的成功 send completion，让长业务回调期间仍能继续有序提交。
# Native completion backends submit the first idle TCP send immediately. Once backlog reaches
# the threshold, only a successful send completion for the current socket is harvested inline
# so a long business callback can keep submitting ordered data.
foreach(_source IN ITEMS _io_uring _iocp)
    foreach(_required
            "if (state.pPendingWriteContext == NULL)"
            "if (!armTcpSend(fd, state))"
            "requestRearm(fd, REARM_WRITE);"
            "progressTcpSend(fd, state)"
			"completionPendingLocalCount() >= static_cast<uint64>(g_maxCompletionsPerTick)"
            "pendingTcpSends.pendingBytes()")
        string(FIND "${${_source}}" "${_required}" _required_pos)
        if(_required_pos EQUAL -1)
            message(FATAL_ERROR "${_source} immediate TCP submission contract is missing: ${_required}")
        endif()
    endforeach()
endforeach()

foreach(_required
        "IO_URING_TCP_SEND_BATCH_BYTES = 1024 * 1024"
        "IO_URING_TCP_SEND_PROGRESS_BYTES = 64 * 1024"
		"IO_URING_TCP_RECEIVE_BYTES = PACKET_MAX_SIZE_TCP"
		"context->data.resize(IO_URING_TCP_RECEIVE_BYTES)"
		"if (!terminal && state->registeredRead"
        "pendingIter->result < 0"
        "tcpSendInlineCompletionCount_")
    string(FIND "${_io_uring}" "${_required}" _required_pos)
    if(_required_pos EQUAL -1)
        message(FATAL_ERROR "io_uring TCP send progress contract is missing: ${_required}")
    endif()
endforeach()

foreach(_required
        "IOCP_TCP_SEND_BATCH_BYTES = 1024 * 1024"
        "IOCP_TCP_SEND_PROGRESS_BYTES = 64 * 1024"
		"IOCP_TCP_RECEIVE_BYTES = PACKET_MAX_SIZE_TCP"
		"IOCP_TCP_SEND_PRIORITY_BURST_SIZE"
		"pendingTcpSendCompletions_"
		"pContext->data.resize(IOCP_TCP_RECEIVE_BYTES)"
		"if (!terminal && pState->registeredRead"
        "!pendingIter->success"
        "tcpSendInlineCompletionCount_")
    string(FIND "${_iocp}" "${_required}" _required_pos)
    if(_required_pos EQUAL -1)
        message(FATAL_ERROR "IOCP TCP send progress contract is missing: ${_required}")
    endif()
endforeach()

message(STATUS "COMPLETION_FAIRNESS_CONTRACT_PASS")
