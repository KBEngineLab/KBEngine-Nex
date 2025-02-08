FROM  --platform=linux/amd64 ubuntu:latest
# --platform=x86-64
# --platform=linux/amd64

RUN apt-get update
RUN apt-get install -y software-properties-common
RUN add-apt-repository ppa:ubuntu-toolchain-r/test
RUN apt-get update
# 安装GCC
RUN apt-get install -y gcc g++
RUN apt-get install -y make

# 安装必备服务
RUN apt-get install -y libssl-dev
RUN apt-get install -y libtirpc-dev
RUN apt-get install -y libtool
RUN apt-get install -y libmysqlclient-dev
RUN apt-get install -y autoconf

# RUN apt-get update
# RUN  apt-get  install  -y gcc build-essential

RUN mkdir /KBE
ADD . /KBE

WORKDIR /KBE/kbe/src
# RUN cd /KBE/kbe/src
RUN make clean || true
RUN make