import KBELog from "./KBELog";
import { Vector2, Vector3, Vector4 } from "./KBEMath";
import { KB_INT64, KB_UINT64 } from "./Int64";

class PackFloatXType
{
    private _unionData: ArrayBuffer;
    fv: Float32Array;
    uv: Uint32Array;
    iv: Int32Array;

    constructor()
    {
        this._unionData = new ArrayBuffer(4);
        this.fv = new Float32Array(this._unionData, 0, 1);
        this.uv = new Uint32Array(this._unionData, 0, 1);
        this.iv = new Int32Array(this._unionData, 0, 1);
    }
}

export class MemoryStream
{
    rpos: number = 0;
    wpos: number = 0;
    private buffer: ArrayBuffer;

    constructor(size_or_buffer: number | ArrayBuffer)
    {
        if(size_or_buffer instanceof ArrayBuffer)
        {
            this.buffer = size_or_buffer;
        }
        else
        {
            this.buffer = new ArrayBuffer(size_or_buffer);
        }
    }

    Space(): number
    {
        return this.buffer.byteLength - this.wpos;
    }

    EnsureSpace(size: number): void
    {
        if(!Number.isSafeInteger(size) || size < 0)
            throw new Error("MemoryStream growth size must be a non-negative safe integer.");
        if(size <= this.Space())
            return;

        const required = this.wpos + size;
        let capacity = Math.max(this.buffer.byteLength, 1);
        while(capacity < required)
            capacity = Math.max(capacity * 2, required);

        // 扩容只复制已写入字节，避免把未使用容量带入大字段的瞬时分配和 GC 压力。
        // Growth copies only written bytes so unused capacity does not amplify transient allocation and GC pressure for large fields.
        const replacement = new ArrayBuffer(capacity);
        new Uint8Array(replacement).set(new Uint8Array(this.buffer, 0, this.wpos));
        this.buffer = replacement;
    }

    Insert(offset: number, size: number): void
    {
        if(!Number.isSafeInteger(offset) || offset < 0 || offset > this.wpos)
            throw new Error("MemoryStream insertion offset is outside written data.");
        if(!Number.isSafeInteger(size) || size < 0)
            throw new Error("MemoryStream insertion size must be a non-negative safe integer.");
        if(size === 0)
            return;

        this.EnsureSpace(size);
        const bytes = new Uint8Array(this.buffer);
        bytes.copyWithin(offset + size, offset, this.wpos);
        bytes.fill(0, offset, offset + size);
        this.wpos += size;
    }

    ReadInt8(): number
    {
        let buf = new Int8Array(this.buffer, this.rpos);
        this.rpos += 1;
        return buf[0];
    }

    ReadUint8(): number
    {
        let buf = new Uint8Array(this.buffer, this.rpos);
        this.rpos += 1;
        return buf[0];
    }

    ReadUint16(): number
    {
        let buf = new Uint8Array(this.buffer, this.rpos);
        this.rpos += 2;
        return ((buf[1] & 0xff) << 8) + (buf[0] & 0xff);
    }

    ReadInt16(): number
    {
        let value = this.ReadUint16();
        if(value >= 32768)
            value -= 65536;
        return value;
    }

    ReadUint32(): number
    {
        let buf = new Uint8Array(this.buffer, this.rpos);
        this.rpos += 4;

        return (buf[3] << 24) + (buf[2] << 16) + (buf[1] << 8) + buf[0];
    }

    ReadInt32(): number
    {
        let value = this.ReadUint32();
        if(value >= 2147483648)
            value -= 4294967296;
        return value;
    }

    ReadUint64(): KB_UINT64
    {
        return new KB_UINT64(this.ReadUint32(), this.ReadUint32());
    }

    ReadInt64(): KB_INT64
    {
        return new KB_INT64(this.ReadUint32(), this.ReadUint32());
    }

    ReadFloat(): number
    {
        let buf: Float32Array | undefined;
        try
        {
            buf = new Float32Array(this.buffer, this.rpos, 1);
        }
        catch(e)
        {
            buf = new Float32Array(this.buffer.slice(this.rpos, this.rpos + 4));
        }

        this.rpos += 4;

        return buf[0];
    }

    ReadDouble(): number
    {
        let buf: Float64Array | undefined;
		try
		{
			buf = new Float64Array(this.buffer, this.rpos, 1);
		}
		catch(e)
		{
			buf = new Float64Array(this.buffer.slice(this.rpos, this.rpos + 8), 0, 1);
        }

        this.rpos += 8;
        return buf[0];
    }

    ReadString(): string
    {
        let buf = new Int8Array(this.buffer, this.rpos);
        let value: string = "";
        let index: number = 0;

        while(true)
        {
            if(buf[index] != 0 )
            {
                value += String.fromCharCode(buf[index]);
                index += 1;
                if(this.rpos + index >= this.buffer.byteLength)
                {
                    throw(new Error("KBEngine.MemoryStream::ReadString overflow(>=) max length:" + this.buffer.byteLength));
                }
            }
            else
            {
                index += 1;
                break;
            }
        }

        this.rpos += index;
        return value;
    }


    ReadUnicode(): string {
        // 协议长度按 UTF-8 字节数编码，不能使用 JavaScript 字符数量推进读指针。
        // The protocol stores the UTF-8 byte count, so the read cursor must not advance by the JavaScript character count.
        const size = this.ReadUint32();
        const buf = new Uint8Array(this.buffer, this.rpos, size);
        this.rpos += size;

        const decoder = new TextDecoder("utf-8");
        return decoder.decode(buf);
    }

    ReadBlob(): Uint8Array
    {
        let size = this.ReadUint32();
        let buf = new Uint8Array(this.buffer, this.rpos, size);
        this.rpos += size;
        return buf;
    }

    ReadVector2(): Vector2 {
        return new Vector2(this.ReadFloat(), this.ReadFloat());
    }

    ReadVector3(): Vector3 {
        return new Vector3(this.ReadFloat(), this.ReadFloat(), this.ReadFloat());
    }

    ReadVector4(): Vector4 {
        return new Vector4(this.ReadFloat(), this.ReadFloat(), this.ReadFloat(), this.ReadFloat());
    }

    ReadPackXZ(): Array<number>
    {
        let xPackData = new PackFloatXType();
        let zPackData = new PackFloatXType();

        xPackData.fv[0] = 0.0;
        zPackData.fv[0] = 0.0;

        xPackData.uv[0] = 0x40000000;
        zPackData.uv[0] = 0x40000000;
		let v1 = this.ReadUint8();
		let v2 = this.ReadUint8();
		let v3 = this.ReadUint8();

		let data = 0;
		data |= (v1 << 16);
		data |= (v2 << 8);
		data |= v3;

		xPackData.uv[0] |= (data & 0x7ff000) << 3;
		zPackData.uv[0] |= (data & 0x0007ff) << 15;

		xPackData.fv[0] -= 2.0;
		zPackData.fv[0] -= 2.0;

		xPackData.uv[0] |= (data & 0x800000) << 8;
		zPackData.uv[0] |= (data & 0x000800) << 20;

		let xzData = new Array(2);
		xzData[0] = xPackData.fv[0];
		xzData[1] = zPackData.fv[0];
		return xzData;
    }

    ReadPackY(): number
    {
        let data = this.ReadUint16();

        let yPackData = new PackFloatXType();
        yPackData.uv[0] = 0x40000000;
        // 先恢复正数尾数并减去编码偏移，最后设置符号位，避免按正负分别执行浮点运算。
        // Restore the positive mantissa and remove the encoding offset before applying the sign bit, avoiding separate floating-point paths by sign.
        yPackData.uv[0] |= (data & 0x7fff) << 12;
        yPackData.fv[0] -= 2.0;
        yPackData.uv[0] |= (data & 0x8000) << 16;

        return yPackData.fv[0];
    }

    WriteInt8(value: number): void
    {
        let buf = new Int8Array(this.buffer, this.wpos, 1);
        buf[0] = value;
        this.wpos += 1;
    }

    WriteInt16(value: number): void
    {
        this.WriteInt8(value & 0xff);
        this.WriteInt8((value >> 8) & 0xff);
    }

    WriteInt32(value: number): void
    {
        for(let i = 0; i < 4; i++)
            this.WriteInt8((value >> i * 8) & 0xff);
    }

    WriteInt64(value: KB_INT64): void
    {
        this.WriteInt32(value.low);
        this.WriteInt32(value.high);
    }

    WriteUint8(value: number): void
    {
        let buf = new Uint8Array(this.buffer, this.wpos, 1);
        buf[0] = value;
        this.wpos += 1;
    }

    WriteUint16(value: number): void
    {
        this.WriteUint8(value & 0xff);
        this.WriteUint8((value >> 8) & 0xff);
    }

    WriteUint32(value: number): void
    {
        for(let i = 0; i < 4; i++)
            this.WriteUint8((value >> i*8) & 0xff);
    }

    WriteUint64(value: KB_UINT64): void
    {
        this.WriteUint32(value.low);
        this.WriteUint32(value.high);
    }

    WriteFloat(value: number): void
    {
        try
        {
            let buf = new Float32Array(this.buffer, this.wpos, 1);
            buf[0] = value;
        }
        catch(e)
        {
            let buf = new Float32Array(1);
            buf[0] = value;
            let buf1 = new Uint8Array(this.buffer);
            let buf2 = new Uint8Array(buf.buffer);
            buf1.set(buf2, this.wpos);
        }

        this.wpos += 4;
    }

    WriteDouble(value: number): void
    {
		try
		{
			let buf = new Float64Array(this.buffer, this.wpos, 1);
			buf[0] = value;
		}
		catch(e)
		{
			let buf = new Float64Array(1);
			buf[0] = value;
			let buf1 = new Uint8Array(this.buffer);
			let buf2 = new Uint8Array(buf.buffer);
			buf1.set(buf2, this.wpos);
        }

        this.wpos += 8;
    }

    WriteBlob(value: string|Uint8Array): void
    {
        let size = value.length;
        if(size + 4 > this.Space())
        {
            KBELog.ERROR_MSG("KBE.MemoryStream:WriteBlob:there is no space for size:%d", size + 4);
            return;
        }

        this.WriteUint32(size);

        let buf = new Uint8Array(this.buffer, this.wpos, size);
        if(typeof(value) == "string")
        {
            for(let i = 0; i < size; i++)
            {
                buf[i] = value.charCodeAt(i);
            }
        }
        else
        {
            for(let i = 0; i< size; i++)
            {
                buf[i] = value[i];
            }
        }

        this.wpos += size;
    }

    WriteString(value: string): void
    {

        if(value.length + 1 > this.Space())
        {
            KBELog.ERROR_MSG("KBE.MemoryStream:WriteString:there is no space for size:%d", value.length + 1);
            return;
        }

        let buf = new Uint8Array(this.buffer, this.wpos, value.length);
        for(let i = 0; i < value.length; i++)
        {
            buf[i] = value.charCodeAt(i);
        }

        buf[value.length] = 0;
        this.wpos = this.wpos + value.length + 1;
    }

    ReadSkip(count: number): void
    {
        this.rpos += count;
    }

    Length(): number
    {
        return this.wpos - this.rpos;
    }

    ReadEOF(): boolean
    {
        return this.buffer.byteLength - this.rpos <= 0;
    }

    Done(): void
    {
        this.rpos = this.wpos;
    }

    GetBuffer(): ArrayBuffer
    {
        return this.buffer.slice(this.rpos, this.wpos);
    }

    GetRawBuffer(): ArrayBuffer
    {
        return this.buffer;
    }
}
