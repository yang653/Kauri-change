# Force rebuild and re-pull source code
FROM debian:bullseye-slim
RUN apt-get update
RUN DEBIAN_FRONTEND=noninteractive apt-get -y install git gcc g++ make cmake libuv1-dev libssl-dev libsodium-dev autoconf libnet1-dev libtool pastebinit python3 bash gdb dnsutils nano inetutils-ping net-tools sudo iproute2
RUN git clone https://github.com/aixoss/gmp && cd gmp && ./configure && make install
ARG CACHEBUST=1
RUN git clone https://github.com/yang653/Kauri-change && cd Kauri-change && git checkout last && git submodule update --init --recursive && git submodule update --recursive --remote
RUN cd Kauri-change && cmake -DCMAKE_BUILD_TYPE=Release -DBUILD_SHARED=ON -DHOTSTUFF_PROTO_LOG=ON && make

ADD ./server.sh /

ENTRYPOINT ["/bin/bash", "/server.sh"]
