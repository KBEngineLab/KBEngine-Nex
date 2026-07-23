
export enum EntityDataFlags
{
    ED_FLAG_UNKOWN													= 0x00000000, // 未定义
    ED_FLAG_CELL_PUBLIC												= 0x00000001, // 相关所有cell广播
    ED_FLAG_CELL_PRIVATE											= 0x00000002, // 当前cell
    ED_FLAG_ALL_CLIENTS												= 0x00000004, // cell广播与所有客户端
    ED_FLAG_CELL_PUBLIC_AND_OWN										= 0x00000008, // cell广播与自己的客户端
    ED_FLAG_OWN_CLIENT												= 0x00000010, // 当前cell和客户端
    ED_FLAG_BASE_AND_CLIENT											= 0x00000020, // base和客户端
    ED_FLAG_BASE													= 0x00000040, // 当前base
    ED_FLAG_OTHER_CLIENTS											= 0x00000080, // cell广播和其他客户端
}


export class Property {
  name: string = '';
  utype: any = null;
  properUtype: number = 0;
  properFlags: number = 0;
  aliasID: number = 0;
  defaultValStr: any = '';
  setHandler?: Function;
  value: any;
  isBase(): boolean { 
    return this.properFlags == EntityDataFlags.ED_FLAG_BASE_AND_CLIENT || this.properFlags == EntityDataFlags.ED_FLAG_BASE;
  }
  isOwnerOnly(): boolean { 
    return this.properFlags == EntityDataFlags.ED_FLAG_CELL_PUBLIC_AND_OWN || this.properFlags == EntityDataFlags.ED_FLAG_OWN_CLIENT;
  }
  isOtherOnly(): boolean { 
    return this.properFlags == EntityDataFlags.ED_FLAG_OTHER_CLIENTS;
  }
} 