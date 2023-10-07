# Nginx module development environment

## Disclaimer 

Module development with Nginx is not a easy task. There are couple of problems contributing to this. In one hand scarcity of updated tutorials is highly being felt and on the other hand, **Nginx** is not welcoming creation of modules instead of delving into configurations and using existing mechanisms to achinve a task. Although the latter is highly valid, there should be some tutorials and references to make the very first steps into module development easier.


## How to use this project

### Run in Docker container
This is the clean way to play around with nginx. In this tutorial, we use a docker container and develop our nginx modules in the container.

> Disclaimer: We try to use the latest Nginx versions but the audience don't have to stick to the latest releases.

* Step 1: build the docker image
in this step we are going to use an ubuntu image and install necessary packages on it. Later, we will compile the nginx from source and install it. Installing from source might not be likeable by many users, but we recommend doing so because many of pre-built packages are being shipped without logging and debuging options. Moreover, you might need to contribute to the source code or default modules in the Nginx source code. 

In order to build the image, do the following:

```bash
   $ cd docker
   $ docker build . -t ngx-mod-dev-img
```
The above command builds the docker image and compile the nginx version ```1.25.2``` from source and then installs it. In the case you want to install a different version of Nginx, you can pass ```NGINX_VERSION``` as a build argument. 

```bash
    $ docker build . -t ngx-mod-dev-img --build-arg="NGINX_VERSION=1.21.6"
```
It will download and compile nginx version 1.21.6 and installs it accordingly.
There is another option if you don't want to compile the nginx. You can use ```NGINX_INSTALL_MODE``` which determines the way you want to make the image. The possible options for ```NGINX_INSTALL_MODE``` are as below:

    # 0) do not install NGINX
    # 1) install the nginx from source. The version is define by $NGINX_VERSION
    # 2) update the mirrors and install the latest
    # 3) install Nginx from the default mirrors

```bash
#  do not install Nginx
 $ docker build . -t ngx-mod-dev-img --build-arg="NGINX_VERSION=1.21.6" --build-arg="NGINX_INSTALL_MODE=0"
```

### Run the dev container
Having the image built, you can start your development container and start development. There are some options to develop on containers. One is to mount a volume to the container and develope in your favorite IDE and run your code in the container.

```bash
    $ docker run -it -d --net=host -v $PWD:/code --name ngx-mod-dev-container ngx-mod-dev-img:latest
    # now you can run the container
    $ docker exec -it ngx-mod-dev-img
```

In the case you are using [Visual Studio Code](https://code.visualstudio.com/), you can use [remote container extension](https://code.visualstudio.com/docs/devcontainers/containers).  

### Develop the first module

In order to create your first module, it's necessary to learn about the mechanicsm of Nginx dynamic modules in general. Each dynamic module consists of two essential parts: 
* config file
* module_code.c

For example, the following tree shows the content of our basic ```ngx_http_sample_module``` module.
```
├── sample-module
│   ├── config
│   └── ngx_http_sample_module.c
```

Let's dive in and see what's inside each file.

### config file

Content of this file is as follows: 

```bash
ngx_addon_name=ngx_http_sample_module
if test -n "$ngx_module_link"; then
    ngx_module_type=HTTP
    ngx_module_name=ngx_http_sample_module
    ngx_module_srcs="$ngx_addon_dir/ngx_http_sample_module.c"

    . auto/module
else
    HTTP_MODULES="$HTTP_MODULES ngx_http_sample_module"
    NGX_ADDON_SRCS="$NGX_ADDON_SRCS $ngx_addon_dir/ngx_http_sample_module.c"
fi
```

This is the [new style config](https://www.nginx.com/resources/wiki/extending/converting/) that supports both dynamic and static modules. For simplicity, we only use new style for our [greetings module](./examples/01-greeting/config).


### ngx_http_sample_module.c
This file is were we pile our module code in. This file has a very important struct of type ```ngx_module_t``` which should be name exactly as what you have named your module in config file.
```c
ngx_module_t ngx_http_sample_module= {
    NGX_MODULE_V1,
    /* module context */
    &ngx_http_sample_module_ctx,        /* void *ctx  */
    /* module directives */
    ngx_http_sample_module_commands,    /* ngx_command_t *command */
    /* module type */
    NGX_HTTP_MODULE,                    /* type /*
    /* init master */
    NULL,                               /* (*init_master)(ngx_log_t *log); */
    /* init module */
    NULL,                               /* (*init_module)(ngx_cycle_t *cycle); */
    /* init process */
    NULL,                               /*  (*init_process)(ngx_cycle_t *cycle); */
    /* init thread */
    NULL,                               /* (*init_thread)(ngx_cycle_t *cycle); */
    /* exit thread */
    NULL,                               /* (*exit_thread)(ngx_cycle_t *cycle); */
    /* exit process */
    NULL,                               /* (*exit_process)(ngx_cycle_t *cycle); */
    /* exit master */
    NULL,                               /* (*exit_master)(ngx_cycle_t *cycle); */
    NGX_MODULE_V1_PADDING};
```

This struct bridges Nginx and your dynamic module. Each module stores it private data in ctx field. Module configurations are defined in command array. Your dynamic module can be [invoked in some stages](http://nginx.org/en/docs/dev/development_guide.html#core_modules) in nginx lifecycle. The module lifecycle consists of the following events:


> Configuration directive handlers are called as they appear in configuration files in the context of the master process.

> After the configuration is parsed successfully, ```init_module``` handler is called in the context of the master process. The init_module handler is called in the master process each time a configuration is loaded.

> The master process creates one or more worker processes and the ```init_process``` handler is called in each of them.

> When a worker process receives the shutdown or terminate command from the master, it invokes the ```exit_process``` handler.

> The master process calls the ```exit_master``` handler before exiting.

> Because threads are used in nginx only as a supplementary I/O facility with its own API, init_thread and ```exit_thread``` handlers are not currently called. There is also no ```init_master``` handler, because it would be unnecessary overhead.


