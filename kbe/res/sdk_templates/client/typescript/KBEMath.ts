

export const FLOAT_MAX_VALUE = 3.402823466e+38;

export function Clamp(value: number, min: number, max: number): number
{
    if(min > max)
    {
        let mid = min;
        min = max;
        max = mid;
    }

    return value < min ? min : value > max? max: value;
}

export function Int8ToAngle(value: number, half: boolean): number
{
    return value * (Math.PI / (half ? 254.0 : 128.0));
}

export function AngleToInt8(value: number, half: boolean): number
{
    var angle = 0;
    if(!half)
    {
        angle = Math.floor((value * 128.0) / Math.PI + 0.5);
    }
    else
    {
        angle = Clamp(Math.floor( (value * 254.0) / Math.PI + 0.5), -128.0, 127.0);
    }

    return angle;
}

export class Vector2
{
    x: number;
    y: number;

    constructor(x: number, y:number)
    {
        this.x = x;
        this.y = y;
    }

    Distance(pos: Vector2)
    {
        let x = this.x - pos.x;
        let y = this.y - pos.y;

        return Math.sqrt(x * x + y * y);
    }
}

export class Vector3
{
    x: number;
    y: number;
    z: number;

    constructor(x: number = 0, y:number = 0, z:number = 0)
    {
        this.x = x;
        this.y = y;
        this.z = z;
    }


    Distance(pos: Vector3)
    {
        let x = this.x - pos.x;
        let y = this.y - pos.y;
        let z = this.z - pos.z;

        return Math.sqrt(x * x + y * y + z * z);
    }
}

export class Vector4
{
    x: number;
    y: number;
    z: number;
    w: number;

    constructor(x: number, y:number, z: number, w: number)
    {
        this.x = x;
        this.y = y;
        this.z = z;
        this.w = w;
    }
}
