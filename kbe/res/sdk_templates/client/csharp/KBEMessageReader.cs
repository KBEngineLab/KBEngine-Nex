namespace KBEngine
{
	using System;

	using MessageID = System.UInt16;

	// 无论字节来自 TCP、KCP 还是自定义 Provider，KBE 协议消息都按同一个有序字节流解析。
	// KBE protocol messages form one ordered byte stream regardless of whether TCP, KCP, or a custom provider supplies the bytes.
	public sealed class KBEMessageReader
	{
		private enum ReadState
		{
			MessageId,
			MessageLength,
			ExtendedMessageLength,
			Body,
		}

		private readonly int _maximumMessageSize;
		private readonly byte[] _header = new byte[sizeof(UInt32)];
		private readonly MemoryStream _body = new MemoryStream();
		private ReadState _state;
		private int _headerCount;
		private int _expectedBodySize;
		private MessageID _messageId;
		private Message _message;

		public KBEMessageReader(int maximumMessageSize)
		{
			if (maximumMessageSize <= 0)
				throw new ArgumentOutOfRangeException(nameof(maximumMessageSize));

			_maximumMessageSize = maximumMessageSize;
			Reset();
		}

		public void Process(byte[] data, int offset, int count)
		{
			if (data == null)
				throw new ArgumentNullException(nameof(data));
			if (offset < 0 || count < 0 || offset > data.Length - count)
				throw new ArgumentOutOfRangeException(nameof(offset));

			int end = offset + count;
			while (offset < end)
			{
				switch (_state)
				{
					case ReadState.MessageId:
						ReadHeader(data, ref offset, end, sizeof(UInt16));
						if (_headerCount == sizeof(UInt16))
							ReadMessageId();
						break;

					case ReadState.MessageLength:
						ReadHeader(data, ref offset, end, sizeof(UInt16));
						if (_headerCount == sizeof(UInt16))
							ReadMessageLength();
						break;

					case ReadState.ExtendedMessageLength:
						ReadHeader(data, ref offset, end, sizeof(UInt32));
						if (_headerCount == sizeof(UInt32))
							BeginBody(ReadUInt32(_header, 0));
						break;

					case ReadState.Body:
						int needed = _expectedBodySize - checked((int)_body.length());
						int available = end - offset;
						int copyCount = Math.Min(needed, available);
						if (copyCount > 0)
						{
							_body.append(data, checked((UInt32)offset), checked((UInt32)copyCount));
							offset += copyCount;
						}

						if (_body.length() == (UInt32)_expectedBodySize)
							DispatchMessage();
						break;

					default:
						throw new InvalidOperationException("Unsupported message reader state.");
				}
			}
		}

		public void Reset()
		{
			_state = ReadState.MessageId;
			_headerCount = 0;
			_expectedBodySize = 0;
			_messageId = 0;
			_message = null;
			_body.clear();
		}

		private void ReadHeader(byte[] data, ref int offset, int end, int expectedSize)
		{
			int copyCount = Math.Min(expectedSize - _headerCount, end - offset);
			Buffer.BlockCopy(data, offset, _header, _headerCount, copyCount);
			_headerCount += copyCount;
			offset += copyCount;
		}

		private void ReadMessageId()
		{
			_messageId = ReadUInt16(_header, 0);
			_headerCount = 0;

			if (!Messages.clientMessages.TryGetValue(_messageId, out _message))
				throw new InvalidOperationException("KBEMessageReader::Process(): message not found: " + _messageId);

			if (_message.msglen < 0)
			{
				_state = ReadState.MessageLength;
				return;
			}

			BeginBody(checked((UInt32)_message.msglen));
		}

		private void ReadMessageLength()
		{
			UInt16 length = ReadUInt16(_header, 0);
			_headerCount = 0;
			if (length == UInt16.MaxValue)
			{
				_state = ReadState.ExtendedMessageLength;
				return;
			}

			BeginBody(length);
		}

		private void BeginBody(UInt32 length)
		{
			_headerCount = 0;
			if (length > _maximumMessageSize)
				throw new InvalidOperationException(
					"KBE message length exceeds MESSAGE_MAX: " + length + " > " + _maximumMessageSize);

			_expectedBodySize = checked((int)length);
			_state = ReadState.Body;
			_body.clear();
			_body.ensureSpace(_expectedBodySize);

			// 变长消息允许空消息体；必须立即派发，避免解析器等待线上根本不存在的下一个字节。
			// Variable-length messages may legitimately have an empty body; dispatch immediately so the reader never waits for a byte absent from the wire.
			if (_expectedBodySize == 0)
				DispatchMessage();
		}

		private void DispatchMessage()
		{
			Message message = _message;
			string messageName = message.name;
			Dbg.profileStart(messageName);
			try
			{
				message.handleMessage(_body);
			}
			catch (Exception exception)
			{
				KBELog.ERROR_MSG("KBEMessageReader::DispatchMessage(): msg=" + messageName + ", " + exception);
			}
			finally
			{
				Dbg.profileEnd(messageName);
			}

			_state = ReadState.MessageId;
			_headerCount = 0;
			_expectedBodySize = 0;
			_messageId = 0;
			_message = null;
			_body.clear();
		}

		private static UInt16 ReadUInt16(byte[] buffer, int offset)
		{
			return (UInt16)(buffer[offset] | (buffer[offset + 1] << 8));
		}

		private static UInt32 ReadUInt32(byte[] buffer, int offset)
		{
			return (UInt32)(buffer[offset] |
				(buffer[offset + 1] << 8) |
				(buffer[offset + 2] << 16) |
				(buffer[offset + 3] << 24));
		}
	}
}
