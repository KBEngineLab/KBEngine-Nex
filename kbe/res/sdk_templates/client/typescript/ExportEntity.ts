
import KBELog from "./KBELog";

// 把所有的entity脚本都注册到此处
var EntityScripts = {};
//EntityScripts[Account.className] = Account;

export function RegisterScript(scriptName: string, script: any)
{
    EntityScripts[scriptName] = script;
}

export function GetEntityScript(name: string)
{
    let script =  EntityScripts[name];
    if(script === undefined)
    {
        KBELog.WARNING_MSG("ExportEntity::GetEntityScript(%s) is undefined.", name);
    }

    return script;
}

