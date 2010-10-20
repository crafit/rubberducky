#ifndef VIDEOAPI_MEDIASERVER_H
#define VIDEOAPI_MEDIASERVER_H

#include <ev.h>

#include "rtmp.h"

#ifndef videoapi_unused
#if defined(__GNUC__)
#   define videoapi_unused __attribute__((unused))
#else
#   define videoapi_unused
#endif
#endif

#define QUOTELITERAL(x) #x
#define QUOTEVALUE(x) QUOTELITERAL(x)

typedef struct {
    rtmp **list;
    int nb_recvs;
    int max_recvs;
}recv_ctx;

struct srv_ctx;

typedef struct client_ctx {
    int id;
    rtmp rtmp;
    recv_ctx *recvs;
    ev_io read_watcher;
    struct srv_ctx *srv;
    struct client_ctx *next;
}client_ctx;

typedef struct srv_ctx {
    int fd;
    int connections;
    int total_cxns;
    struct ev_loop *loop;
    ev_io io;             /* socket listener event */
    client_ctx *clients;
}srv_ctx;

// TODO temporary; refactor into rtmp
void rtmp_invoke(rtmp *rtmp, struct rtmp_packet *pkt, srv_ctx *ctx);

#endif //VIDEOAPI_MEDIASERVER_H
