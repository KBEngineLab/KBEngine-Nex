
import KBELog from "./KBELog";

export function UTF8ArrayToString(array: Uint8Array): string
{
    let out = "";
    let char1: number;
    let char2: number;
    let char3: number;

    for(let i = 0; i < array.length;)
    {
        char1 = array[i];
        switch(char1 >> 4)
        {
            case 0:
            case 1:
            case 2:
            case 3:
            case 4:
            case 5:
            case 6:
            case 7:
                out += String.fromCharCode(char1);
                i += 1;
                break;
            case 12:
            case 13:
                char2 = array[i + 1];
                out += String.fromCharCode(((char1 & 0x1F) << 6) | (char2 & 0x3F));
                i += 2;
                break;
            case 14:
                char2 = array[i + 1];
                char3 = array[i + 2];
                out += String.fromCharCode( (char1 & 0x0F) << 12 | (char2 & 0x3F) << 6 | (char3 & 0x3F) << 0);
                i += 3;
                break;
            default:
                KBELog.ERROR_MSG("UTF8ArrayToString::execute flow shouldnt reach here.");
        }
    }

    return out;
}

export function StringToUTF8Array(value: string): Uint8Array
{
    let utf8 = new Array<number>();

    for (let i = 0; i < value.length; i++) 
    {
        let charcode = value.charCodeAt(i);
        if (charcode < 0x80) 
        {
            utf8.push(charcode);
        }
        else if (charcode < 0x800) 
        {
            utf8.push(0xc0 | (charcode >> 6), 
                      0x80 | (charcode & 0x3f));
        }
        else if (charcode < 0xd800 || charcode >= 0xe000) 
        {
            utf8.push(0xe0 | (charcode >> 12), 
                      0x80 | ((charcode>>6) & 0x3f), 
                      0x80 | (charcode & 0x3f));
        }
        // surrogate pair
        else
        {
            i++;
            // UTF-16 encodes 0x10000-0x10FFFF by
            // subtracting 0x10000 and splitting the
            // 20 bits of 0x0-0xFFFFF into two halves
            charcode = 0x10000 + (((charcode & 0x3ff)<<10)
                      | (value.charCodeAt(i) & 0x3ff))
            utf8.push(0xf0 | (charcode >>18), 
                      0x80 | ((charcode>>12) & 0x3f), 
                      0x80 | ((charcode>>6) & 0x3f), 
                      0x80 | (charcode & 0x3f));
        }
    }

    return new Uint8Array(utf8);
}