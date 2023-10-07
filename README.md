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
   $ docker build . -t ngx-mod-dev
```
The above command builds the docker image and compile the nginx version ```1.25.2``` from source and then installs it. In the case you want to install a different version of Nginx, you can pass ```NGINX_VERSION``` as a build argument. 

```bash
    $ docker build . -t ngx-mod-dev --build-arg="NGINX_VERSION=1.21.6"
```
It will download and compile nginx version 1.21.6 and installs it accordingly.
There is another option if you don't want to compile the nginx. You can use ```NGINX_INSTALL_MODE``` which determines the way you want to make the image. The possible options for ```NGINX_INSTALL_MODE``` are as below:

    # 0) do not install NGINX
    # 1) install the nginx from source. The version is define by $NGINX_VERSION
    # 2) update the mirrors and install the latest
    # 3) install Nginx from the default mirrors

```bash
#  do not install Nginx
 $ docker build . -t ngx-mod-dev --build-arg="NGINX_VERSION=1.21.6" --build-arg="NGINX_INSTALL_MODE=0"
```

### Develop the first module
