#!/bin/sh

set -e 

DEFAULT_NGINX_VERSION="1.25.2"
# NGINX installation mode: 
# 0) do not install NGINX
# 1) install the nginx from source. The version is define by $NGINX_VERSION
# 2) update the mirrors and install the latest
# 3) install Nginx from the default mirrors
NGINX_INSTALL_MODE=
NGINX_VERSION=


for ARG in "$@"
do
   key=$(echo "$ARG" | cut -f1 -d=)
   val=$(echo "$ARG" | cut -f2 -d=)
   printf "k:v = <%s,%s>\n" "$key" "$val"
   case "$key" in 
   "NGINX_VERSION") NGINX_VERSION="$val";;
   "NGINX_INSTALL_MODE") NGINX_INSTALL_MODE="$val";;
   *) printf "invalid argument '%s'\n" "$key"
   exit 1;;
   esac
done

if [ -z "$NGINX_VERSION" ]; then
    NGINX_VERSION="$DEFAULT_NGINX_VERSION"
fi

if [ -z "$NGINX_INSTALL_MODE" ]; then
    NGINX_INSTALL_MODE=1
fi

NGINX_DIR="nginx-$NGINX_VERSION"

if [ ! -d "$NGINX_DIR" ];
then
    wget "https://nginx.org/download/nginx-${NGINX_VERSION}.tar.gz"
    tar -xzvf "${NGINX_DIR}.tar.gz" && rm "${NGINX_DIR}.tar.gz" 
fi

# skip manual compile 
case "$NGINX_INSTALL_MODE" in
    0)
    echo "Skipping manual compile. You should install Nginx later in the docker container."
    ;;
    1)
    useradd -r nginx
    mkdir -p /usr/lib/nginx/modules
    mkdir -p /var/cache/nginx

    cd "$NGINX_DIR" && ./configure --prefix=/etc/nginx \
    --sbin-path=/usr/sbin/nginx  \
    --modules-path=/usr/lib/nginx/modules  \
    --conf-path=/etc/nginx/nginx.conf  \
    --error-log-path=/var/log/nginx/error.log  \
    --http-log-path=/var/log/nginx/access.log  \
    --pid-path=/var/run/nginx.pid  \
    --lock-path=/var/run/nginx.lock \
    --http-client-body-temp-path=/var/cache/nginx/client_temp \
    --http-proxy-temp-path=/var/cache/nginx/proxy_temp  \
    --http-fastcgi-temp-path=/var/cache/nginx/fastcgi_temp  \
    --http-uwsgi-temp-path=/var/cache/nginx/uwsgi_temp  \
    --http-scgi-temp-path=/var/cache/nginx/scgi_temp  \
    --user=nginx --group=nginx \
    --with-debug \
    --with-compat \
    --with-file-aio  \
    --with-threads  \
    --with-http_addition_module   \
    --with-http_dav_module  \
    --with-http_flv_module  \
    --with-http_gunzip_module  \
    --with-http_gzip_static_module  \
    --with-http_mp4_module  \
    --with-http_random_index_module \
    --with-http_realip_module  \
    --with-http_secure_link_module  \
    --with-http_slice_module  \
    --with-http_ssl_module  \
    --with-http_stub_status_module  \
    --with-http_sub_module  \
    --with-http_v2_module  \
    --with-mail  \
    --with-mail_ssl_module  \
    --with-stream  \
    --with-stream_realip_module  \
    --with-http_auth_request_module \
    --with-stream_ssl_module \
    --with-stream_ssl_preread_module  \
    --with-ld-opt='-Wl,-z,relro -Wl,-z,now -Wl,--as-needed -pie' 

    make  && make install && make clean

    ln -s /usr/lib/nginx/modules/ /etc/nginx/modules
    ;;
    2)
    echo "use the newer pre-built Nginx"
    curl https://nginx.org/keys/nginx_signing.key | gpg --dearmor \
    | tee /usr/share/keyrings/nginx-archive-keyring.gpg >/dev/null
    gpg --dry-run --quiet --no-keyring --import --import-options import-show /usr/share/keyrings/nginx-archive-keyring.gpg
    echo "deb [signed-by=/usr/share/keyrings/nginx-archive-keyring.gpg] \
        http://nginx.org/packages/mainline/ubuntu $(lsb_release -cs) nginx" \
        | tee /etc/apt/sources.list.d/nginx.list && apt update && apt install nginx -y
        ;;
    3)
        apt install nginx -y;;
    *)
        printf "invalid value for NGINX_INSTALL MODE: \"%s\"\n" "$NGINX_INSTALL_MODE"
        echo """
            0) skip installing NGINX
            1) install the nginx from source. The version is define by NGINX_VERSION.
            2) update the mirrors and install the latest
            3) install Nginx from the default mirrors """
        exit 1
    ;;
esac