
export default class KBELog
{

    static DEBUG_MSG(msg: string, ...optionalParams: any[]): void
    {
        optionalParams.unshift(msg);
       
        console.debug.apply(this, optionalParams);
        
    }

    static INFO_MSG(msg: string, ...optionalParams: any[]): void
    {
        optionalParams.unshift(msg);
        
        console.info.apply(this, optionalParams);
    }

    static WARNING_MSG(msg: string, ...optionalParams: any[]): void
    {
        optionalParams.unshift(msg);
       
        console.warn.apply(this, optionalParams);
    }

    static ERROR_MSG(msg: string, ...optionalParams: any[]): void
    {
        optionalParams.unshift(msg);
      
        console.error.apply(this, optionalParams);
    }

    static ASSERT(condition?: boolean, message?: string, ...data: any[]): void
    {
        // 使用抛出异常的方式来实现类似断言功能
        if(!condition)
        {
            throw(new Error(message));
        }

        // note：微信小游戏平台不支持，手册中提到的CC_WECHATGAME未定义，无法区分是否微信小游戏平台，
        // console.assert(condition, message, ...data);
    }
}
