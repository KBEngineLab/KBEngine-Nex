#!/bin/sh

# 项目路径
projectPath=$(cd ../; pwd)
if [ -n "$KBE_ROOT" ]; then
  :
else
    # 引擎路径
    export KBE_ROOT="$(cd ../../; pwd)"
fi


# 遍历子目录并查找 site-packages 目录
for dir in $(find "$projectPath" -type d -name "site-packages"); do
    echo "VENV: $dir"
    KBE_VENV_PATH="$KBE_VENV_PATH$dir;"
done

#RES路径
export KBE_RES_PATH="$KBE_ROOT/kbe/res/:$projectPath:$projectPath/res:$projectPath/"
#BIN路径
export KBE_BIN_PATH="$KBE_ROOT/kbe/bin/server/"
#虚拟环境路径
export KBE_VENV_PATH="$KBE_VENV_PATH"




echo KBE_ROOT = \"${KBE_ROOT}\"
echo KBE_RES_PATH = \"${KBE_RES_PATH}\"
echo KBE_BIN_PATH = \"${KBE_BIN_PATH}\"
echo KBE_VENV_PATH = \"${KBE_VENV_PATH}\"



sh ./kill_server.sh

"$KBE_BIN_PATH/machine" --cid=2129652375332859700 --gus=1&
"$KBE_BIN_PATH/logger" --cid=1129653375331859700 --gus=2&
"$KBE_BIN_PATH/interfaces" --cid=1129652375332859700 --gus=3&
"$KBE_BIN_PATH/dbmgr" --cid=3129652375332859700 --gus=4&
"$KBE_BIN_PATH/baseappmgr" --cid=4129652375332859700 --gus=5&
"$KBE_BIN_PATH/cellappmgr" --cid=5129652375332859700 --gus=6&
"$KBE_BIN_PATH/baseapp" --cid=6129652375332859700 --gus=7&
"$KBE_BIN_PATH/cellapp" --cid=7129652375332859700 --gus=8&
"$KBE_BIN_PATH/loginapp" --cid=8129652375332859700 --gus=9&
