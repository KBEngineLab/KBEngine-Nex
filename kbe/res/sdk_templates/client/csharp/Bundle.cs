namespace KBEngine
{
	using System; 
	using System.Collections;
	using System.Collections.Generic;
	
	/*
		这个模块将多个数据包打捆在一起
		由于每个数据包都有最大上限， 向Bundle中写入大量数据将会在内部产生多个MemoryStream
		在send时会全部发送出去
	*/
	public class Bundle : ObjectPool<Bundle>
    {
		public MemoryStream stream = new MemoryStream();
		public List<MemoryStream> streamList = new List<MemoryStream>();
		public int numMessage = 0;
		public int messageLength = 0;
		public Message msgtype = null;
		private int _curMsgStreamIndex = 0;
		
		public Bundle()
		{
		}

		public void clear()
		{
            // 把不用的MemoryStream放回缓冲池，以减少垃圾回收的消耗
            for (int i = 0; i < streamList.Count; ++i)
            {
                if (stream != streamList[i])
                    streamList[i].reclaimObject();
            }

            streamList.Clear();

            if(stream != null)
                stream.clear();
            else
                stream = MemoryStream.createObject();

			numMessage = 0;
			messageLength = 0;
			msgtype = null;
			_curMsgStreamIndex = 0;
		}

		/// <summary>
		/// 把自己放回缓冲池
		/// </summary>
		public void reclaimObject()
		{
			clear();
			reclaimObject(this);
		}
		
		public void newMessage(Message mt)
		{
			fini(false);
			
			msgtype = mt;
			numMessage += 1;

			writeUint16(msgtype.id);

			if(msgtype.msglen == -1)
			{
				writeUint16(0);
				messageLength = 0;
			}

			_curMsgStreamIndex = 0;
		}
		
		public void writeMsgLength()
		{
			if(msgtype.msglen != -1)
				return;

			MemoryStream stream = this.stream;
			if(_curMsgStreamIndex > 0)
			{
				stream = streamList[streamList.Count - _curMsgStreamIndex];
			}
			stream.data()[2] = (Byte)(messageLength & 0xff);
			stream.data()[3] = (Byte)(messageLength >> 8 & 0xff);
		}
		
		public void fini(bool issend)
		{
			if(numMessage > 0)
			{
				writeMsgLength();

				streamList.Add(stream);
				stream = MemoryStream.createObject();
			}
			
			if(issend)
			{
				numMessage = 0;
				msgtype = null;
			}

			_curMsgStreamIndex = 0;
		}
		
		public bool send(NetworkInterfaceBase networkInterface)
		{
			fini(true);
			bool sent = false;
			try
			{
				sent = networkInterface != null && networkInterface.send(streamList);
			}
			catch (Exception exception)
			{
				KBELog.ERROR_MSG("Bundle::send: atomic batch raised an exception: " + exception);
			}
			finally
			{
				// Bundle 是消费型对象；无论提交成功、背压拒绝还是 transport 异常，都必须归还全部 stream，防止对象池资源泄漏。
				// A Bundle is consumed by send; success, backpressure rejection, and transport exceptions must all return every stream to prevent pooled-resource leaks.
				reclaimObject();
			}

			if (!sent && networkInterface != null)
			{
				KBELog.ERROR_MSG("Bundle::send: atomic batch rejected; closing the current transport!");
				Event.fireIn("_closeNetwork", new object[] { networkInterface });
			}

			return sent;
		}
		
		public void checkStream(int v)
		{
			if (v < 0)
				throw new ArgumentOutOfRangeException(nameof(v));

			if(v > stream.space())
			{
				streamList.Add(stream);
				stream = MemoryStream.createObject();
				++ _curMsgStreamIndex;
			}

			// 单个 BLOB/UNICODE 字段可能大于默认分段容量；换到新 stream 后仍需保证整个字段连续，读取端才能按长度前缀解析。
			// A single BLOB/UNICODE field may exceed the default segment; after switching streams it must remain contiguous for length-prefixed decoding.
			stream.ensureSpace(v);
	
			messageLength += v;
		}
		
		//---------------------------------------------------------------------------------
		public void writeInt8(SByte v)
		{
			checkStream(1);
			stream.writeInt8(v);
		}
	
		public void writeInt16(Int16 v)
		{
			checkStream(2);
			stream.writeInt16(v);
		}
			
		public void writeInt32(Int32 v)
		{
			checkStream(4);
			stream.writeInt32(v);
		}
	
		public void writeInt64(Int64 v)
		{
			checkStream(8);
			stream.writeInt64(v);
		}
		
		public void writeUint8(Byte v)
		{
			checkStream(1);
			stream.writeUint8(v);
		}
	
		public void writeUint16(UInt16 v)
		{
			checkStream(2);
			stream.writeUint16(v);
		}
			
		public void writeUint32(UInt32 v)
		{
			checkStream(4);
			stream.writeUint32(v);
		}
	
		public void writeUint64(UInt64 v)
		{
			checkStream(8);
			stream.writeUint64(v);
		}
		
		public void writeFloat(float v)
		{
			checkStream(4);
			stream.writeFloat(v);
		}
	
		public void writeDouble(double v)
		{
			checkStream(8);
			stream.writeDouble(v);
		}
		
		public void writeString(string v)
		{
			checkStream(v.Length + 1);
			stream.writeString(v);
		}

		public void writeUnicode(string v)
		{
			writeBlob(System.Text.Encoding.UTF8.GetBytes((string)v));
		}
		
		public void writeBlob(byte[] v)
		{
			checkStream(v.Length + 4);
			stream.writeBlob(v);
		}

		public void writePython(byte[] v)
		{
			writeBlob(v);
		}

		public void writeVector2(KBVector2 v)
		{
			checkStream(8);
			stream.writeVector2(v);
		}

		public void writeVector3(KBVector3 v)
		{
			checkStream(12);
			stream.writeVector3(v);
		}

		public void writeVector4(KBVector4 v)
		{
			checkStream(16);
			stream.writeVector4(v);
		}

		public void writeEntitycall(byte[] v)
		{
			checkStream(16);

			UInt64 cid = 0;
			Int32 id = 0;
			UInt16 type = 0;
			UInt16 utype = 0;

			stream.writeUint64(cid);
			stream.writeInt32(id);
			stream.writeUint16(type);
			stream.writeUint16(utype);
		}
    }
} 
