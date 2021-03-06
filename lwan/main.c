/*
 * lwan - simple web server
 * Copyright (c) 2012 Leandro A. F. Pereira <leandro@hardinfo.org>
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version 2
 * of the License, or any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301, USA.
 */

#define _GNU_SOURCE
#include <getopt.h>
#include <stdlib.h>
#include <unistd.h>
#include <limits.h>

#include "lwan.h"
#include "lwan-mod-serve-files.h"

enum args {
    ARGS_FAILED,
    ARGS_USE_CONFIG,
    ARGS_SERVE_FILES
};

static enum args
parse_args(int argc, char *argv[], struct lwan_config *config, char *root)
{
    static const struct option opts[] = {
        { .name = "root", .has_arg = 1, .val = 'r' },
        { .name = "listen", .has_arg = 1, .val = 'l' },
        { .name = "help", .val = 'h' },
        { .name = "config", .has_arg = 1, .val = 'c' },
        { }
    };
    int c, optidx = 0;
    enum args result = ARGS_USE_CONFIG;

    while ((c = getopt_long(argc, argv, "hr:l:c:", opts, &optidx)) != -1) {
        switch (c) {
        case 'c':
            // 开始config->config_file_path为NULL，free NULL是没什么问题的
            free(config->config_file_path);
            config->config_file_path = strdup(optarg);
            result = ARGS_USE_CONFIG;
            break;

        case 'l':
            free(config->listener);
            config->listener = strdup(optarg);
            result = ARGS_SERVE_FILES;
            break;

        case 'r': {
            size_t len = strlen(optarg);

            if (len >= PATH_MAX) {
                fprintf(stderr, "Root path length exeeds %d characters\n", PATH_MAX);
                return ARGS_FAILED;
            }

            memcpy(root, optarg, len + 1);
            result = ARGS_SERVE_FILES;
            break;
        }

        case 'h':
            printf("Usage: %s [--root /path/to/root/dir] [--listener addr:port]\n", argv[0]);
            printf("\t[--config]\n");
            printf("Serve files through HTTP.\n\n");
            printf("Defaults to listening on %s, serving from ./wwwroot.\n\n", config->listener);
            printf("Options:\n");
            printf("\t-r, --root      Path to serve files from (default: ./wwwroot).\n");
            printf("\t-l, --listener  Listener (default: %s).\n", config->listener);
            printf("\t-c, --config    Path to config file path.\n");
            printf("\t-h, --help      This.\n");
            printf("\n");
            printf("Examples:\n");
            printf("  Serve system-wide documentation: %s -r /usr/share/doc\n", argv[0]);
            printf("        Serve on a different port: %s -l '*:1337'\n", argv[0]);
            printf("\n");
            printf("Report bugs at <https://github.com/lpereira/lwan>.\n");
            return ARGS_FAILED;

        default:
            printf("Run %s --help for usage information.\n", argv[0]);
            return ARGS_FAILED;
        }
    }

    return result;
}

int
main(int argc, char *argv[])
{
    struct lwan l;
    struct lwan_config c;
    char root[PATH_MAX];            // 默认服务的目录
    int ret = EXIT_SUCCESS;

    if (!getcwd(root, PATH_MAX))
        return 1;

    c = *lwan_get_default_config();
    c.listener = strdup("*:8080");

    switch (parse_args(argc, argv, &c, root)) {
    case ARGS_SERVE_FILES:
        lwan_status_info("Serving files from %s", root);
        lwan_init_with_config(&l, &c);

        const struct lwan_url_map map[] = {
            { .prefix = "/", SERVE_FILES(root) },
            { }
        };
        lwan_set_url_map(&l, map);
        break;
    case ARGS_USE_CONFIG:
        if (c.config_file_path)
            lwan_init_with_config(&l, &c);
        else
            lwan_init(&l);
        break;
    case ARGS_FAILED:
        ret = EXIT_FAILURE;
        goto out;
    }

    lwan_main_loop(&l);
    lwan_shutdown(&l);

out:
    // 可能导致double free问题
    // 原因是在lwan_shutdown函数里面，下面判断总是成立
    //     if (l->config.listener != default_config.listener)
    //         free(l->config.listener);
    // 已经提交issue https://github.com/lpereira/lwan/issues/197
    // 不知道是个人表达原因，还是作者原因，作者的修复没有修复到重点
    // 这里只是粗暴注释掉，原因是程序已经退出了，动态分配的内存被OS回收

    //free(c.listener);
    //free(c.config_file_path);

    return ret;
}
