# 使用 Ubuntu 最新版作为基础镜像
FROM ubuntu:latest

# 设置环境变量
ENV KBE_ROOT=/KBE
ENV KBE_RES_PATH=${KBE_ROOT}/kbe/res/:${KBE_ROOT}/server_assets/:${KBE_ROOT}/server_assets/scripts/:${KBE_ROOT}/server_assets/res/
ENV KBE_BIN_PATH=${KBE_ROOT}/kbe/bin/server/


# 创建工作目录
RUN mkdir -p /KBE

# 将当前目录内容复制进容器
ADD . /KBE

# 设置工作目录
WORKDIR /KBE

# 运行编译脚本
RUN sed -i 's/sudo //g' /KBE/install_linux.sh && bash /KBE/install/install_linux.sh


# 清理编译缓存和不必要文件
RUN rm -rf /KBE/kbe/src/build \
           /root/kbe-vcpkg \
           /tmp/* /var/tmp/*

RUN apt-get clean && rm -rf /var/lib/apt/lists/* /tmp/* /var/tmp/*


# login base app  客户端通讯端口
EXPOSE 20013-20025/tcp

# base UDP 客户端通讯端口
EXPOSE 20005-20009/udp

# machine  接收广播端口
EXPOSE 20086/tcp
# machine  服务发现广播端口
EXPOSE 20087/tcp
# machine  对外提供 TCP 服务端口
EXPOSE 20088/tcp


# machine 客户端通讯端口
EXPOSE 20099-20102/tcp


#interfaces telnet
EXPOSE 33000-33002/tcp

#dbmgr telnet
EXPOSE 32000/tcp

#cellapp telnet
EXPOSE 50000-50002/tcp

# baseapp telnet
EXPOSE 40000-40002/tcp

#loginapp telnet
EXPOSE 31000-31002/tcp

#bots telnet
EXPOSE 51000-51002/tcp


WORKDIR /KBE/server_assets/scripts
CMD ["sh", "-c", "mkdir -p ./logs && ( ./start_server.sh ) & sleep 2 && tail -F ./logs/*"]