/*
This source file is part of KBEngine
For the latest info, see http://www.kbengine.org/
*/

#ifndef KBE_SERVER_BOUNDED_STREAM_READER_H
#define KBE_SERVER_BOUNDED_STREAM_READER_H

#include "common/common.h"
#include "common/memorystream.h"

#include <cstring>

namespace KBEngine{

class BoundedStreamReader
{
public:
	explicit BoundedStreamReader(const MemoryStream& stream):
	data_(stream.length() > 0 ? stream.data() + stream.rpos() : NULL),
	remaining_(stream.length())
	{
	}

	bool skip(size_t count)
	{
		if (count > remaining_)
			return false;
		if (count == 0)
			return true;

		data_ += count;
		remaining_ -= count;
		return true;
	}

	template <typename T> bool read(T& value)
	{
		if (remaining_ < sizeof(T))
			return false;

		std::memcpy(&value, data_, sizeof(T));
		EndianConvert(value);
		return skip(sizeof(T));
	}

	bool skipString(size_t maximumLength, bool allowEmpty)
	{
		for (size_t length = 0; length < remaining_; ++length)
		{
			const uint8 value = data_[length];
			if (value == 0)
			{
				if ((!allowEmpty && length == 0) || length > maximumLength)
					return false;

				return skip(length + 1);
			}

			// MemoryStream strings are ASCII and NUL terminated. Rejecting other bytes
			// here prevents the normal extractor from treating one byte as an implicit terminator.
			// MemoryStream 字符串使用 ASCII 与 NUL 结尾；提前拒绝其他字节，避免普通提取器把它误作隐式终止符。
			if (value > 0x7f || length >= maximumLength)
				return false;
		}

		return false;
	}

	bool skipBlob(size_t maximumLength)
	{
		ArraySize wireLength = 0;
		if (!read(wireLength) || wireLength > maximumLength)
			return false;

		return skip(static_cast<size_t>(wireLength));
	}

	bool empty() const { return remaining_ == 0; }

private:
	const uint8* data_;
	size_t remaining_;
};

}

#endif // KBE_SERVER_BOUNDED_STREAM_READER_H
