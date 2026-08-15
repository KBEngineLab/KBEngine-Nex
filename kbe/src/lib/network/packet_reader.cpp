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

#include "packet_reader.h"
#include "network/channel.h"
#include "network/message_handler.h"
#include "network/network_stats.h"
#include "common/timestamp.h"

namespace
{
std::string formatPacketByteWindow(const KBEngine::MemoryStream& stream, size_t focus,
	size_t bytesBefore, size_t bytesAfter, size_t& windowStart, size_t& windowEnd)
{
	const size_t streamEnd = stream.wpos();
	if(focus > streamEnd)
		focus = streamEnd;

	windowStart = focus > bytesBefore ? focus - bytesBefore : 0;
	windowEnd = streamEnd - focus > bytesAfter ? focus + bytesAfter : streamEnd;

	static const char HEX_DIGITS[] = "0123456789abcdef";
	std::string result;
	result.reserve((windowEnd - windowStart) * 3);
	const KBEngine::uint8* bytes = stream.data();
	for(size_t i = windowStart; i < windowEnd; ++i)
	{
		if(i > windowStart)
			result.push_back(' ');

		result.push_back(HEX_DIGITS[(bytes[i] >> 4) & 0x0f]);
		result.push_back(HEX_DIGITS[bytes[i] & 0x0f]);
	}

	return result;
}

class ScopedMessageProcessingSample
{
public:
	explicit ScopedMessageProcessingSample(KBEngine::Network::MessageHandler& handler) :
		metrics_(KBEngine::Network::MessageHandlers::processingMetrics()),
		category_(handler.processingCategory),
		handlerID_(handler.msgID),
		handlerName_(handler.name),
		sampled_(metrics_.beginCall(category_)),
		started_(sampled_ ? KBEngine::timestamp() : 0)
	{
	}

	~ScopedMessageProcessingSample()
	{
		if (!sampled_)
			return;

		const std::uint64_t elapsed = KBEngine::timestamp() - started_;
		const std::uint64_t nanos = static_cast<std::uint64_t>(
			static_cast<double>(elapsed) * 1000000000.0 / KBEngine::stampsPerSecondD());
		metrics_.recordSample(category_, nanos, handlerID_, handlerName_);
	}

private:
	KBEngine::Network::MessageProcessingMetrics& metrics_;
	KBEngine::Network::MessageProcessingCategory category_;
	KBEngine::Network::MessageID handlerID_;
	const std::string& handlerName_;
	bool sampled_;
	std::uint64_t started_;
};
}

namespace KBEngine {
namespace Network
{


//-------------------------------------------------------------------------------------
PacketReader::PacketReader(Channel* pChannel):
	pFragmentDatas_(NULL),
	pFragmentDatasWpos_(0),
	pFragmentDatasRemain_(0),
	fragmentDatasFlag_(FRAGMENT_DATA_UNKNOW),
	pFragmentStream_(NULL),
	currMsgID_(0),
	currMsgLen_(0),
	currMsgIDFragmented_(false),
	lastCompletedMsgID_(0),
	lastCompletedMsgLen_(0),
	pChannel_(pChannel)
{
}

//-------------------------------------------------------------------------------------
PacketReader::~PacketReader()
{
	reset();
	pChannel_ = NULL;
}

//-------------------------------------------------------------------------------------
void PacketReader::reset()
{
	fragmentDatasFlag_ = FRAGMENT_DATA_UNKNOW;
	pFragmentDatasWpos_ = 0;
	pFragmentDatasRemain_ = 0;
	currMsgID_ = 0;
	currMsgLen_ = 0;
	currMsgIDFragmented_ = false;
	lastCompletedMsgID_ = 0;
	lastCompletedMsgLen_ = 0;
	
	SAFE_RELEASE_ARRAY(pFragmentDatas_);
	MemoryStream::reclaimPoolObject(pFragmentStream_);
	pFragmentStream_ = NULL;
}

//-------------------------------------------------------------------------------------
void PacketReader::processMessages(KBEngine::Network::MessageHandlers* pMsgHandlers, Packet* pPacket)
{
	while(pPacket->length() > 0 || pFragmentStream_ != NULL)
	{
		if(fragmentDatasFlag_ == FRAGMENT_DATA_UNKNOW)
		{
			// 如果没有ID信息，先获取ID
			if(currMsgID_ == 0)
			{
				if(NETWORK_MESSAGE_ID_SIZE > 1 && pPacket->length() < NETWORK_MESSAGE_ID_SIZE)
				{
					writeFragmentMessage(FRAGMENT_DATA_MESSAGE_ID, pPacket, NETWORK_MESSAGE_ID_SIZE);
					break;
				}

				(*pPacket) >> currMsgID_;
				currMsgIDFragmented_ = false;
				pPacket->messageID(currMsgID_);
			}

			Network::MessageHandler* pMsgHandler = pMsgHandlers->find(currMsgID_);

			if(pMsgHandler == NULL)
			{
				MemoryStream* pPacket1 = pFragmentStream_ != NULL ? pFragmentStream_ : pPacket;
				TRACE_MESSAGE_PACKET(true, pPacket1, pMsgHandler, pPacket1->length(), pChannel_->c_str(), false);
				
				// 用作调试时比对
				size_t rpos = pPacket1->rpos();
				pPacket1->rpos(0);
				TRACE_MESSAGE_PACKET(true, pPacket1, pMsgHandler, pPacket1->length(), pChannel_->c_str(), false);
				pPacket1->rpos(rpos);

				Network::MessageHandler* pLastCompletedHandler =
					lastCompletedMsgID_ != 0 ? pMsgHandlers->find(lastCompletedMsgID_) : NULL;
				// 仅在致命协议错误时生成有限窗口，正常收包路径不承担格式化和内存分配开销。
				// Build a bounded window only for fatal protocol errors, keeping formatting and allocation off the normal receive path.
				const size_t packetReadPos = pPacket->rpos();
				const size_t messageIDOffset = !currMsgIDFragmented_ && packetReadPos >= NETWORK_MESSAGE_ID_SIZE ?
					packetReadPos - NETWORK_MESSAGE_ID_SIZE : packetReadPos;
				size_t byteWindowStart = 0;
				size_t byteWindowEnd = 0;
				const std::string byteWindow = formatPacketByteWindow(*pPacket, messageIDOffset,
					32, 64, byteWindowStart, byteWindowEnd);
				ERROR_MSG(fmt::format("PacketReader::processMessages: not found msgID={}, msglen={}, "
					"lastCompletedMsgID={}, lastCompletedMsgName={}, lastCompletedMsgLen={}, "
					"messageIDFragmented={}, packetReadPos={}, byteWindow=[{},{}), messageIDOffset={}, bytes={}, from {}.\n",
					currMsgID_, pPacket1->length(), lastCompletedMsgID_,
					pLastCompletedHandler != NULL ? pLastCompletedHandler->name : std::string("none"),
					lastCompletedMsgLen_, currMsgIDFragmented_, packetReadPos, byteWindowStart,
					byteWindowEnd, messageIDOffset, byteWindow, pChannel_->c_str()));

				currMsgID_ = 0;
				currMsgLen_ = 0;
				currMsgIDFragmented_ = false;
				pChannel_->condemn("PacketReader::processMessages: not found msgID");
				break;
			}

			// 如果没有可操作的数据了则退出等待下一个包处理。
			// 可能是一个无参数数据包
			//if(pPacket->opsize() == 0)	
			//	break;
			
			// 如果长度信息没有获得，则等待获取长度信息
			if(currMsgLen_ == 0)
			{
				// 如果长度信息是可变的或者配置了永远包含长度信息选项时，从流中分析长度数据
				if(pMsgHandler->msgLen == NETWORK_VARIABLE_MESSAGE)
				{
					// 如果长度信息不完整，则等待下一个包处理
					if(pPacket->length() < NETWORK_MESSAGE_LENGTH_SIZE)
					{
						writeFragmentMessage(FRAGMENT_DATA_MESSAGE_LENGTH, pPacket, NETWORK_MESSAGE_LENGTH_SIZE);
						break;
					}
					else
					{
						// 此处获得了长度信息
						Network::MessageLength currlen;
						(*pPacket) >> currlen;
						currMsgLen_ = currlen;

						NetworkStats::getSingleton().trackMessage(NetworkStats::RECV, *pMsgHandler, 
							currMsgLen_ + NETWORK_MESSAGE_ID_SIZE + NETWORK_MESSAGE_LENGTH_SIZE);

						if (currMsgLen_ == NETWORK_MESSAGE_MAX_SIZE)
							currMsgLen_ = NETWORK_MESSAGE_MAX_SIZE1;
					}
				}
				else
				{
					currMsgLen_ = pMsgHandler->msgLen;

					NetworkStats::getSingleton().trackMessage(NetworkStats::RECV, *pMsgHandler, 
						currMsgLen_ + NETWORK_MESSAGE_LENGTH_SIZE);
				}
			}

			// 如果长度占满说明使用了扩展长度，我们还需要等待扩展长度信息
			if (currMsgLen_ == NETWORK_MESSAGE_MAX_SIZE1)
			{
				if (pPacket->length() < NETWORK_MESSAGE_LENGTH1_SIZE)
				{
					// 如果长度信息不完整，则等待下一个包处理
					writeFragmentMessage(FRAGMENT_DATA_MESSAGE_LENGTH1, pPacket, NETWORK_MESSAGE_LENGTH1_SIZE);
					break;
				}
				else
				{
					// 此处获得了扩展长度信息
					(*pPacket) >> currMsgLen_;

					NetworkStats::getSingleton().trackMessage(NetworkStats::RECV, *pMsgHandler,
						currMsgLen_ + NETWORK_MESSAGE_ID_SIZE + NETWORK_MESSAGE_LENGTH1_SIZE);
				}
			}

			if(this->pChannel_->isExternal() && 
				g_componentType != BOTS_TYPE && 
				g_componentType != CLIENT_TYPE && 
				currMsgLen_ > NETWORK_MESSAGE_MAX_SIZE)
			{
				MemoryStream* pPacket1 = pFragmentStream_ != NULL ? pFragmentStream_ : pPacket;
				TRACE_MESSAGE_PACKET(true, pPacket1, pMsgHandler, pPacket1->length(), pChannel_->c_str(), false);

				// 用作调试时比对
				size_t rpos = pPacket1->rpos();
				pPacket1->rpos(0);
				TRACE_MESSAGE_PACKET(true, pPacket1, pMsgHandler, pPacket1->length(), pChannel_->c_str(), false);
				pPacket1->rpos(rpos);

				WARNING_MSG(fmt::format("PacketReader::processMessages({0}): msglen exceeds the limit! msgID={1}, msglen=({2}:{3}), maxlen={5}, from {4}.\n", 
					pMsgHandler->name.c_str(), currMsgID_, currMsgLen_, pPacket1->length(), pChannel_->c_str(), NETWORK_MESSAGE_MAX_SIZE));

				currMsgLen_ = 0;
				pChannel_->condemn("PacketReader::processMessages: msglen exceeds the limit!");
				break;
			}

			if(pFragmentStream_ != NULL)
			{
				TRACE_MESSAGE_PACKET(true, pFragmentStream_, pMsgHandler, currMsgLen_, pChannel_->c_str(), false);
				{
					ScopedMessageProcessingSample processingSample(*pMsgHandler);
					pMsgHandler->handle(pChannel_, *pFragmentStream_);
				}
				MemoryStream::reclaimPoolObject(pFragmentStream_);
				pFragmentStream_ = NULL;
			}
			else
			{
				if(pPacket->length() < currMsgLen_)
				{
					writeFragmentMessage(FRAGMENT_DATA_MESSAGE_BODY, pPacket, currMsgLen_);
					break;
				}

				// 临时设置有效读取位， 防止接口中溢出操作
				size_t wpos = pPacket->wpos();
				// size_t rpos = pPacket->rpos();
				size_t frpos = pPacket->rpos() + currMsgLen_;
				pPacket->wpos(frpos);

				TRACE_MESSAGE_PACKET(true, pPacket, pMsgHandler, currMsgLen_, pChannel_->c_str(), true);
				{
					ScopedMessageProcessingSample processingSample(*pMsgHandler);
					pMsgHandler->handle(pChannel_, *pPacket);
				}

				// 如果handler没有处理完数据则输出一个警告
				if(currMsgLen_ > 0)
				{
					if(frpos != pPacket->rpos())
					{
						WARNING_MSG(fmt::format("PacketReader::processMessages({}): rpos({}) invalid, expect={}. msgID={}, msglen={}.\n",
							pMsgHandler->name.c_str(), pPacket->rpos(), frpos, currMsgID_, currMsgLen_));

						pPacket->rpos(frpos);
					}
				}

				pPacket->wpos(wpos);
			}

			lastCompletedMsgID_ = currMsgID_;
			lastCompletedMsgLen_ = currMsgLen_;

			currMsgID_ = 0;
			currMsgLen_ = 0;
			currMsgIDFragmented_ = false;
		}
		else
		{
			mergeFragmentMessage(pPacket);
		}
	}
}

//-------------------------------------------------------------------------------------
void PacketReader::writeFragmentMessage(FragmentDataTypes fragmentDatasFlag, Packet* pPacket, uint32 datasize)
{
	KBE_ASSERT(pFragmentDatas_ == NULL);

	size_t opsize = pPacket->length();
	// 调用方只在当前包不足以填满协议字段时进入，因此转换前已经证明长度可由uint32表示。
	// Callers enter only when the packet cannot fill the protocol field, so the length is proven to fit uint32 before conversion.
	KBE_ASSERT(opsize < static_cast<size_t>(datasize));
	const uint32 fragmentSize = static_cast<uint32>(opsize);
	pFragmentDatasRemain_ = datasize - fragmentSize;
	pFragmentDatas_ = new uint8[opsize + pFragmentDatasRemain_ + 1];

	fragmentDatasFlag_ = fragmentDatasFlag;
	pFragmentDatasWpos_ = fragmentSize;

	if(pPacket->length() > 0)
	{
		memcpy(pFragmentDatas_, pPacket->data() + pPacket->rpos(), opsize);
		pPacket->done();
	}

	//DEBUG_MSG(fmt::format("PacketReader::writeFragmentMessage({}): channel[{:p}], fragmentDatasFlag={}, remainsize={}, currMsgID={}, currMsgLen={}.\n", 
	//	pChannel_->c_str(), (void*)pChannel_, fragmentDatasFlag, pFragmentDatasRemain_, currMsgID_, currMsgLen_));
}

//-------------------------------------------------------------------------------------
void PacketReader::mergeFragmentMessage(Packet* pPacket)
{
	size_t opsize = pPacket->length();
	if(opsize == 0)
		return;

	if(pPacket->length() >= pFragmentDatasRemain_)
	{
		memcpy(pFragmentDatas_ + pFragmentDatasWpos_, pPacket->data() + pPacket->rpos(), pFragmentDatasRemain_);
		pPacket->rpos(pPacket->rpos() + pFragmentDatasRemain_);

		KBE_ASSERT(pFragmentStream_ == NULL);

		switch(fragmentDatasFlag_)
		{
		case FRAGMENT_DATA_MESSAGE_ID:			// 消息ID信息不全
			memcpy(&currMsgID_, pFragmentDatas_, NETWORK_MESSAGE_ID_SIZE);
			currMsgIDFragmented_ = true;
			break;

		case FRAGMENT_DATA_MESSAGE_LENGTH:		// 消息长度信息不全
			memcpy(&currMsgLen_, pFragmentDatas_, NETWORK_MESSAGE_LENGTH_SIZE);
			if (currMsgLen_ == NETWORK_MESSAGE_MAX_SIZE) 
				currMsgLen_ = NETWORK_MESSAGE_MAX_SIZE1;
			break;

		case FRAGMENT_DATA_MESSAGE_LENGTH1:		// 消息长度信息不全
			memcpy(&currMsgLen_, pFragmentDatas_, NETWORK_MESSAGE_LENGTH1_SIZE);
			if (currMsgLen_ == NETWORK_MESSAGE_MAX_SIZE1) 
				pChannel_->condemn("PacketReader::mergeFragmentMessage: msglen1 exceeds the limit!");
			break;

		case FRAGMENT_DATA_MESSAGE_BODY:		// 消息内容信息不全
			pFragmentStream_ = MemoryStream::createPoolObject(OBJECTPOOL_POINT);
			pFragmentStream_->append(pFragmentDatas_, currMsgLen_);
			break;

		default:
			break;
		};

		//DEBUG_MSG(fmt::format("PacketReader::mergeFragmentMessage({}): channel[{:p}], fragmentDatasFlag={}, currMsgID={}, currMsgLen={}, completed!\n", 
		//	pChannel_->c_str(), (void*)pChannel_, fragmentDatasFlag_, currMsgID_, currMsgLen_));

		fragmentDatasFlag_ = FRAGMENT_DATA_UNKNOW;
		pFragmentDatasRemain_ = 0;
		SAFE_RELEASE_ARRAY(pFragmentDatas_);
	}
	else
	{
		memcpy(pFragmentDatas_ + pFragmentDatasWpos_, pPacket->data(), opsize);
		// 当前分支已证明opsize小于32位剩余长度，单次转换后同时更新剩余量和写入位置。
		// This branch proves opsize is below the 32-bit remainder; convert once to update both counters consistently.
		const uint32 fragmentSize = static_cast<uint32>(opsize);
		pFragmentDatasRemain_ -= fragmentSize;
		pFragmentDatasWpos_ += fragmentSize;
		pPacket->rpos(pPacket->rpos() + opsize);

		//DEBUG_MSG(fmt::format("PacketReader::mergeFragmentMessage({}): channel[{:p}], fragmentDatasFlag={}, remainsize={}, currMsgID={}, currMsgLen={}.\n",
		//	pChannel_->c_str(), (void*)pChannel_, fragmentDatasFlag_, pFragmentDatasRemain_, currMsgID_, currMsgLen_));
	}	
}

//-------------------------------------------------------------------------------------
} 
}
