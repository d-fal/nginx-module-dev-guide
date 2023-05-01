
SHELL=/bin/bash


NGINX_VERSION=$$(nginx -v 2>&1 | sed -n 's/.*\/\([0-9.]*\).*/\1/p')
MODULE_DIR = $(path)


MODULE_NAME=$$(grep 'ngx_addon_name' "${MODULE_DIR}/config" | sed -n 's/ngx_addon_name=\(.*\)/\1/p')
NGINX_DIR="nginx-${NGINX_VERSION}"
NGINX_MODULES_DIR=$$(nginx -V 2>&1 | sed -n 's/.*--modules-path=\([a-z\/]*\).*/\1/p')

required_commands = git wget tar cc

init: .prerequisite configure build .test
	@echo "nginx version ${NGINX_VERSION}"

module:
	@echo "module name : ${MODULE_NAME}"
hello-world:
	@if [ -z "${MODULE_DIR}" ]; then \
    MODULE_DIR="nginx-hello-world-module"; \
    if [ ! -d "$MODULE_DIR" ]; then \
        git clone https://github.com/perusio/nginx-hello-world-module.git; \
    fi \
	fi

nginx-download:
	@echo checking if nginx source code is ready ... 
	@if [ ! -d "${NGINX_DIR}" ]; then \
	   wget "https://nginx.org/download/nginx-${NGINX_VERSION}.tar.gz"; \
	   tar -xzvf "${NGINX_DIR}.tar.gz" && rm "${NGINX_DIR}.tar.gz";  \
	fi

configure:
	@cd "${NGINX_DIR}"; \
	./configure --with-compat --add-dynamic-module="../${MODULE_DIR}/" --with-debug 


.PHONY: build
build: .prerequisite
	@cd "${NGINX_DIR}";  \
	make modules && cp "objs/${MODULE_NAME}.so" "${NGINX_MODULES_DIR}";  \
	echo "module copied";  \
	echo "Leaving nginx-${NGINX_VERSION}"

	@if ! grep -Rq ''"${MODULE_NAME}"'.so' /etc/nginx/nginx.conf; then \
		sed -i '1iload_module modules/'"${MODULE_NAME}"'.so;' /etc/nginx/nginx.conf; \
		echo "module ${MODULE_NAME} loaded"; \
	fi


.prerequisite: init nginx-download
	@echo checking required tools ... 
	@(bash ./tools/sanity-check.sh)
ifndef NGINX_VERSION
	@echo "nginx not installed"
	@exit 1
endif

.test:
	@nginx -t

clean:
.PHONY: clean
	@cd "${NGINX_DIR}"; make clean



