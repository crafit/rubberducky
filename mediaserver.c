/* system includes */
#include <stdio.h>
#include <errno.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>

#include <sys/types.h>
#include <sys/socket.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <netdb.h>
#include <fcntl.h>

/* lib includes */
#include <librtmp/log.h>
#include <librtmp/amf.h>
#include <ev.h>

/* local includes */
#include "mediaserver.h"
#include "rtmp.h"

#define BACKLOG           20

#define HOSTNAME "localhost"
#define RTMP_PORT_STRING  QUOTEVALUE(RTMP_PORT)

static int resolve_host(struct sockaddr_in *addr,
                        const char *hostname, const char *port)
{
    /* hostname lookup might not be needed */
    if (!inet_aton(hostname, &addr->sin_addr))
    {
        struct addrinfo hints, *res, *cur;
        memset(&hints, 0, sizeof(hints));
        hints.ai_family = AF_INET;
        hints.ai_socktype = SOCK_STREAM;
        if (getaddrinfo(hostname, port, NULL, &res)) {
            return -1;
        }

        /* only do ipv4 for now */
        for (cur = res; cur; cur = cur->ai_next) {
            if (cur->ai_family == AF_INET) {
                addr->sin_addr = ((struct sockaddr_in *)cur->ai_addr)->sin_addr;
                break;
            }
        }
        freeaddrinfo(res);
    }
    return 0;
}

static int setup_socket(const char *hostname, int port)
{
    int sockfd = socket(AF_INET, SOCK_STREAM, 0), tmp = 1;
    struct sockaddr_in addr = {0};
    const char *errstr;

    if (sockfd < 0) {
        sockfd = 0;
        errstr = "Failed to create socket";
        goto fail;
    }

    addr.sin_port = htons(port);
    setsockopt(sockfd, SOL_SOCKET, SO_REUSEADDR, &tmp, sizeof(tmp));

    if (resolve_host(&addr, HOSTNAME, QUOTEVALUE(RTMP_PORT))) {
        errstr = "Failed to resolve host";
        goto fail;
    }

    if (bind(sockfd, (struct sockaddr *)&addr, sizeof(addr))) {
        errstr = "Socket binding failed";
        goto fail;
    }

    if (listen(sockfd, BACKLOG)) {
        errstr = "Failed to listen to port";
        goto fail;
    }

    fprintf(stdout, "Starting server at %s:%d\n", HOSTNAME, RTMP_PORT);
    return sockfd;

fail:
    fprintf(stderr, "%s: %s\n", errstr, strerror(errno));
    if(sockfd) close(sockfd);
    return -errno;
}

static inline client_ctx* get_client(rtmp *r)
{
    return (client_ctx*)((uint8_t*)r - offsetof(client_ctx, rtmp_handle));
}

static void free_client(client_ctx *client)
{
    int i;
    srv_ctx *ctx = client->srv;
    client_ctx *c = ctx->clients, *p = NULL;
    while (c != client) { //TODO get rid of the list
        p = c;
        c = c->next;
    }
    if (!p)
        ctx->clients = c->next;
    else
        p->next = c->next;

    fprintf(stdout, "(%d) Disconnecting\n", c->id);

    // free streams in the server tree
    for (i = 0; i < RTMP_MAX_STREAMS; i++) {
        rtmp_stream *s = c->rtmp_handle.streams[i];
        if (s && s->name) {
            printf("Deleting stream %s\n", s->name);
            rxt_delete(s->name, ctx->streams);
        }
    }

    // if sending, make sure recipients know they're off the send list
    if (c->outgoing) {
        int i;
        for (i = 0; i < c->outgoing->max_recvs; i++) {
            if (c->outgoing->list[i]) {
                client_ctx *listener = get_client(c->outgoing->list[i]);
                assert(listener->incoming == c->outgoing);
                listener->incoming = NULL;
                c->outgoing->nb_recvs--;
            }
        }
        assert(!c->outgoing->nb_recvs);
    }

    // if listening, remove from stream send list
    if (c->incoming) {
        int i;
        for (i = 0; i < c->incoming->max_recvs; i++)
            if (c->incoming->list[i] == &(c->rtmp_handle)) {
                c->incoming->list[i] = NULL;
                c->incoming->nb_recvs--;
                break;
            }
    }

    rtmp_free(&c->rtmp_handle);
    if (c->outgoing) free(c->outgoing);
    ev_io_stop(ctx->loop, &c->read_watcher);
    free(c);
    ctx->connections--;
}

static void free_all(srv_ctx *ctx)
{
    client_ctx *c = ctx->clients;

    //TODO Get rid of the list? Options?
    while (c) {
        client_ctx *d = c;
        c = c->next;
        free_client(d);
    }
    ctx->clients = NULL;

    close(ctx->fd);
    ev_io_stop(ctx->loop, &ctx->io);
    ev_unloop(ctx->loop, EVUNLOOP_ALL);
    rxt_free(ctx->streams);
    ctx->streams = NULL;
    free(ctx);
    ctx = NULL;
    fprintf(stdout, "Shutting down\n");
}

static void close_cb(struct ev_loop *loop, ev_signal *signal, int revents)
{
    free_all((srv_ctx*)signal->data);
}

static void rd_rtmp_close_cb(rtmp *r)
{
    free_client(get_client(r));
}

static void rd_rtmp_publish_cb(rtmp *r, rtmp_stream *stream)
{
#define MAX_CLIENTS 10
    client_ctx *client;
    recv_ctx *recvs;
    srv_ctx *srv;

    client = get_client(r);
    recvs = malloc(MAX_CLIENTS * sizeof(rtmp*) + sizeof(recv_ctx));
    srv = client->srv;
    if (!recvs) {
        fprintf(stderr, "Out of memory when mallocing receivers!\n");
        return; // TODO something drastic
    }
    memset(recvs, 0, MAX_CLIENTS * sizeof(rtmp*) + sizeof(recv_ctx));
    recvs->stream = stream;
    recvs->max_recvs = MAX_CLIENTS;
    recvs->list = (rtmp**)(recvs + 1);
    client->outgoing = recvs;

    rxt_put(stream->name, client, srv->streams);
#undef MAX_CLIENTS
}

static rtmp_stream* rd_rtmp_play_cb(rtmp *r, char *stream_name)
{
    client_ctx *listener = get_client(r);
    srv_ctx *srv = listener->srv;
    client_ctx *client = rxt_get(stream_name, srv->streams);
    recv_ctx *recvs;
    const char *errstr;
    int i;
    if (!client) {
        errstr = "Couldn not find client!\n";
        goto play_fail;
    }
    recvs = client->outgoing;
    listener->incoming = recvs;
    if (!recvs) {
        errstr = "Could not find recvs!\n";
        goto play_fail;
    }
    for (i = 0; i < recvs->max_recvs; i++)
        if (!recvs->list[i]) {
            r->keyframe_pending = recvs->stream->vcodec == 2; // h263 only
            recvs->list[i] = r;
            recvs->nb_recvs++;
            return recvs->stream;
        }

    errstr = "Receiver list full!\n";

play_fail:
    fprintf(stderr, "Error processing play request: %s\n", errstr);
    return NULL;
}

static int is_keyframe(rtmp *listener, rtmp_packet *pkt)
{
  if (0x10 == (0xf0 & *pkt->body)) {
        // for the very first frame its probably better
        // to send as a large 12byte header w/ msg id and all
      listener->keyframe_pending = 0;
      printf("KEYFRAME FOUND\n");
    }
    return !listener->keyframe_pending;
}

static void rd_rtmp_read_cb(rtmp *r, rtmp_packet *pkt)
{
    if (pkt->msg_type == 0x08 || pkt->msg_type == 0x09) {
    client_ctx *client = get_client(r);
    recv_ctx *recv = client->outgoing;
    int i, j;
    if (!recv) return;

    for (i = j = 0; j < recv->nb_recvs; i++)
        if (recv->list[i]){

            // for receiver's first packet(s), skip up to keyframe
            if (pkt->msg_type == 0x09 &&
                recv->list[i]->keyframe_pending &&
                !is_keyframe(recv->list[i], pkt)) {
                return;
            }

            // memcpy every packet for each client? VOMIT
            rtmp_packet packet;
            memcpy(&packet, pkt, sizeof(rtmp_packet));
            packet.body = pkt->body;
            rtmp_send(recv->list[i], &packet);
            j++;
        }
    }
}

static void rd_rtmp_delete_cb(rtmp *r, rtmp_stream *s)
{
    client_ctx *client = get_client(r);
    srv_ctx *srv = client->srv;
    if (s->name)
        rxt_delete(s->name, srv->streams);
}

static void incoming_cb(struct ev_loop *loop, ev_io *io, int revents)
{
    int clientfd;
    socklen_t len = 0; // weird type to sate the compiler
    srv_ctx *ctx = io->data;
    client_ctx *client;
    struct sockaddr_in addr = {0};
    const char *errstr;

    /* accept cxn, alloc space and setup client context */
    if ((clientfd = accept(ctx->fd,
                           (struct sockaddr *)&addr, &len)) < 0) {
        clientfd = 0;
        errstr = strerror(errno);
        goto fail;
    }

    if (!(client = malloc(sizeof(client_ctx)))) {
        errstr = "Failed to allocate memory for client cxn.";
        goto fail;
    }
    client->next = ctx->clients;
    ctx->clients = client;
    ctx->connections++;
    client->srv = ctx;
    client->id = ctx->total_cxns++;
    client->incoming = client->outgoing = NULL;

    rtmp_init(&client->rtmp_handle);
    client->rtmp_handle.fd = clientfd;
    client->read_watcher.data = &client->rtmp_handle;
    client->rtmp_handle.close_cb = rd_rtmp_close_cb;
    client->rtmp_handle.publish_cb = rd_rtmp_publish_cb;
    client->rtmp_handle.delete_cb = rd_rtmp_delete_cb;
    client->rtmp_handle.play_cb = rd_rtmp_play_cb;
    client->rtmp_handle.read_cb = rd_rtmp_read_cb;

    fcntl(clientfd, F_SETFL, O_NONBLOCK);

    /* setup the events */
    ev_io_init(&client->read_watcher, rtmp_read, client->rtmp_handle.fd, EV_READ);
    ev_io_start(ctx->loop, &client->read_watcher);

    // we can convert this to a readable hostname later
    // during some postprocessing/analytics stage.
    fprintf(stdout, "(%d) Accepted connection from %d\n",
            client->id, clientfd);
    return;

fail:
    fprintf(stderr, "%s", errstr);
    if (clientfd) close(clientfd);
    if (ctx->clients == client)
        free_client(client);
}

static ev_signal signal_watcher_int;
static ev_signal signal_watcher_term;

static void setup_events(srv_ctx *ctx)
{
    sigset_t sigpipe;

    //XXX what does the auto method use?
    // select() offers better response times, but
    // epoll is MUCH more scalable
    ctx->loop = ev_default_loop(EVFLAG_AUTO);
    ctx->io.data = ctx;

    /* setup primary acceptor */
    ev_io_init(&ctx->io, incoming_cb, ctx->fd, EV_READ);
    ev_io_start(ctx->loop, &ctx->io);

    signal_watcher_int.data = ctx;
    ev_signal_init(&signal_watcher_int, close_cb, SIGINT);
    ev_signal_start(ctx->loop, &signal_watcher_int);

    signal_watcher_term.data = ctx;
    ev_signal_init(&signal_watcher_term, close_cb, SIGTERM);
    ev_signal_start(ctx->loop, &signal_watcher_term);

    // ignore SIGPIPE
    sigemptyset(&sigpipe);
    sigaddset(&sigpipe, SIGPIPE);
    sigprocmask(SIG_BLOCK, &sigpipe, NULL);
}

int main(int argc, char** argv)
{
    int serverfd = 0;
    const char *errstr;
    srv_ctx *ctx = malloc(sizeof(srv_ctx));

    if ((serverfd = setup_socket(HOSTNAME, RTMP_PORT)) < 0) {
        serverfd = 0;
        errstr = "Failed to set up socket";
        goto fail;
    }

    ctx->fd = serverfd;
    ctx->connections = 0;
    ctx->total_cxns = 0;
    ctx->clients = NULL;
    ctx->streams = rxt_init();
    setup_events(ctx);
    ev_loop(ctx->loop, EVBACKEND_EPOLL);

    return 0;

fail:
    fprintf(stderr, "%s: %s\n", errstr, strerror(errno));
    return 0;
}
