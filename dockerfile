FROM --platform=linux/amd64 ubuntu:20.04

# FROM --platform=linux/arm64 mdsol/ubuntu20:latest
# --platform=x86-64
# --platform=linux/amd64
# --platform=arm64



RUN apt-get update && \
apt-get install -y software-properties-common && \
add-apt-repository ppa:ubuntu-toolchain-r/test && \
# 安装GCC
apt-get install -y gcc g++ && \
apt-get install -y make && \
# 安装必备服务
apt-get install -y libssl-dev && \
apt-get install -y libtirpc-dev && \
apt-get install -y libtool && \
apt-get install -y libmysqlclient-dev && \
apt-get install -y autoconf && \
apt-get clean && rm -rf /var/lib/apt/lists/*

# RUN apt-get update
# RUN  apt-get  install  -y gcc build-essential

ENV KBE_ROOT=/KBE
ENV KBE_RES_PATH=${KBE_ROOT}/kbe/res/:${KBE_ROOT}/assets/:${KBE_ROOT}/assets/scripts/:${KBE_ROOT}/assets/res/
ENV KBE_BIN_PATH=${KBE_ROOT}/kbe/bin/server/



RUN mkdir /KBE
ADD . /KBE


WORKDIR /KBE/kbe/src

RUN make clean || true && make

WORKDIR /KBE


EXPOSE 20013
EXPOSE 20015


# ENTRYPOINT ["sh", "assets/start_server.sh"]


# 定义容器启动后执行的命令
# CMD ["bash", "-c", "chmod -R 775 assets && sh assets/start_server.sh"]
