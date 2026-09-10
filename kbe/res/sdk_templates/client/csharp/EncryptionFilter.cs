namespace KBEngine
{
	using System;

	public interface INetworkFrameCodec
	{
		void Encode(MemoryStream stream);
		void Decode(byte[] buffer, int offset, int count, Action<byte[], int, int> plaintextCallback);
		void Reset();
	}

	public sealed class BlowfishFrameCodec : INetworkFrameCodec
	{
		private const int BLOCK_SIZE = 8;
		private const int HEADER_SIZE = sizeof(UInt16) + sizeof(Byte);
		private const int MIN_FRAME_SIZE = HEADER_SIZE + BLOCK_SIZE;
		private const int MAX_FRAME_PAYLOAD = (UInt16.MaxValue - 1) / BLOCK_SIZE * BLOCK_SIZE;
		private readonly Blowfish _blowfish = new Blowfish();
		private readonly MemoryStream _encodedStream = new MemoryStream();
		private readonly byte[] _encodeBlock = new byte[MAX_FRAME_PAYLOAD];
		private readonly byte[] _pending = new byte[UInt16.MaxValue + HEADER_SIZE];
		private int _pendingCount;
		private int _resetGeneration;

		public byte[] Key => _blowfish.key();

		public void Encode(MemoryStream stream)
		{
			if (stream == null)
				throw new ArgumentNullException(nameof(stream));
			if (!_blowfish.isGood())
				throw new InvalidOperationException("Blowfish codec is not initialized.");

			int remaining = checked((int)stream.length());
			if (remaining == 0)
				throw new InvalidOperationException("Blowfish cannot encode an empty frame.");

			int sourceOffset = stream.rpos;
			_encodedStream.clear();
			while (remaining > 0)
			{
				int payloadLength = Math.Min(remaining, MAX_FRAME_PAYLOAD);
				int padSize = payloadLength % BLOCK_SIZE == 0 ? 0 : BLOCK_SIZE - payloadLength % BLOCK_SIZE;
				int encryptedLength = checked(payloadLength + padSize);

				Buffer.BlockCopy(stream.data(), sourceOffset, _encodeBlock, 0, payloadLength);
				if (padSize > 0)
					Array.Clear(_encodeBlock, payloadLength, padSize);
				_blowfish.encipher(_encodeBlock, encryptedLength);

				_encodedStream.ensureSpace(HEADER_SIZE + encryptedLength);
				_encodedStream.writeUint16(checked((UInt16)(encryptedLength + 1)));
				_encodedStream.writeUint8(checked((Byte)padSize));
				_encodedStream.append(_encodeBlock, 0, checked((UInt32)encryptedLength));

				sourceOffset += payloadLength;
				remaining -= payloadLength;
			}

			stream.swap(_encodedStream);
			_encodedStream.clear();
		}

		public void Decode(
			byte[] buffer,
			int offset,
			int count,
			Action<byte[], int, int> plaintextCallback)
		{
			if (buffer == null)
				throw new ArgumentNullException(nameof(buffer));
			if (plaintextCallback == null)
				throw new ArgumentNullException(nameof(plaintextCallback));
			if (offset < 0 || count < 0 || offset > buffer.Length - count)
				throw new ArgumentOutOfRangeException(nameof(offset));
			if (!_blowfish.isGood())
				throw new InvalidOperationException("Blowfish codec is not initialized.");

			while (count > 0)
			{
				int copyCount = Math.Min(count, _pending.Length - _pendingCount);
				if (copyCount == 0)
					throw new InvalidOperationException("invalid encrypted frame: buffered data exceeds the wire frame limit.");

				Buffer.BlockCopy(buffer, offset, _pending, _pendingCount, copyCount);
				_pendingCount += copyCount;
				offset += copyCount;
				count -= copyCount;
				int generation = _resetGeneration;
				DrainFrames(plaintextCallback);
				if (generation != _resetGeneration)
					return;
			}
		}

		public void Reset()
		{
			if (_pendingCount > 0)
				Array.Clear(_pending, 0, _pendingCount);
			_pendingCount = 0;
			_encodedStream.clear();
			unchecked { ++_resetGeneration; }
		}

		private void DrainFrames(Action<byte[], int, int> plaintextCallback)
		{
			int consumed = 0;
			int generation = _resetGeneration;
			while (_pendingCount - consumed >= HEADER_SIZE)
			{
				int encodedLength = _pending[consumed] | (_pending[consumed + 1] << 8);
				int padSize = _pending[consumed + sizeof(UInt16)];
				int payloadLength;
				if (!ValidFrame(encodedLength, padSize, out payloadLength))
				{
					Reset();
					throw new InvalidOperationException("invalid encrypted frame");
				}

				int frameLength = HEADER_SIZE + payloadLength;
				if (_pendingCount - consumed < frameLength)
					break;

				int payloadOffset = consumed + HEADER_SIZE;
				_blowfish.decipher(_pending, payloadOffset, payloadLength);
				plaintextCallback(_pending, payloadOffset, payloadLength - padSize);
				// 消息处理可能同步销毁 Session 并 Reset codec；此时缓存游标已失效，不能继续扣减。
				// Message handling may synchronously destroy the session and reset this codec; its cache cursors are then invalid.
				if (generation != _resetGeneration)
					return;
				consumed += frameLength;
			}

			if (consumed == 0)
				return;

			_pendingCount -= consumed;
			if (_pendingCount > 0)
				Buffer.BlockCopy(_pending, consumed, _pending, 0, _pendingCount);
		}

		private static bool ValidFrame(int encodedLength, int padSize, out int payloadLength)
		{
			payloadLength = encodedLength > 0 ? encodedLength - 1 : 0;
			return payloadLength > 0 &&
				payloadLength % (int)BLOCK_SIZE == 0 &&
				padSize >= 0 &&
				padSize < (int)BLOCK_SIZE &&
				padSize <= payloadLength &&
				HEADER_SIZE + payloadLength >= MIN_FRAME_SIZE;
		}
	}
}
