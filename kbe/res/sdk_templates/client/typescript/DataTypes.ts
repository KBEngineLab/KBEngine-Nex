import KBELog from "./KBELog";
import * as KBEEncoding from "./KBEEncoding";
import * as Int64 from "./Int64";
import { Vector2, Vector3, Vector4 } from "./KBEMath";
import { MemoryStream } from "./MemoryStream";

// 协议数据类型只依赖最小写入契约，使 schema 模块不需要反向导入应用聚合入口。
// Protocol data types depend on a minimal writer contract so schema modules never import the application aggregate.
export interface DataTypeWriter {
    WriteUint8(value: number): void;
    WriteUint16(value: number): void;
    WriteUint32(value: number): void;
    WriteUint64(value: Int64.KB_UINT64): void;
    WriteInt8(value: number): void;
    WriteInt16(value: number): void;
    WriteInt32(value: number): void;
    WriteInt64(value: Int64.KB_INT64): void;
    WriteFloat(value: number): void;
    WriteDouble(value: number): void;
    WriteBlob(value: string | Uint8Array): void;
    WriteString(value: string): void;
    WriteUnicode(value: string): void;
}

export type DataTypeResolver = (type: string | number) => DataTypes.DATATYPE_BASE | undefined;

//#region KBEngine DataTypes


export namespace DataTypes{
    const TWO_PWR_16_DBL = 1 << 16;
    const TWO_PWR_32_DBL = TWO_PWR_16_DBL * TWO_PWR_16_DBL;

    // 用显式运行时常量保留旧 SDK 的 DataTypes.KB_* 访问方式，避免部分打包器擦除 namespace import alias。
    // Use explicit runtime constants for legacy DataTypes.KB_* access so bundlers cannot erase namespace import aliases.
    export type KB_INT64 = Int64.KB_INT64;
    export const KB_INT64: typeof Int64.KB_INT64 = Int64.KB_INT64;
    export type KB_UINT64 = Int64.KB_UINT64;
    export const KB_UINT64: typeof Int64.KB_UINT64 = Int64.KB_UINT64;
    export class NumberUtil {
        static toUInt8(n: number): number {
            return n & 0xFF;
        }

        static toUInt16(n: number): number {
            return n & 0xFFFF;
        }

        static toUInt32(n: number): number {
            return n >>> 0;
        }

        // 使用 BigInt 保留 UInt64 的完整位宽，避免 JavaScript number 精度截断。
        // Use BigInt to preserve the full UInt64 width instead of losing precision through JavaScript numbers.
        static toUInt64(n: number | bigint): bigint {
            return BigInt(n) & BigInt("0xFFFFFFFFFFFFFFFF");
        }

        static toInt8(n: number): number {
            const val = n & 0xFF;
            return val > 0x7F ? val - 0x100 : val;
        }

        static toInt16(n: number): number {
            const val = n & 0xFFFF;
            return val > 0x7FFF ? val - 0x10000 : val;
        }

        static toInt32(n: number): number {
            // 位运算按 JavaScript 规范将结果归一为有符号 Int32。
            // Bitwise operations normalize the result to a signed Int32 under JavaScript semantics.
            return n | 0;
        }

        // 将无符号位模式折回有符号区间，同时保持 64 位精度。
        // Fold the unsigned bit pattern into the signed range while preserving 64-bit precision.
        static toInt64(n: number | bigint): bigint {
            const big = BigInt(n);
            return big >= BigInt("0x8000000000000000")
                ? big - BigInt("0x10000000000000000")
                : big;
        }

        // 通过单元素缓冲区执行与协议 FLOAT 一致的 Float32 舍入。
        // Use a single-element buffer to apply the same Float32 rounding as the protocol FLOAT type.
        static toFloat(n: number): number {
            const f32 = new Float32Array(1);
            f32[0] = n;
            return f32[0];
        }

        // JavaScript number 已采用 IEEE-754 双精度表示，无需额外转换。
        // JavaScript numbers already use IEEE-754 double precision and need no additional conversion.
        static toDouble(n: number): number {
            return n;
        }
    }


    export class UINT64_OLD {
        low: number;
        high: number;

        constructor(p_low: number, p_high: number) {
            this.low = p_low >>> 0;
            this.high = p_high;
        }

        toString() {
            let low = this.low.toString(16);
            let high = this.high.toString(16);

            let result = "";
            if (this.high > 0) {
                result += high;
                for (let i = 8 - low.length; i > 0; --i) {
                    result += "0";
                }
            }

            return result + low;
        }

        static BuildUINT64(data: number): KB_UINT64 {
            let low = (data % TWO_PWR_32_DBL) | 0;
            low >>>= 0;
            let high = (data / TWO_PWR_32_DBL) | 0;
            high >>>= 0;
            return new KB_UINT64(low, high);
        }
    }


    function IsNumber(anyObject: any): boolean {
        return typeof anyObject === "number" || typeof anyObject == 'boolean';
    }

    export abstract class DATATYPE_BASE {
        static readonly FLOATE_MAX = Number.MAX_VALUE;

        Bind(_resolveType?: DataTypeResolver): void { }

        CreateFromStream(stream: MemoryStream): any {
            return null;
        }
        AddToStream(stream: DataTypeWriter, value: any): void {

        }
        ParseDefaultValueString(value: string): any {
            return null;
        }
        IsSameType(value: any): boolean {
            return value == null;
        }
    }

    export class DATATYPE_UINT8 extends DATATYPE_BASE {
        CreateFromStream(stream: MemoryStream): any {

            return stream.ReadUint8();
        }

        AddToStream(stream: DataTypeWriter, value: any): void {
            return stream.WriteUint8(value);
        }

        ParseDefaultValueString(value: string): any {
            return parseInt(value);
        }

        IsSameType(value: any): boolean {
            if (!IsNumber(value))
                return false;

            if (value < 0 || value > 0xff) {
                return false;
            }

            return true;
        }
    }

    export class DATATYPE_UINT16 extends DATATYPE_BASE {
        CreateFromStream(stream: MemoryStream): any {
            return stream.ReadUint16();
        }

        AddToStream(stream: DataTypeWriter, value: any): void {
            return stream.WriteUint16(value);
        }

        ParseDefaultValueString(value: string): any {
            return parseInt(value);
        }

        IsSameType(value: any): boolean {
            if (!IsNumber(value))
                return false;

            if (value < 0 || value > 0xffff) {
                return false;
            }

            return true;
        }
    }

    export class DATATYPE_UINT32 extends DATATYPE_BASE {
        CreateFromStream(stream: MemoryStream): any {
            return stream.ReadUint32();
        }

        AddToStream(stream: DataTypeWriter, value: any): void {
            return stream.WriteUint32(value);
        }

        ParseDefaultValueString(value: string): any {
            return parseInt(value);
        }

        IsSameType(value: any): boolean {
            if (!IsNumber(value))
                return false;

            if (value < 0 || value > 0xffffffff) {
                return false;
            }

            return true;
        }
    }

    export class DATATYPE_UINT64 extends DATATYPE_BASE {
        CreateFromStream(stream: MemoryStream): any {
            return stream.ReadUint64();
        }

        AddToStream(stream: DataTypeWriter, value: any): void {
            return stream.WriteUint64(value);
        }

        ParseDefaultValueString(value: string): any {
            return parseInt(value);
        }

        IsSameType(value: any): boolean {
            return value instanceof KB_UINT64 || typeof value === "bigint";
        }
    }

    export class DATATYPE_INT8 extends DATATYPE_BASE {
        CreateFromStream(stream: MemoryStream): any {
            return stream.ReadInt8();
        }

        AddToStream(stream: DataTypeWriter, value: any): void {
            return stream.WriteInt8(value);
        }

        ParseDefaultValueString(value: string): any {
            return parseInt(value);
        }

        IsSameType(value: any): boolean {
            if (!IsNumber(value))
                return false;

            if (value < -0x80 || value > 0x7f) {
                return false;
            }

            return true;
        }
    }

    export class DATATYPE_INT16 extends DATATYPE_BASE {
        CreateFromStream(stream: MemoryStream): any {
            return stream.ReadInt16();
        }

        AddToStream(stream: DataTypeWriter, value: any): void {
            return stream.WriteInt16(value);
        }

        ParseDefaultValueString(value: string): any {
            return parseInt(value);
        }

        IsSameType(value: any): boolean {
            if (!IsNumber(value))
                return false;

            if (value < -0x8000 || value > 0x7fff) {
                return false;
            }

            return true;
        }
    }

    export class DATATYPE_INT32 extends DATATYPE_BASE {
        CreateFromStream(stream: MemoryStream): any {
            return stream.ReadInt32();
        }

        AddToStream(stream: DataTypeWriter, value: any): void {
            return stream.WriteInt32(value);
        }

        ParseDefaultValueString(value: string): any {
            return parseInt(value);
        }

        IsSameType(value: any): boolean {
            if (!IsNumber(value))
                return false;

            if (value < -0x80000000 || value > 0x7fffffff) {
                return false;
            }

            return true;
        }
    }

    export class DATATYPE_INT64 extends DATATYPE_BASE {
        CreateFromStream(stream: MemoryStream): any {
            return stream.ReadInt64();
        }

        AddToStream(stream: DataTypeWriter, value: any): void {
            return stream.WriteInt64(value);
        }

        ParseDefaultValueString(value: string): any {
            return parseInt(value);
        }

        IsSameType(value: any): boolean {
            return value instanceof KB_INT64 || typeof value === "bigint";
        }
    }

    export class DATATYPE_FLOAT extends DATATYPE_BASE {
        CreateFromStream(stream: MemoryStream): any {
            return stream.ReadFloat();
        }

        AddToStream(stream: DataTypeWriter, value: any): void {
            return stream.WriteFloat(value);
        }

        ParseDefaultValueString(value: string): any {
            return parseFloat(value);
        }

        IsSameType(value: any): boolean {
            return typeof (value) === "number";
        }
    }

    export class DATATYPE_DOUBLE extends DATATYPE_BASE {
        CreateFromStream(stream: MemoryStream): any {
            return stream.ReadDouble();
        }

        AddToStream(stream: DataTypeWriter, value: any): void {
            return stream.WriteDouble(value);
        }

        ParseDefaultValueString(value: string): any {
            return parseFloat(value);
        }

        IsSameType(value: any): boolean {
            return typeof (value) === "number";
        }
    }

    export class DATATYPE_STRING extends DATATYPE_BASE {
        CreateFromStream(stream: MemoryStream): any {
            return stream.ReadString();
        }

        AddToStream(stream: DataTypeWriter, value: any): void {
            return stream.WriteString(value);
        }

        ParseDefaultValueString(value: string): any {
            return value;   // TODO: 需要测试正确
        }

        IsSameType(value: any): boolean {
            return typeof (value) === "string";
        }
    }

    export class DATATYPE_VECTOR2 extends DATATYPE_BASE {
        CreateFromStream(stream: MemoryStream): any {
            return new Vector2(stream.ReadFloat(), stream.ReadFloat());
        }

        AddToStream(stream: DataTypeWriter, value: any): void {
            stream.WriteFloat(value.x);
            stream.WriteFloat(value.y);
        }

        ParseDefaultValueString(value: string): any {
            return new Vector2(0.0, 0.0);
        }

        IsSameType(value: any): boolean {
            return value instanceof Vector2;
        }
    }

    export class DATATYPE_VECTOR3 extends DATATYPE_BASE {
        CreateFromStream(stream: MemoryStream): any {
            return new Vector3(stream.ReadFloat(), stream.ReadFloat(), stream.ReadFloat());
        }

        AddToStream(stream: DataTypeWriter, value: any): void {
            stream.WriteFloat(value.x);
            stream.WriteFloat(value.y);
            stream.WriteFloat(value.z);
        }

        ParseDefaultValueString(value: string): any {
            return new Vector3(0.0, 0.0, 0.0);
        }

        IsSameType(value: any): boolean {
            return value instanceof Vector3;
        }
    }

    export class DATATYPE_VECTOR4 extends DATATYPE_BASE {
        CreateFromStream(stream: MemoryStream): any {
            return new Vector4(stream.ReadFloat(), stream.ReadFloat(), stream.ReadFloat(), stream.ReadFloat());
        }

        AddToStream(stream: DataTypeWriter, value: any): void {
            stream.WriteFloat(value.x);
            stream.WriteFloat(value.y);
            stream.WriteFloat(value.z);
            stream.WriteFloat(value.w);
        }

        ParseDefaultValueString(value: string): any {
            return new Vector4(0.0, 0.0, 0.0, 0.0);
        }

        IsSameType(value: any): boolean {
            return value instanceof Vector4;
        }
    }


    export class DATATYPE_PYTHON extends DATATYPE_BASE {
        CreateFromStream(stream: MemoryStream): any {
            return stream.ReadBlob();
        }

        AddToStream(stream: DataTypeWriter, value: any): void {
            stream.WriteBlob(value);
        }

        ParseDefaultValueString(value: string): any {
            return new Uint8Array(0);
        }

        IsSameType(value: any): boolean {
            return value instanceof Uint8Array;
        }
    }

    export class DATATYPE_UNKNOW extends DATATYPE_BASE {
        CreateFromStream(stream: MemoryStream): any {
        }

        AddToStream(stream: DataTypeWriter, value: any): void {
        }

        ParseDefaultValueString(value: string): any {
        }

        IsSameType(value: any): any {
        }
    }

    export class DATATYPE_UNICODE extends DATATYPE_BASE {
        CreateFromStream(stream: MemoryStream): any {
            return KBEEncoding.UTF8ArrayToString(stream.ReadBlob());
        }

        AddToStream(stream: DataTypeWriter, value: any): void {
            stream.WriteBlob(KBEEncoding.StringToUTF8Array(value));
        }

        ParseDefaultValueString(value: string): any {
            return value;
        }

        IsSameType(value: any): boolean {
            return typeof value === "string";
        }
    }

    export class DATATYPE_ENTITYCALL extends DATATYPE_BASE {
        CreateFromStream(stream: MemoryStream): any {
            stream.ReadInt32()
            stream.ReadUint64()
            stream.ReadUint16()
            stream.ReadUint16()
            return null
        }

        AddToStream(stream: DataTypeWriter, value: any): void {
            stream.WriteBlob(value);
        }

        ParseDefaultValueString(value: string): any {
        }

        IsSameType(value: any): boolean {
            return false;
        }
    }

    export class DATATYPE_BLOB extends DATATYPE_BASE {
        CreateFromStream(stream: MemoryStream): any {
            return stream.ReadBlob();
        }

        AddToStream(stream: DataTypeWriter, value: any): void {
            stream.WriteBlob(value);
        }

        ParseDefaultValueString(value: string): any {
            return new Uint8Array(0);
        }

        IsSameType(value: any): boolean {
            return true;
        }
    }

    export class DATATYPE_ARRAY extends DATATYPE_BASE {
        type: any;

        Bind(resolveType: DataTypeResolver) {
            if (typeof (this.type) == "number")
                this.type = resolveType(this.type);
        }

        CreateFromStream(stream: MemoryStream): Array<any> {
            let size = stream.ReadUint32();
            let items = [];
            while (size-- > 0) {
                size--;
            }

            return items;
        }

        AddToStream(stream: DataTypeWriter, value: any): void {
            stream.WriteUint32(value.length);
            for (let i = 0; i < value.length; i++) {
                this.type.AddToStream(stream, value[i]);
            }
        }

        ParseDefaultValueString(value: string): any {
            return [];
        }

        IsSameType(value: any): boolean {
            for (let i = 0; i < value.length; i++) {
                if (!this.type.IsSameType(value[i]))
                    return false;
            }

            return true;
        }
    }

    export class DATATYPE_FIXED_DICT extends DATATYPE_BASE {
        dictType: { [key: string]: any } = {};
        implementedBy: string;

        Bind(resolveType: DataTypeResolver) {
            for (let key in this.dictType) {
                if (typeof (this.dictType[key]) == "number") {
                    let utype = Number(this.dictType[key]);
                    this.dictType[key] = resolveType(utype);
                }
            }
        }

        CreateFromStream(stream: MemoryStream): { [key: string]: any } {
            let datas = {};
            for (let key in this.dictType) {
                KBELog.DEBUG_MSG("DATATYPE_FIXED_DICT::CreateFromStream------------------->>>FIXED_DICT(key:%s).", key);
                datas[key] = this.dictType[key].CreateFromStream(stream);
            }

            return datas;
        }

        AddToStream(stream: DataTypeWriter, value: any): void {
            for (let key in this.dictType) {
                this.dictType[key].AddToStream(stream, value[key]);
            }
        }

        ParseDefaultValueString(value: string): any {
            return {};
        }

        IsSameType(value: any): boolean {
            for (let key in this.dictType) {
                if (!this.dictType[key].IsSameType(value[key]))
                    return false;
            }
            return true;
        }
    }
}
//#endregion
