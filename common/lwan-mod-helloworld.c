#include "lwan.h"

static void *helloworld_init_from_hash(const char *prefix, const struct hash *hash)
{
    return NULL;
}

static enum lwan_http_status
helloworld_handle_cb(struct lwan_request *request,
                   struct lwan_response *response,
                   void *data)
{
    response->mime_type = "text/plain";

    //char helo[102400] = {0};
    
    char msg[] = "hello world from lwan module";
    strbuf_append_str(response->buffer, msg, strlen(msg));
    

    return HTTP_OK;
}

const struct lwan_module *lwan_module_helloworld(void)
{
        static const struct lwan_module helloworld_module = {
        .init = NULL,
        .init_from_hash = helloworld_init_from_hash,
        .shutdown = NULL,
        .handle = helloworld_handle_cb,
        .flags = 0
    };

    return &helloworld_module;
}