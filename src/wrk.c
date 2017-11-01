// Copyright (C) 2012 - Will Glozer.  All rights reserved.

#include "wrk.h"
#include "script.h"
#include "main.h"
#include "hdr_histogram.h"
#include "stats.h"

/* Max recordable latency of 1 day */
#define MAX_LATENCY 24L * 60 * 60 * 1000000

/*---------------------------------------------------------------------------*/
static struct config
{
    uint64_t num_connections;   /* Number of connections to server  */
    uint64_t num_requests;      /* Number of requests per thread    */
    uint64_t num_threads;       /* Number of threads                */

    uint64_t duration;

    uint64_t timeout;
    uint64_t pipeline;

    uint64_t rate;
    uint64_t delay_ms;

    bool     delay;
    bool     dynamic;
    bool     latency;
    bool     u_latency;
    bool     record_all_responses;
    char    *host;
    char    *script;
    SSL_CTX *ctx;

    enum {
        PROTO_HTTP, PROTO_RAW
    } protocol;

} cfg;
/*---------------------------------------------------------------------------*/
static struct
{
    stats *requests;
    pthread_mutex_t mutex;
} statistics;
/*---------------------------------------------------------------------------*/
static struct sock sock =
{
    .connect  = sock_connect,
    .close    = sock_close,
    .read     = sock_read,
    .write    = sock_write,
    .readable = sock_readable
};
/*---------------------------------------------------------------------------*/
static struct http_parser_settings parser_settings =
{
    .on_message_complete = response_http_complete
};

static volatile sig_atomic_t stop = 0;

/*---------------------------------------------------------------------------*/
static void handler( int sig )
{
    stop = 1;
}
/*---------------------------------------------------------------------------*/
static void usage()
{
    printf("Usage: wrk <options> <url>                            \n"
           "  Options:                                            \n"
           "    -c, --connections <N>  Connections to keep open   \n"
           "    -d, --duration    <T>  Duration of test           \n"
           "    -t, --threads     <N>  Number of threads to use   \n"
           "    -r, --requests    <N>  Number of requests to use  \n"
           "                                                      \n"
           "    -s, --script      <S>  Load Lua script file       \n"
           "    -H, --header      <H>  Add header to request      \n"
           "    -L  --latency          Print latency statistics   \n"
           "    -U  --u_latency        Print uncorrceted latency statistics\n"
           "        --timeout     <T>  Socket/request timeout     \n"
           "    -B, --batch_latency    Measure latency of whole   \n"
           "                           batches of pipelined ops   \n"
           "                           (as opposed to each op)    \n"
           "    -v, --version          Print version details      \n"
           "    -R, --rate        <T>  work rate (throughput)     \n"
           "                           in requests/sec (total)    \n"
           "                           [Required Parameter]       \n"
           "                                                      \n"
           "                                                      \n"
           "  Numeric arguments may include a SI unit (1k, 1M, 1G)\n"
           "  Time arguments may include a time unit (2s, 2m, 2h)\n");
}
/*---------------------------------------------------------------------------*/
/*         Main entry point                                                  */
/*---------------------------------------------------------------------------*/
int main( int argc, char **argv )
{
    uint64_t i = 0;
    uint64_t connections = 0;
    uint64_t throughput  = 0;
    uint64_t stop_at     = 0;
    uint64_t runtime_us  = 0;
    char *runtime_msg = NULL;

    struct sigaction sa =
    {
        .sa_handler = handler,
        .sa_flags   = 0,
    };

    thread *ths = NULL;
    lua_State *L = NULL;
    char *url = NULL;
    char **headers = zmalloc(argc * sizeof(char *));
    struct http_parser_url parts = {};

    if( parse_args(&cfg, &url, &parts, headers, argc, argv) )
    {
        usage();
        exit(1);
    }

    char *schema  = copy_url_part(url, &parts, UF_SCHEMA);
    char *host    = copy_url_part(url, &parts, UF_HOST);
    char *port    = copy_url_part(url, &parts, UF_PORT);
    char *service = port ? port : schema;

    /* Set protocol (default) as HTTP */
    cfg.protocol = PROTO_HTTP;

    if( !strncmp("https", schema, 5) )
    {
        if( (cfg.ctx = ssl_init()) == NULL )
        {
            fprintf(stderr, "unable to initialize SSL\n");
            ERR_print_errors_fp(stderr);
            exit(1);
        }

        sock.connect  = ssl_connect;
        sock.close    = ssl_close;
        sock.read     = ssl_read;
        sock.write    = ssl_write;
        sock.readable = ssl_readable;
    }
    else if( !strncmp("raw", schema, 3) || !strncmp("raws", schema, 4) )
    {
        /* Set protocol as raw */
        cfg.protocol = PROTO_RAW;

        if( !strncmp("raws", schema, 4) )
        {
            sock.connect  = ssl_connect;
            sock.close    = ssl_close;
            sock.read     = ssl_read;
            sock.write    = ssl_write;
            sock.readable = ssl_readable;
        }
    }

    signal(SIGPIPE, SIG_IGN);
    signal(SIGINT,  SIG_IGN);

    pthread_mutex_init( &statistics.mutex, NULL );
    statistics.requests = stats_alloc(10);
    //statistics.requests = stats_alloc(MAX_THREAD_RATE_S);
    /* Allocate array threads */
    ths = zcalloc(cfg.num_threads * sizeof(thread));

    hdr_init(1, MAX_LATENCY, 3, &(statistics.requests->histogram));

    /* Create script */
    L = script_create(cfg.script, url, headers);

    if( !script_resolve(L, host, service) )
    {
        char *msg = strerror(errno);
        fprintf(stderr, "unable to connect to %s:%s %s\n", host, service, msg);
        exit(1);
    }

    /* Set host name */
    cfg.host = host;

    /* Calculate all needed connections */
    connections = cfg.num_connections / cfg.num_threads;

    if( cfg.num_requests == 0 )
    {
        stop_at = time_us() + (cfg.duration * 1000000);
        throughput  = cfg.rate / cfg.num_threads;
    }
    else
    {
        stop_at = 0;
        throughput  = UINT64_MAX;
    }

    /* Create threads */
    for( i = 0; i < cfg.num_threads; i++ )
    {
        thread *t        = &ths[i];
        t->loop          = aeCreateEventLoop(10 + cfg.num_connections * 3);
        t->connections   = connections;
        t->throughput    = throughput;;
        t->stop_at       = stop_at;
        t->max_nrequests = cfg.num_requests;

        t->L = script_create(cfg.script, url, headers);
        script_init(L, t, argc - optind, &argv[optind]);

        if( i == 0 )
        {
            if( cfg.protocol != PROTO_RAW )
                cfg.pipeline = script_verify_http_request(t->L);
            else
                cfg.pipeline = 1;

            cfg.dynamic  = !script_is_static(t->L);
            cfg.delay    = script_has_delay(t->L);

            if( script_want_response(t->L) )
            {
                parser_settings.on_header_field = header_field;
                parser_settings.on_header_value = header_value;
                parser_settings.on_body         = response_body;
            }
        }

        if( !t->loop || pthread_create(&t->thread, NULL, &thread_main, t) )
        {
            char *msg = strerror(errno);
            fprintf(stderr, "unable to create thread %"PRIu64": %s\n", i, msg);
            exit(2);
        }
    }

    sigfillset(&sa.sa_mask);
    sigaction(SIGINT, &sa, NULL);

    if( cfg.num_requests > 0 )
    {
        uint64_t requests = cfg.num_threads * cfg.num_requests;
        printf("Running requests %"PRIu64" test @ %s\n", requests, url);
        printf("  %"PRIu64" threads (%"PRIu64" requests) and %"PRIu64" connections\n", cfg.num_threads, cfg.num_requests ,cfg.num_connections);
    }
    else /* duration test */
    {
        char *time = format_time_s(cfg.duration);
        printf("Running duration %s test @ %s\n", time, url);
        printf("  %"PRIu64" threads (unlimited requests) and %"PRIu64" connections\n", cfg.num_threads, cfg.num_connections);
    }

    uint64_t start    = time_us();
    uint64_t complete = 0;
    uint64_t bytes    = 0;
    errors errors     = { 0 };

    struct hdr_histogram* latency_histogram;
    hdr_init(1, MAX_LATENCY, 3, &latency_histogram);
    struct hdr_histogram* u_latency_histogram;
    hdr_init(1, MAX_LATENCY, 3, &u_latency_histogram);

    /* Start to wait end of threads */
    for( i = 0; i < cfg.num_threads; i++ )
    {
        thread *t = &ths[i];
        pthread_join(t->thread, NULL);
    }

    runtime_us = time_us() - start;

    for( i = 0; i < cfg.num_threads; i++ )
    {
        thread *t = &ths[i];

        complete += t->complete;
        bytes    += t->bytes;

        errors.connect += t->errors.connect;
        errors.read    += t->errors.read;
        errors.write   += t->errors.write;
        errors.timeout += t->errors.timeout;
        errors.status  += t->errors.status;

        hdr_add(latency_histogram, t->latency_histogram);
        hdr_add(u_latency_histogram, t->u_latency_histogram);
    }

    long double runtime_s   = runtime_us / 1000000.0;
    long double req_per_s   = complete   / runtime_s;
    long double bytes_per_s = bytes      / runtime_s;

    stats *latency_stats = stats_alloc(10);
    latency_stats->min = hdr_min(latency_histogram);
    latency_stats->max = hdr_max(latency_histogram);
    latency_stats->histogram = latency_histogram;

    print_stats_header();
    print_stats("Latency", latency_stats, format_time_us);
    print_stats("Req/Sec", statistics.requests, format_metric);

    if (cfg.latency)
    {
        print_hdr_latency(latency_histogram, "Recorded Latency");
        printf("----------------------------------------------------------\n");
    }

    if( cfg.u_latency )
    {
        printf("\n");
        print_hdr_latency(u_latency_histogram,"Uncorrected Latency (measured without taking delayed starts into account)");
        printf("----------------------------------------------------------\n");
    }

    runtime_msg = format_time_us(runtime_us);

    printf("  %"PRIu64" requests in %s, %sB read\n", complete, runtime_msg, format_binary(bytes));

    if( errors.connect || errors.read || errors.write || errors.timeout )
    {
        printf("  Socket errors: connect %d, read %d, write %d, timeout %d\n",
               errors.connect, errors.read, errors.write, errors.timeout);
    }

    if( errors.status )
        printf("  Non-2xx or 3xx responses: %d\n", errors.status);

    printf("Requests/sec: %9.2Lf\n", req_per_s);
    printf("Transfer/sec: %10sB\n", format_binary(bytes_per_s));

    if( script_has_done(L) )
    {
        script_summary(L, runtime_us, complete, bytes);
        script_errors(L, &errors);
        script_done(L, latency_stats, statistics.requests);
    }

    return 0;
}
/*---------------------------------------------------------------------------*/
void *thread_main( void *arg )
{
    thread *th = (thread *)arg;
    char *request = NULL;
    size_t length = 0;
    connection *c = NULL;
    double throughput = 0.0f;
    aeEventLoop *loop = th->loop;

    th->cs = zcalloc(th->connections * sizeof(connection));

    tinymt64_init(&th->rand, time_us());
    hdr_init(1, MAX_LATENCY, 3, &th->latency_histogram);
    hdr_init(1, MAX_LATENCY, 3, &th->u_latency_histogram);

    /* Create request message */
    if( !cfg.dynamic )
        script_request(th->L, &request, &length);

    throughput = (th->throughput / 1000000.0) / th->connections;

    c = th->cs;

    for( uint64_t i = 0; i < th->connections; i++, c++ )
    {
        c->thread = th;
        c->ssl     = cfg.ctx ? SSL_new(cfg.ctx) : NULL;
        c->request = request;
        c->length  = length;
        c->throughput = throughput;
        c->catch_up_throughput = throughput * 2;
        c->complete   = 0;
        c->caught_up  = true;

        /* Stagger connects 5 msec apart within thread */
        aeCreateTimeEvent(loop, i * 5, delayed_initial_connect, c, NULL);

        //connect_socket(th, c);
    }

    uint64_t calibrate_delay = CALIBRATE_DELAY_MS + (th->connections * 5);
    uint64_t timeout_delay = TIMEOUT_INTERVAL_MS + (th->connections * 5);

    aeCreateTimeEvent(loop, calibrate_delay, calibrate, th, NULL);

    if( th->max_nrequests == 0 )
        aeCreateTimeEvent(loop, timeout_delay, check_timeouts, th, NULL);

    th->start = time_us();
    aeMain(loop);

    aeDeleteEventLoop(loop);
    zfree(th->cs);

    return NULL;
}
/*---------------------------------------------------------------------------*/
static int connect_socket( thread *th, connection *c )
{
    struct addrinfo *addr = th->addr;
    struct aeEventLoop *loop = th->loop;
    int fd, flags;

    fd = socket(addr->ai_family, addr->ai_socktype, addr->ai_protocol);

    flags = fcntl(fd, F_GETFL, 0);
    fcntl(fd, F_SETFL, flags | O_NONBLOCK);

    if( connect(fd, addr->ai_addr, addr->ai_addrlen) == -1 )
    {
        if( errno != EINPROGRESS )
        {
            th->errors.connect++;
            close(fd);
            return -1;
        }
    }

    flags = 1;
    setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &flags, sizeof(flags));

    c->latest_connect = time_us();

    flags = AE_READABLE | AE_WRITABLE;

    if( aeCreateFileEvent(loop, fd, flags, socket_connected, c) == AE_OK )
    {
        c->parser.data = c;
        c->fd = fd;
        return fd;
    }

    th->errors.connect++;
    close(fd);
    return -1;
}
/*---------------------------------------------------------------------------*/
static int reconnect_socket( thread *th, connection *c )
{
    aeDeleteFileEvent(th->loop, c->fd, AE_WRITABLE | AE_READABLE);
    sock.close(c);
    close(c->fd);
    return connect_socket(th, c);
}
/*---------------------------------------------------------------------------*/
static int delayed_initial_connect( aeEventLoop *loop, long long id, void *data )
{
    connection* c = (connection*)data;
    c->thread_start = time_us();
    connect_socket(c->thread, c);
    return AE_NOMORE;
}
/*---------------------------------------------------------------------------*/
static int calibrate( aeEventLoop *loop, long long id, void *data )
{
    thread *th = (thread*)data;

    long double mean = hdr_mean(th->latency_histogram);
    long double latency = hdr_value_at_percentile(th->latency_histogram, 90.0) / 1000.0L;
    long double interval = MAX(latency * 2, 10);

    if( mean == 0 )
        return CALIBRATE_DELAY_MS;

    th->mean     = (uint64_t) mean;
    hdr_reset(th->latency_histogram);
    hdr_reset(th->u_latency_histogram);

    th->start    = time_us();
    th->interval = interval;
    th->requests = 0;

    printf("  Thread calibration: mean lat.: %.3fms, rate sampling interval: %dms\n",
            (th->mean)/1000.0,
            th->interval);

    aeCreateTimeEvent(loop, th->interval, sample_rate, th, NULL);

    return AE_NOMORE;
}
/*---------------------------------------------------------------------------*/
static int check_timeouts( aeEventLoop *loop, long long id, void *data )
{
    thread *th = (thread*)data;
    connection *c  = th->cs;
    uint64_t now   = time_us();

    uint64_t maxAge = now - (cfg.timeout * 1000);

    for( uint64_t i = 0; i < th->connections; i++, c++ )
    {
        if( maxAge > c->start )
            th->errors.timeout++;
    }

    if( stop || now >= th->stop_at )
        aeStop(loop);

    return TIMEOUT_INTERVAL_MS;
}
/*---------------------------------------------------------------------------*/
static int sample_rate( aeEventLoop *loop, long long id, void *data )
{
    thread *th = data;

    uint64_t elapsed_ms = (time_us() - th->start) / 1000;
    uint64_t requests = (th->requests / (double) elapsed_ms) * 1000;

    pthread_mutex_lock(&statistics.mutex);
    stats_record(statistics.requests, requests);
    pthread_mutex_unlock(&statistics.mutex);

    th->requests = 0;
    th->start    = time_us();

    return th->interval;
}
/*---------------------------------------------------------------------------*/
static int header_field( http_parser *parser, const char *at, size_t len )
{
    connection *c = parser->data;

    if( c->state == VALUE )
    {
        *c->headers.cursor++ = '\0';
        c->state = FIELD;
    }

    buffer_append(&c->headers, at, len);
    return 0;
}
/*---------------------------------------------------------------------------*/
static int header_value( http_parser *parser, const char *at, size_t len )
{
    connection *c = parser->data;

    if( c->state == FIELD )
    {
        *c->headers.cursor++ = '\0';
        c->state = VALUE;
    }

    buffer_append(&c->headers, at, len);
    return 0;
}
/*---------------------------------------------------------------------------*/
static int response_body( http_parser *parser, const char *at, size_t len )
{
    connection *c = parser->data;
    buffer_append(&c->body, at, len);
    return 0;
}
/*---------------------------------------------------------------------------*/
static uint64_t usec_to_next_send( connection *c )
{
    uint64_t now = time_us();
    bool send_now = true;

    uint64_t next_start_time = c->thread_start + (c->complete / c->throughput);

    if( next_start_time > now )
    {
        /* We are on pace. Indicate caught_up and don't send now */
        c->caught_up = true;
        send_now = false;
    }
    else
    {
        /* We are behind */
        if( c->caught_up )
        {
            /* This is the first fall-behind since we were last caught up */
            c->caught_up = false;
            c->catch_up_start_time = now;
            c->complete_at_catch_up_start = c->complete;
        }

        /* Figure out if it's time to send, per catch up throughput */
        uint64_t complete_since_catch_up_start = c->complete - c->complete_at_catch_up_start;

        next_start_time = c->catch_up_start_time + (complete_since_catch_up_start / c->catch_up_throughput);

        if( next_start_time > now )
            send_now = false; /* Not yet time to send, even at catch-up throughout */
    }

    if( send_now )
    {
        c->latest_should_send_time = now;
        c->latest_expected_start = next_start_time;
    }

    return send_now ? 0 : (next_start_time - now);
}
/*---------------------------------------------------------------------------*/
static int delay_request( aeEventLoop *loop, long long id, void *data )
{
    connection* c = data;
    uint64_t time_usec_to_wait = usec_to_next_send(c);
    if( time_usec_to_wait )
        return round((time_usec_to_wait / 1000.0L) + 0.5); /* don't send, wait */

    aeCreateFileEvent(c->thread->loop, c->fd, AE_WRITABLE, socket_writeable, c);
    return AE_NOMORE;
}
/*---------------------------------------------------------------------------*/
static int response_http_complete( http_parser *parser )
{
    uint64_t expected_latency_start = 0;
    int64_t expected_latency_timing = 0;
    connection *c = parser->data;
    thread *th = c->thread;
    uint64_t now = time_us();
    int status = parser->status_code;

    th->complete++;
    th->requests++;

    if( status > 399 )
        th->errors.status++;

    if( c->headers.buffer )
    {
        *c->headers.cursor++ = '\0';
        script_http_response(th->L, status, &c->headers, &c->body);
        c->state = FIELD;
    }

    if( stop || (th->stop_at > 0 && now >= th->stop_at) )
    {
        aeStop(th->loop);
        return 0;
    }

    /* Count all responses (including pipelined ones) */
    c->complete++;

    /* Note that expected start time is computed based on the completed
     * response count seen at the beginning of the last request batch sent.
     * A single request batch send may contain multiple requests, and
     * result in multiple responses. If we incorrectly calculated expect
     * start time based on the completion count of these individual pipelined
     * requests we can easily end up "gifting" them time and seeing
     * negative latencies  */
    expected_latency_start = c->thread_start + (c->complete_at_last_batch_start / c->throughput);
    expected_latency_timing = now - expected_latency_start;

    if( expected_latency_timing < 0 )
    {
        printf("\n\n ---------- \n\n");
        printf("We are about to crash and die (recoridng a negative #)");
        printf("This wil never ever ever happen...");
        printf("But when it does. The following information will help in debugging");
        printf("response_complete:\n");
        printf("  expected_latency_timing = %lld\n", expected_latency_timing);
        printf("  now = %lld\n", now);
        printf("  expected_latency_start = %lld\n", expected_latency_start);
        printf("  c->thread_start = %lld\n", c->thread_start);
        printf("  c->complete = %lld\n", c->complete);
        printf("  throughput = %g\n", c->throughput);
        printf("  latest_should_send_time = %lld\n", c->latest_should_send_time);
        printf("  latest_expected_start = %lld\n", c->latest_expected_start);
        printf("  latest_connect = %lld\n", c->latest_connect);
        printf("  latest_write = %lld\n", c->latest_write);

        expected_latency_start = c->thread_start + ((c->complete ) / c->throughput);
        printf("  next expected_latency_start = %lld\n", expected_latency_start);
    }

    c->latest_should_send_time = 0;
    c->latest_expected_start = 0;

    if( --c->pending == 0 )
    {
        c->has_pending = false;
        aeCreateFileEvent(th->loop, c->fd, AE_WRITABLE, socket_writeable, c);
    }

    /* Record if needed, either last in batch or all, depending in cfg */
    if( cfg.record_all_responses || !c->has_pending )
    {
        hdr_record_value(th->latency_histogram, expected_latency_timing);

        uint64_t actual_latency_timing = now - c->actual_latency_start;
        hdr_record_value(th->u_latency_histogram, actual_latency_timing);
    }

    if( th->max_nrequests > 0 && th->max_nrequests == th->requests )
    {
        aeStop(th->loop);
        return 0;
    }

    if( !http_should_keep_alive(parser) )
    {
        reconnect_socket(th, c);
        return 0;
    }

    http_parser_init(parser, HTTP_RESPONSE);
    return 0;
}
/*---------------------------------------------------------------------------*/
static int response_raw( connection *c, size_t n )
{
    uint64_t expected_latency_start = 0;
    int64_t expected_latency_timing = 0;
    thread *th = c->thread;
    uint64_t now = time_us();

    int status = script_raw_response(th->L, c, n);

    th->complete++;
    th->requests++;

    if( status > 399 )
        th->errors.status++;

    if( stop || now >= th->stop_at )
    {
        aeStop(th->loop);
        return 0;
    }

    /* Count all responses (including pipelined ones) */
    c->complete++;

    /* Note that expected start time is computed based on the completed
     * response count seen at the beginning of the last request batch sent.
     * A single request batch send may contain multiple requests, and
     * result in multiple responses. If we incorrectly calculated expect
     * start time based on the completion count of these individual pipelined
     * requests we can easily end up "gifting" them time and seeing
     * negative latencies  */
    expected_latency_start = c->thread_start + (c->complete_at_last_batch_start / c->throughput);
    expected_latency_timing = now - expected_latency_start;

    if( expected_latency_timing < 0 )
    {
        printf("\n\n ---------- \n\n");
        printf("We are about to crash and die (recoridng a negative #)");
        printf("This wil never ever ever happen...");
        printf("But when it does. The following information will help in debugging");
        printf("response_complete:\n");
        printf("  expected_latency_timing = %lld\n", expected_latency_timing);
        printf("  now = %lld\n", now);
        printf("  expected_latency_start = %lld\n", expected_latency_start);
        printf("  c->thread_start = %lld\n", c->thread_start);
        printf("  c->complete = %lld\n", c->complete);
        printf("  throughput = %g\n", c->throughput);
        printf("  latest_should_send_time = %lld\n", c->latest_should_send_time);
        printf("  latest_expected_start = %lld\n", c->latest_expected_start);
        printf("  latest_connect = %lld\n", c->latest_connect);
        printf("  latest_write = %lld\n", c->latest_write);

        expected_latency_start = c->thread_start + ((c->complete ) / c->throughput);
        printf("  next expected_latency_start = %lld\n", expected_latency_start);
    }

    c->latest_should_send_time = 0;
    c->latest_expected_start = 0;

    if( --c->pending == 0 )
    {
        c->has_pending = false;
        aeCreateFileEvent(th->loop, c->fd, AE_WRITABLE, socket_writeable, c);
    }

    /* Record if needed, either last in batch or all, depending in cfg */
    if( cfg.record_all_responses || !c->has_pending )
    {
        hdr_record_value(th->latency_histogram, expected_latency_timing);

        uint64_t actual_latency_timing = now - c->actual_latency_start;
        hdr_record_value(th->u_latency_histogram, actual_latency_timing);
    }

    if( th->max_nrequests > 0 && th->max_nrequests == th->requests )
    {
        aeStop(th->loop);
        return 0;
    }

    return 0;
}
/*---------------------------------------------------------------------------*/
static void socket_connected( aeEventLoop *loop, int fd, void *data, int mask )
{
    connection *c = data;

    switch( sock.connect(c, cfg.host) )
    {
        case OK:    break;

        case ERROR:
                    c->thread->errors.connect++;
                    reconnect_socket(c->thread, c);
                    return;

        case RETRY: return;
    }

    if( cfg.protocol == PROTO_HTTP )
        http_parser_init(&c->parser, HTTP_RESPONSE);

    c->written = 0;

    aeCreateFileEvent(c->thread->loop, fd, AE_READABLE, socket_readable, c);
    aeCreateFileEvent(c->thread->loop, fd, AE_WRITABLE, socket_writeable, c);
}
/*---------------------------------------------------------------------------*/
static void socket_writeable( aeEventLoop *loop, int fd, void *data, int mask )
{
    connection *c = data;
    thread *th = c->thread;

    if( !c->written )
    {
        uint64_t time_usec_to_wait = usec_to_next_send(c);

        if( time_usec_to_wait )
        {
            int msec_to_wait = round((time_usec_to_wait / 1000.0L) + 0.5);

            /* Not yet time to send. Delay */
            aeDeleteFileEvent(loop, fd, AE_WRITABLE);
            aeCreateTimeEvent( th->loop, msec_to_wait, delay_request, c, NULL);
            return;
        }

        c->latest_write = time_us();
    }

    if( !c->written && cfg.dynamic )
        script_request(th->L, &c->request, &c->length);

    char  *buf = c->request + c->written;
    size_t len = c->length  - c->written;
    size_t n;

    if( !c->written )
    {
        c->start = time_us();
        if( !c->has_pending )
        {
            c->actual_latency_start = c->start;
            c->complete_at_last_batch_start = c->complete;
            c->has_pending = true;
        }
        c->pending = cfg.pipeline;
    }

    switch( sock.write(c, buf, len, &n) )
    {
        case OK:    break;

        case ERROR:
                    th->errors.write++;
                    reconnect_socket(th, c);
                    return;

        case RETRY: return;
    }

    c->written += n;

    if( c->written == c->length )
    {
        c->written = 0;
        aeDeleteFileEvent(loop, fd, AE_WRITABLE);
    }
}
/*---------------------------------------------------------------------------*/
static void socket_readable( aeEventLoop *loop, int fd, void *data, int mask )
{
    connection *c = data;
    size_t n = 0;

    do
    {
        switch( sock.read(c, &n) )
        {
            case OK:    break;            
            case ERROR:
                        c->thread->errors.read++;
                        reconnect_socket(c->thread, c);
                        return;

            case RETRY:
                        return;
        }

        /* Get latest read time */
        c->latest_read = time_us();

        if( cfg.protocol == PROTO_HTTP ) /* HTTP protocol */
        {
            if( http_parser_execute( &c->parser, &parser_settings, c->buf, n ) != n ||
                ( n == 0 && !http_body_is_final(&c->parser) ) )
            {
                c->thread->errors.read++;
                reconnect_socket(c->thread, c);
                return;
            }
        }
        else if( cfg.protocol == PROTO_RAW ) /* raw protocol */
        {
            response_raw( c, n );
        }

        c->thread->bytes += n;

    } while( n == RECVBUF && sock.readable(c) > 0 );

}
/*---------------------------------------------------------------------------*/
static uint64_t time_us()
{
    struct timeval t;
    gettimeofday(&t, NULL);
    return (t.tv_sec * 1000000) + t.tv_usec;
}
/*---------------------------------------------------------------------------*/
static char *copy_url_part(char *url, struct http_parser_url *parts, enum http_parser_url_fields field)
{
    char *part = NULL;

    if( parts->field_set & (1 << field) )
    {
        uint16_t off = parts->field_data[field].off;
        uint16_t len = parts->field_data[field].len;
        part = zcalloc(len + 1 * sizeof(char));
        memcpy(part, &url[off], len);
    }

    return part;
}
/*---------------------------------------------------------------------------*/
static struct option longopts[] =
{
    { "connections",    required_argument, NULL, 'c' },
    { "duration",       required_argument, NULL, 'd' },
    { "threads",        required_argument, NULL, 't' },
    { "requests",       required_argument, NULL, 'r' },
    { "script",         required_argument, NULL, 's' },
    { "header",         required_argument, NULL, 'H' },
    { "latency",        no_argument,       NULL, 'L' },
    { "u_latency",      no_argument,       NULL, 'U' },
    { "batch_latency",  no_argument,       NULL, 'B' },
    { "timeout",        required_argument, NULL, 'T' },
    { "help",           no_argument,       NULL, 'h' },
    { "version",        no_argument,       NULL, 'v' },
    { "rate",           required_argument, NULL, 'R' },
    { NULL,             0,                 NULL,  0  }
};
/*---------------------------------------------------------------------------*/
static int parse_args( struct config *cfg, char **url, struct http_parser_url *parts,
                       char **headers, int argc, char **argv )
{
    char **header = headers;
    int c;

    /* Init structure */
    memset(cfg, 0, sizeof(struct config));

    cfg->num_threads          = 2;
    cfg->num_connections      = 10;
    cfg->num_requests         = 0;     /* Unlimited requests */
    cfg->duration             = 10;
    cfg->timeout              = SOCKET_TIMEOUT_MS;
    cfg->rate                 = 0;
    cfg->record_all_responses = true;

    while( (c = getopt_long(argc, argv, "t:c:d:r:s:H:T:R:LUBv?", longopts, NULL)) != -1 )
    {
        switch( c )
        {
            case 't':
                if( scan_metric(optarg, &cfg->num_threads) ) return -1;
                break;
            case 'c':
                if( scan_metric(optarg, &cfg->num_connections) ) return -1;
                break;
            case 'd':
                if( scan_time(optarg, &cfg->duration) ) return -1;
                break;
            case 'r':
                if( scan_metric(optarg, &cfg->num_requests) ) return -1;
                break;
            case 's':
                cfg->script = optarg;
                break;
            case 'H':
                *header++ = optarg;
                break;
            case 'L':
                cfg->latency = true;
                break;
            case 'B':
                cfg->record_all_responses = false;
                break;
            case 'U':
                cfg->latency = true;
                cfg->u_latency = true;
                break;
            case 'T':
                if( scan_time(optarg, &cfg->timeout) ) return -1;
                cfg->timeout *= 1000;
                break;
            case 'R':
                if( scan_metric(optarg, &cfg->rate) ) return -1;
                break;
            case 'v':
                printf("wrk %s [%s] ", VERSION, aeGetApiName());
                printf("Copyright (C) 2012 Will Glozer\n");
                break;
            case 'h':
            case '?':
            case ':':
            default:
                return -1;
        }
    }

    if( optind == argc || !cfg->num_threads || !cfg->duration )
        return -1;

    if( !script_parse_url(argv[optind], parts) )
    {
        fprintf(stderr, "invalid URL: %s\n", argv[optind]);
        return -1;
    }

    if( !cfg->num_connections || cfg->num_connections < cfg->num_threads )
    {
        fprintf(stderr, "number of connections must be >= threads\n");
        return -1;
    }

    *url    = argv[optind];
    *header = NULL;

    return 0;
}
/*---------------------------------------------------------------------------*/
static void print_stats_header()
{
    printf("  Thread Stats%6s%11s%8s%12s\n", "Avg", "Stdev", "Max", "+/- Stdev");
}
/*---------------------------------------------------------------------------*/
static void print_units( long double n, char *(*fmt)(long double), int width )
{
    char *msg = fmt(n);
    int len = strlen(msg), pad = 2;

    if (isalpha(msg[len-1])) pad--;
    if (isalpha(msg[len-2])) pad--;
    width -= pad;

    printf("%*.*s%.*s", width, width, msg, pad, "  ");

    free(msg);
}
/*---------------------------------------------------------------------------*/
static void print_stats( char *name, stats *stats, char *(*fmt)(long double) )
{
    uint64_t max = stats->max;
    long double mean  = stats_mean(stats);
    long double stdev = stats_stdev(stats, mean);

    printf("    %-10s", name);
    print_units(mean,  fmt, 8);
    print_units(stdev, fmt, 10);
    print_units(max,   fmt, 9);
    printf("%8.2Lf%%\n", stats_within_stdev(stats, mean, stdev, 1));
}
/*---------------------------------------------------------------------------*/
static void print_stats_latency( stats *stats )
{
    long double percentiles[] = { 50.0, 75.0, 90.0, 99.0, 99.9, 99.99, 99.999, 100.0 };
    printf("  Latency Distribution\n");
    for( size_t i = 0; i < sizeof(percentiles) / sizeof(long double); i++ )
    {
        long double p = percentiles[i];
        uint64_t n = stats_percentile(stats, p);
        printf("%7.3Lf%%", p);
        print_units(n, format_time_us, 10);
        printf("\n");
    }
}
/*---------------------------------------------------------------------------*/
static void print_hdr_latency( struct hdr_histogram* histogram, const char* description )
{
    long double percentiles[] = { 50.0, 75.0, 90.0, 99.0, 99.9, 99.99, 99.999, 100.0};
    printf("  Latency Distribution (HdrHistogram - %s)\n", description);
    for( size_t i = 0; i < sizeof(percentiles) / sizeof(long double); i++ )
    {
        long double p = percentiles[i];
        int64_t n = hdr_value_at_percentile(histogram, p);
        printf("%7.3Lf%%", p);
        print_units(n, format_time_us, 10);
        printf("\n");
    }
    printf("\n%s\n", "  Detailed Percentile spectrum:");
    hdr_percentiles_print(histogram, stdout, 5, 1000.0, CLASSIC);
}
/*---------------------------------------------------------------------------*/
