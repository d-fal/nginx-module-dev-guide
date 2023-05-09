### Nginx extended auth request module

The nginx auth request module is a helpful module when you want to authenticate your requests before resolving them. [Nginx has already have the module added](http://nginx.org/en/docs/http/ngx_http_auth_request_module.html) and you can use it like this: 

```
     location = /auth {

     }

```