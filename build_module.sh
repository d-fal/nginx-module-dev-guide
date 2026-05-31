#!/bin/bash

set -euo pipefail

NGINX_VERSION=$(nginx -v 2>&1 | sed -n 's/.*\/\([0-9.]*\).*/\1/p')
MODULE_DIR=$1

if [ -z "$1" ]
then
    echo "building module failed: no modules provided!"
    exit 1
fi


MODULE_NAME=$(grep 'ngx_addon_name' "$MODULE_DIR/config" | sed -n 's/ngx_addon_name=\(.*\)/\1/p')
NGINX_DIR="nginx-$NGINX_VERSION"
NGINX_MODULES_DIR=$(nginx -V 2>&1 | sed -n 's/.*--modules-path=\([a-z\/]*\).*/\1/p')
TEST_NGINX_CONFIG_FILE=sample_nginx.conf



echo "module name: $MODULE_NAME"

if [ ! -d "$NGINX_DIR" ];
then
    wget "https://nginx.org/download/nginx-${NGINX_VERSION}.tar.gz"
    tar -xzvf "${NGINX_DIR}.tar.gz" && rm "${NGINX_DIR}.tar.gz" 
fi


cd "$NGINX_DIR" 
./configure --with-compat --add-dynamic-module="../${MODULE_DIR}/" --with-debug --with-stream


make modules
cp "objs/${MODULE_NAME}.so" "$NGINX_MODULES_DIR" 


echo "module copied"
echo "Leaving nginx-$NGINX_VERSION"
cd ..

echo "check nginx config"

cat > $TEST_NGINX_CONFIG_FILE <<EOF
load_module modules/${MODULE_NAME}.so;
events {
   worker_connections  2;
}

http {
   server {
        listen 80;
    }
    
}
EOF
cat $TEST_NGINX_CONFIG_FILE
nginx -t -c $(pwd)/$TEST_NGINX_CONFIG_FILE

rm $(pwd)/$TEST_NGINX_CONFIG_FILE



