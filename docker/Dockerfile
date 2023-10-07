FROM ubuntu 


RUN apt update && apt install build-essential git wget \
    curl zlib1g-dev libpcre3-dev iputils-ping vim gnupg2 \
    ca-certificates lsb-release libssl-dev libgd-dev \
    libxml2 libxml2-dev uuid-dev -y

ARG NGINX_VERSION
ARG NGINX_INSTALL_MODE

WORKDIR /tmp
COPY install-nginx.sh .

RUN ./install-nginx.sh NGINX_VERSION=${NGINX_VERSION} NGINX_INSTALL_MODE=${NGINX_INSTALL_MODE}
RUN rm -rf /tmp/*

WORKDIR /code

ENTRYPOINT ["tail","-f","/dev/null"]





