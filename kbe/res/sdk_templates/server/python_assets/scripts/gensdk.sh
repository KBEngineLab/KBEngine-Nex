#!/bin/sh

currPath=$(pwd)
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

"$KBE_BIN_PATH/kbcmd" --clientsdk=unity --outpath="$currPath/kbengine_unity3d_plugins"
"$KBE_BIN_PATH/kbcmd" --clientsdk=ue4 --outpath="$currPath/kbengine_ue4_plugins"