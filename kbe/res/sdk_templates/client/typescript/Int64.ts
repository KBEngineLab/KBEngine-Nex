const TWO_PWR_16_DBL = 1 << 16;
const TWO_PWR_32_DBL = TWO_PWR_16_DBL * TWO_PWR_16_DBL;

export class KB_INT64 {
    low: number;
    high: number;
    private _isNegative: boolean;

    constructor(low: number, high: number) {
        this.low = low >>> 0;

        // 把输入规范化为 32 位补码后再读取符号位，负数输入和无符号 high 都能得到相同的协议位模式。
        // Normalize the input to a 32-bit two's-complement pattern before reading its sign bit so signed and unsigned high words decode identically.
        this.high = high >>> 0;
        this._isNegative = (this.high & 0x80000000) !== 0;

        // 转为绝对值分量供数值转换使用；写回协议时仍由构造入口维持既有补码语义。
        // Convert to magnitude words for numeric conversion while preserving the existing two's-complement construction contract for protocol writes.
        if (this._isNegative) {
            const notLow = (~this.low + 1) >>> 0;
            const carry = notLow === 0 ? 1 : 0;
            const notHigh = (~this.high + carry) >>> 0;
            this.low = notLow;
            this.high = notHigh;
        }
    }

    toNumber(): number {
        let val = this.high * TWO_PWR_32_DBL + this.low;
        return this._isNegative ? -val : val;
    }

    toBigInt(): bigint {
        let bi = (BigInt(this.high) << 32n) | BigInt(this.low);
        return this._isNegative ? -bi : bi;
    }

    toString(radix: number = 10): string {
        return this.toBigInt().toString(radix);
    }

    equals(other: KB_INT64): boolean {
        return this.toBigInt() === other.toBigInt();
    }

    static fromNumber(num: number): KB_INT64 {
        const isNegative = num < 0;
        if (isNegative) num = -num;

        const low = num >>> 0;
        const high = Math.floor(num / TWO_PWR_32_DBL) >>> 0;
        let int64 = new KB_INT64(low, high);

        if (isNegative) {
            let l = (~int64.low + 1) >>> 0;
            let h = (~int64.high + (l === 0 ? 1 : 0)) >>> 0;
            int64.low = l;
            int64.high = h;
            int64._isNegative = true;
        }

        return int64;
    }

    static fromBigInt(bi: bigint): KB_INT64 {
        const isNegative = bi < 0n;
        if (isNegative) bi = -bi;

        let low = Number(bi & 0xFFFFFFFFn);
        let high = Number((bi >> 32n) & 0xFFFFFFFFn);

        let int64 = new KB_INT64(low, high);

        if (isNegative) {
            let l = (~int64.low + 1) >>> 0;
            let h = (~int64.high + (l === 0 ? 1 : 0)) >>> 0;
            int64.low = l;
            int64.high = h;
            int64._isNegative = true;
        }

        return int64;
    }
}
export class KB_UINT64 {
    low: number;
    high: number;

    constructor(low: number, high: number) {
        this.low = low >>> 0;
        this.high = high >>> 0;
    }

    toNumber(): number {
        return this.high * TWO_PWR_32_DBL + this.low;
    }

    toBigInt(): bigint {
        return (BigInt(this.high) << 32n) | BigInt(this.low);
    }

    toString(radix: number = 10): string {
        return this.toBigInt().toString(radix);
    }

    equals(other: KB_UINT64): boolean {
        return this.toBigInt() === other.toBigInt();
    }

    static fromNumber(num: number): KB_UINT64 {
        const low = num >>> 0;
        const high = Math.floor(num / TWO_PWR_32_DBL) >>> 0;
        return new KB_UINT64(low, high);
    }

    static fromBigInt(bi: bigint): KB_UINT64 {
        const low = Number(bi & 0xFFFFFFFFn);
        const high = Number((bi >> 32n) & 0xFFFFFFFFn);
        return new KB_UINT64(low, high);
    }
}
