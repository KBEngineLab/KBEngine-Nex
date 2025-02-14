FROM  --platform=linux/amd64 asappinc/ubuntu20:latest
# --platform=x86-64
# --platform=linux/amd64

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
apt-get clean

# RUN apt-get update
# RUN  apt-get  install  -y gcc build-essential

RUN mkdir /KBE
ADD . /KBE

WORKDIR /KBE/kbe/src
# RUN cd /KBE/kbe/src
RUN make clean || true
RUN make