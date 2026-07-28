#!/bin/sh

currPath=$(pwd)
keyStr="/kbengine/"

bcontain=`echo $currPath|grep $keyStr|wc -l`


if [ $bcontain = 0 ]
then
	export KBE_ROOT="$(cd ../; pwd)"
else
	export KBE_ROOT="$(pwd | awk -F "/kbengine/" '{print $1}')/kbengine"
fi

export KBE_RES_PATH="$KBE_ROOT/kbe/res/:$(pwd):$(pwd)/res:$(pwd)/scripts/"
export KBE_BIN_PATH="$KBE_ROOT/kbe/bin/server/"

echo KBE_ROOT = \"${KBE_ROOT}\"
echo KBE_RES_PATH = \"${KBE_RES_PATH}\"
echo KBE_BIN_PATH = \"${KBE_BIN_PATH}\"

# 四端必须顺序生成，任一失败立即返回，避免留下无法发布的半成品。
# Generate the four supported SDKs sequentially and fail fast to avoid partial release artifacts.
"$KBE_BIN_PATH/kbcmd" --clientsdk=csharp --outpath="$currPath/kbengine_csharp_sdk" || exit $?
"$KBE_BIN_PATH/kbcmd" --clientsdk=cxx --outpath="$currPath/kbengine_cxx_sdk" || exit $?
"$KBE_BIN_PATH/kbcmd" --clientsdk=typescript --outpath="$currPath/kbengine_typescript_sdk" || exit $?
"$KBE_BIN_PATH/kbcmd" --clientsdk=gdscript --outpath="$currPath/kbengine_gdscript_sdk" || exit $?
