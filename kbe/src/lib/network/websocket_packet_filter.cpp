/*
This source file is part of KBEngine
For the latest info, see http://www.kbengine.org/

Copyright (c) 2008-2018 KBEngine.

KBEngine is free software: you can redistribute it and/or modify
it under the terms of the GNU Lesser General Public License as published by
the Free Software Foundation, either version 3 of the License, or
(at your option) any later version.

KBEngine is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU Lesser General Public License for more details.
 
You should have received a copy of the GNU Lesser General Public License
along with KBEngine.  If not, see <http://www.gnu.org/licenses/>.
*/


#include "websocket_packet_filter.h"
#include "websocket_protocol.h"

#include "network/bundle.h"
#include "network/channel.h"
#include "network/tcp_packet.h"
#include "utf8cpp/utf8.h"
#include "network/network_interface.h"
#include "network/packet_receiver.h"

namespace KBEngine { 
namespace Network
{

namespace
{

// WebSocketProtocol 在返回非错误帧前已将 payload 限制为 65535；集中转换可保持分片状态字段的既有 int32 ABI。
// WebSocketProtocol caps payloads at 65535 before returning a non-error frame; centralizing the conversion preserves the existing int32 fragment-state ABI.
inline int32 payloadLengthToFragmentSize(uint64 payloadLength)
{
	KBE_ASSERT(payloadLength <= NETWORK_MESSAGE_MAX_SIZE);
	return static_cast<int32>(payloadLength);
}

}

//-------------------------------------------------------------------------------------
WebSocketPacketFilter::WebSocketPacketFilter(Channel* pChannel):
	pFragmentDatasRemain_(0),
	fragmentDatasFlag_(FRAGMENT_MESSAGE_HREAD),
	msg_opcode_(0),
	msg_fin_(0),
	msg_masked_(0),
	msg_mask_(0),
	msg_length_field_(0),
	msg_payload_length_(0),
	msg_frameType_(websocket::WebSocketProtocol::ERROR_FRAME),
	pChannel_(pChannel),
	pTCPPacket_(NULL)
{
}

//-------------------------------------------------------------------------------------
WebSocketPacketFilter::~WebSocketPacketFilter()
{
	reset();
}

//-------------------------------------------------------------------------------------
void WebSocketPacketFilter::reset()
{
	msg_opcode_ = 0;
	msg_fin_ = 0;
	msg_masked_ = 0;
	msg_mask_ = 0;
	msg_length_field_ = 0;
	msg_payload_length_ = 0;
	pFragmentDatasRemain_ = 0;
	fragmentDatasFlag_ = FRAGMENT_MESSAGE_HREAD;

	TCPPacket::reclaimPoolObject(pTCPPacket_);
	pTCPPacket_ = NULL;
}

//-------------------------------------------------------------------------------------
Reason WebSocketPacketFilter::send(Channel * pChannel, PacketSender& sender, Packet * pPacket, int userarg)
{
	if(pPacket->encrypted())
		return PacketFilter::send(pChannel, sender, pPacket, userarg);

	Bundle* pBundle = pPacket->pBundle();
	TCPPacket* pRetTCPPacket = TCPPacket::createPoolObject(OBJECTPOOL_POINT);
	websocket::WebSocketProtocol::FrameType frameType = websocket::WebSocketProtocol::BINARY_FRAME;

	if (pBundle)
	{
		Bundle::Packets& packs = pBundle->packets();

		if (packs.size() > 1)
		{
			bool isEnd = packs.back() == pPacket;
			bool isBegin = packs.front() == pPacket;

			if (!isEnd && !isBegin)
			{
				frameType = websocket::WebSocketProtocol::NEXT_FRAME;
			}
			else
			{
				if (!isEnd)
				{
					frameType = websocket::WebSocketProtocol::INCOMPLETE_BINARY_FRAME;
				}
				else
				{
					frameType = websocket::WebSocketProtocol::END_FRAME;
				}
			}
		}
	}

	websocket::WebSocketProtocol::makeFrame(frameType, pPacket, pRetTCPPacket);

	if(pPacket->length() > pRetTCPPacket->space())
	{
		const size_t additionalSpace = pPacket->length() - pRetTCPPacket->space();
		WARNING_MSG(fmt::format("WebSocketPacketFilter::send: no free space, buffer added:{}, total={}.\n",
			additionalSpace, pRetTCPPacket->size()));

		pRetTCPPacket->data_resize(pRetTCPPacket->size() + additionalSpace);
	}

	(*pRetTCPPacket).append(pPacket->data() + pPacket->rpos(), pPacket->length());
	pRetTCPPacket->swap(*(static_cast<KBEngine::MemoryStream*>(pPacket)));
	TCPPacket::reclaimPoolObject(pRetTCPPacket);

	pPacket->encrypted(true);
	return PacketFilter::send(pChannel, sender, pPacket, userarg);
}

//-------------------------------------------------------------------------------------
Reason WebSocketPacketFilter::recv(Channel * pChannel, PacketReceiver & receiver, Packet * pPacket)
{
	while(pPacket->length() > 0)
	{
		if(fragmentDatasFlag_ == FRAGMENT_MESSAGE_HREAD)
		{
			if(pFragmentDatasRemain_ == 0)
			{
				KBE_ASSERT(pTCPPacket_ == NULL);

				size_t rpos = pPacket->rpos();

				reset();

				// 如果没有创建过缓存，先尝试直接解析包头，如果信息足够成功解析则继续到下一步
				pFragmentDatasRemain_ = websocket::WebSocketProtocol::getFrame(pPacket, msg_opcode_, msg_fin_, msg_masked_, 
					msg_mask_, msg_length_field_, msg_payload_length_, msg_frameType_);

				if(pFragmentDatasRemain_ > 0)
				{
					pPacket->rpos(rpos);
					pTCPPacket_ = TCPPacket::createPoolObject(OBJECTPOOL_POINT);
					pTCPPacket_->append(*(static_cast<MemoryStream*>(pPacket)));
					pPacket->done();
				}
				else if(msg_frameType_ != websocket::WebSocketProtocol::ERROR_FRAME)
				{
					fragmentDatasFlag_ = FRAGMENT_MESSAGE_DATAS;
					pFragmentDatasRemain_ = payloadLengthToFragmentSize(msg_payload_length_);
				}
			}
			else
			{
				KBE_ASSERT(pTCPPacket_ != NULL);

				// 长度如果大于剩余读取长度，那么可以开始解析了
				// 否则将包内存继续缓存
				if(pPacket->length() >= static_cast<size_t>(pFragmentDatasRemain_))
				{
					size_t wpos = pPacket->wpos();
					size_t rpos = pPacket->rpos();

					pPacket->wpos(rpos + pFragmentDatasRemain_);

					// 首先将需要的数据添加到pTCPPacket_
					pTCPPacket_->append(*(static_cast<MemoryStream*>(pPacket)));
					
					// 将写位置还原回去
					pPacket->wpos(wpos);
					
					// 丢弃已经读取的数据
					pPacket->read_skip(pFragmentDatasRemain_);
					
					size_t buffer_rpos = pTCPPacket_->rpos();
					pFragmentDatasRemain_ = websocket::WebSocketProtocol::getFrame(pTCPPacket_, msg_opcode_, msg_fin_, msg_masked_, 
						msg_mask_, msg_length_field_, msg_payload_length_, msg_frameType_);

					// 如果仍然大于0， 说明需要继续收包
					if(pFragmentDatasRemain_ > 0)
					{
						// 由于一次没有解析完， 我们回撤数据下一次再尝试解析
						pTCPPacket_->rpos(buffer_rpos);

						// 当前包如果还有数据并且大于等于我们需要的数据，则继续下一循环立即解析
						if (pPacket->length() >= static_cast<size_t>(pFragmentDatasRemain_))
							continue;
					}
					else
					{
						// frame解析完毕，将对象回收
						TCPPacket::reclaimPoolObject(pTCPPacket_);
						pTCPPacket_ = NULL;

						// 是否有数据携带？如果没有则不进入data解析
						if(msg_frameType_ != websocket::WebSocketProtocol::ERROR_FRAME && msg_payload_length_ > 0)
						{
							fragmentDatasFlag_ = FRAGMENT_MESSAGE_DATAS;
							pFragmentDatasRemain_ = payloadLengthToFragmentSize(msg_payload_length_);
						}
					}
				}
				else
				{
					pTCPPacket_->append(*(static_cast<MemoryStream*>(pPacket)));
					pFragmentDatasRemain_ -= static_cast<int32>(pPacket->length());

					pPacket->done();
				}
			}

			if(websocket::WebSocketProtocol::ERROR_FRAME == msg_frameType_)
			{
				ERROR_MSG(fmt::format("WebSocketPacketFilter::recv: frame error! addr={}!\n",
					pChannel_->c_str()));

				this->pChannel_->condemn("WebSocketPacketFilter::recv: frame error!");
				reset();

				TCPPacket::reclaimPoolObject(static_cast<TCPPacket*>(pPacket));
				return REASON_WEBSOCKET_ERROR;
			}
			else if (msg_frameType_ == websocket::WebSocketProtocol::TEXT_FRAME ||
				msg_frameType_ == websocket::WebSocketProtocol::INCOMPLETE_TEXT_FRAME ||
				msg_frameType_ == websocket::WebSocketProtocol::PONG_FRAME)
			{
				ERROR_MSG(fmt::format("WebSocketPacketFilter::recv: Does not support FRAME_TYPE({})! addr={}!\n",
					websocket::WebSocketProtocol::getFrameTypeName(msg_frameType_), pChannel_->c_str()));

				this->pChannel_->condemn("WebSocketPacketFilter::recv: Does not support FRAME_TYPE");
				reset();

				TCPPacket::reclaimPoolObject(static_cast<TCPPacket*>(pPacket));
				return REASON_WEBSOCKET_ERROR;
			}
			else if(msg_frameType_ == websocket::WebSocketProtocol::CLOSE_FRAME && msg_payload_length_ == 0)
			{
				Reason reason = onClose(pChannel, NULL);
				reset();

				TCPPacket::reclaimPoolObject(static_cast<TCPPacket*>(pPacket));
				return reason;
			}
			else if(msg_frameType_ == websocket::WebSocketProtocol::INCOMPLETE_FRAME)
			{
				// 继续等待后续内容到达
			}
			else if (msg_frameType_ == websocket::WebSocketProtocol::PING_FRAME)
			{
				if (pFragmentDatasRemain_ <= 0)
				{
					Reason reason = onPing(pChannel, pPacket);
					if (reason != REASON_SUCCESS)
					{
						reset();

						TCPPacket::reclaimPoolObject(static_cast<TCPPacket*>(pPacket));
						return reason;
					}
				}

				continue;
			}
		}
		else
		{
			if (pFragmentDatasRemain_ <= 0)
			{
				ERROR_MSG(fmt::format("WebSocketPacketFilter::recv: pFragmentDatasRemain_ <= 0! addr={}!\n",
					pChannel_->c_str()));

				this->pChannel_->condemn("WebSocketPacketFilter::recv: pFragmentDatasRemain_ <= 0!");
				reset();

				TCPPacket::reclaimPoolObject(static_cast<TCPPacket*>(pPacket));
				return REASON_WEBSOCKET_ERROR;
			}

			if(pTCPPacket_ == NULL)
				pTCPPacket_ = TCPPacket::createPoolObject(OBJECTPOOL_POINT);

			if(static_cast<size_t>(pFragmentDatasRemain_) <= pPacket->length())
			{
				pTCPPacket_->append(pPacket->data() + pPacket->rpos(), pFragmentDatasRemain_);
				pPacket->read_skip((size_t)pFragmentDatasRemain_);
				pFragmentDatasRemain_ = 0;
			}
			else
			{
				pTCPPacket_->append(*(static_cast<MemoryStream*>(pPacket)));
				pFragmentDatasRemain_ -= static_cast<int32>(pPacket->length());
				pPacket->done();
			}

			Reason reason = REASON_SUCCESS;

			if (msg_frameType_ == websocket::WebSocketProtocol::PING_FRAME ||
				msg_frameType_ == websocket::WebSocketProtocol::CLOSE_FRAME)
			{
				// 继续等剩余的内容到来为止
				if (pFragmentDatasRemain_ > 0)
					continue;

				if (!websocket::WebSocketProtocol::decodingDatas(pTCPPacket_, msg_masked_, msg_mask_))
				{
					ERROR_MSG(fmt::format("WebSocketPacketFilter::recv: decoding-frame error! addr={}!\n",
						pChannel_->c_str()));

					this->pChannel_->condemn("WebSocketPacketFilter::recv: decoding-frame error!");
					reset();

					TCPPacket::reclaimPoolObject(static_cast<TCPPacket*>(pPacket));
					return REASON_WEBSOCKET_ERROR;
				}

				reason = msg_frameType_ == websocket::WebSocketProtocol::PING_FRAME ?
					onPing(pChannel, pTCPPacket_) : onClose(pChannel, pTCPPacket_);
			}
			else
			{
				if (!websocket::WebSocketProtocol::decodingDatas(pTCPPacket_, msg_masked_, msg_mask_))
				{
					ERROR_MSG(fmt::format("WebSocketPacketFilter::recv: decoding-frame error! addr={}!\n",
						pChannel_->c_str()));

					this->pChannel_->condemn("WebSocketPacketFilter::recv: decoding-frame error!");
					reset();

					TCPPacket::reclaimPoolObject(static_cast<TCPPacket*>(pPacket));
					return REASON_WEBSOCKET_ERROR;
				}

				reason = PacketFilter::recv(pChannel, receiver, pTCPPacket_);
				KBE_ASSERT(reason == REASON_SUCCESS);

				// pTCPPacket_不需要在这里回收了
				pTCPPacket_ = NULL;
			}

			if(pFragmentDatasRemain_ == 0)
				reset();

			if (reason != REASON_SUCCESS)
			{
				TCPPacket::reclaimPoolObject(static_cast<TCPPacket*>(pPacket));
				reset();
				return reason;
			}
		}
	}

	TCPPacket::reclaimPoolObject(static_cast<TCPPacket*>(pPacket));
	return REASON_SUCCESS;
}

//-------------------------------------------------------------------------------------
Reason WebSocketPacketFilter::onPing(Channel * pChannel, Packet* pPacket)
{
	KBE_ASSERT(pFragmentDatasRemain_ == 0);

	TCPPacket* pPongPacket = TCPPacket::createPoolObject(OBJECTPOOL_POINT);
	websocket::WebSocketProtocol::makeFrame(websocket::WebSocketProtocol::PONG_FRAME, pPacket, pPongPacket);

	if (msg_payload_length_ > 0)
	{
		pPongPacket->append(pPacket->data() + pPacket->rpos(), msg_payload_length_);
		pPacket->read_skip((size_t)msg_payload_length_);
	}

	// RFC 6455 控制帧 payload 最大 125 字节，连同帧头也远小于底层 TLS API 的 int 上限。
	// RFC 6455 control-frame payloads are capped at 125 bytes, so the framed packet remains far below the TLS API's int limit.
	KBE_ASSERT(pPongPacket->length() <= NETWORK_MESSAGE_MAX_SIZE);
	int sendSize = static_cast<int>(pPongPacket->length());
	if (pChannel->pEndPoint()->usesSSLMemoryBIO())
	{
		// pong 已经是完整 WebSocket 控制帧，只需进行 TLS 编码并交给 IOCP，不能再经过 WebSocketPacketFilter 二次封帧。
		// The pong is already a complete WebSocket control frame; encode it as TLS and hand it to IOCP without framing it a second time.
		if (!pChannel->pEndPoint()->encryptSSLNetworkData(pPongPacket->data(), sendSize) ||
			!pChannel->flushSSLNetworkOutput())
		{
			ERROR_MSG(fmt::format("WebSocketPacketFilter::recv: queue encrypted pong-frame failed! addr={}, sendSize={}\n",
				pChannel_->c_str(), sendSize));
			pChannel->condemn("WebSocket encrypted pong send failed");
		}
	}
	else
	{
		while (sendSize > 0)
		{
			int ret = pChannel->pEndPoint()->send(pPongPacket->data() + (pPongPacket->length() - sendSize), sendSize);
			if (ret <= 0)
			{
				ERROR_MSG(fmt::format("WebSocketPacketFilter::recv: send({}) pong-frame error! addr={}, sendSize={}\n",
					ret, pChannel_->c_str(), sendSize));

				break;
			}

			sendSize -= ret;
		}
	}

	TCPPacket::reclaimPoolObject(pPongPacket);

	pFragmentDatasRemain_ = 0;
	fragmentDatasFlag_ = FRAGMENT_MESSAGE_HREAD;
	return REASON_SUCCESS;
}

//-------------------------------------------------------------------------------------
Reason WebSocketPacketFilter::onClose(Channel * pChannel, Packet* pPacket)
{
	const size_t length = pPacket ? pPacket->length() : 0;
	const uint8* payload = pPacket ? pPacket->data() + pPacket->rpos() : NULL;
	uint16 responseCode = 1000;
	bool valid = msg_fin_ == 1 && msg_masked_ == 1 && length <= 125 && length != 1;

	if (valid && length >= 2)
	{
		const uint16 closeCode = static_cast<uint16>((payload[0] << 8) | payload[1]);
		// 1016..2999 由协议保留，未协商扩展时不能接受；3000 以上分别供框架和应用使用。
		// Codes 1016..2999 are protocol-reserved and invalid without an extension; 3000+ are available to frameworks and applications.
		valid = ((closeCode >= 1000 && closeCode <= 1014) || (closeCode >= 3000 && closeCode <= 4999)) &&
			closeCode != 1004 && closeCode != 1005 && closeCode != 1006 && closeCode != 1015;
		responseCode = valid ? closeCode : 1002;

		bool reasonIsValidUtf8 = true;
		if (valid && length > 2)
		{
			// utf8cpp 旧版内部把迭代器差值传给 int；输入最多 123 字节，局部屏蔽不会隐藏本项目的宽度问题。
			// Old utf8cpp passes iterator differences to int; input is at most 123 bytes, so local suppression cannot hide project width issues.
#if KBE_PLATFORM == PLATFORM_WIN32
#pragma warning(push)
#pragma warning(disable: 4244)
#endif
			reasonIsValidUtf8 = utf8::is_valid(payload + 2, payload + length);
#if KBE_PLATFORM == PLATFORM_WIN32
#pragma warning(pop)
#endif
		}

		if (!reasonIsValidUtf8)
		{
			valid = false;
			responseCode = 1007;
		}
	}
	else if (!valid)
	{
		responseCode = 1002;
	}

	bool started = false;
	if (valid)
	{
		started = pChannel->handleWebSocketClose(payload, length);
	}
	else
	{
		// 无效 close 不能原样回显；1002 表示协议错误，1007 表示 reason 不是合法 UTF-8。
		// An invalid close cannot be echoed; 1002 reports protocol error and 1007 reports an invalid UTF-8 reason.
		started = pChannel->handleWebSocketCloseError(responseCode);
	}

	if (!started)
	{
		pChannel->condemn("WebSocket close response failed");
		return REASON_GENERAL_NETWORK;
	}

	return REASON_SUCCESS;
}

//-------------------------------------------------------------------------------------
} 
}
