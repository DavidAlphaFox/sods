/*
 * Socket over DNS client.
 *
 * Copyright (c) 2009 Michael Santos <michael.santos@gmail.com>
 *
 */

#include <signal.h>

#include <fcntl.h>
#include <errno.h>

#include <sys/types.h>
#include <sys/wait.h>

#include "sdt.h"

extern char *__progname;

int woken = 0;
int forcesend = 0;
int nsauto = 1;

    int
main(int argc, char *argv[])
{
    SDT_STATE *ss = NULL;
    pid_t pid = 0;
    int nd = 0;
    int ch = 0;

    IS_NULL(ss = (SDT_STATE *)calloc(1, sizeof(SDT_STATE)));

    sdt_rand_init();
    (void)sdt_dns_init();

    ss->sess.o.id = (u_int16_t)arc4random();
    ss->backoff = 1;
    ss->maxbackoff = MAXBACKOFF;
    ss->sleep = SLEEP_TXT;
    ss->bufsz = 110;
    ss->delay = 1000000/2; /* 2/second */
    ss->faststart = 3;
    ss->maxpollfail = MAXPOLLFAIL;
    ss->type = ns_t_txt;
    ss->verbose_lines = 100;

    while ( (ch = getopt(argc, argv, "A:B:b:dF:hM:m:R:r:S:s:T:t:vx:")) != -1) {
        switch (ch) {
            case 'A':	/* alarm, delay buf */
                ss->delay = (u_int32_t)atoi(optarg);
                break;
            case 'B':   /* send buf size */
                ss->bufsz = (size_t)atoi(optarg);
                break;
            case 'b':   /* max backoff */
                ss->maxbackoff = (u_int16_t)atoi(optarg);
                break;
            case 'd':   /* Debug DNS */
                sdt_dns_setopt(SDT_RES_DEBUG, 1);
                break;
            case 'F':   /* fast start */
                ss->faststart = (int32_t)atoi(optarg);
                break;
            case 'M':
                ss->maxpollfail = (int32_t)atoi(optarg);
                break;
            case 'm':
                ss->sleep = (u_int32_t)atoi(optarg);
                break;
            case 'R':   /* Retry lookup */
                sdt_dns_setopt(SDT_RES_RETRY, (u_int32_t)atoi(optarg));
                break;
            case 'r':
                if (sdt_dns_parsens(ss, optarg) < 0)
                        errx(EXIT_FAILURE, "Invalid NS address");

                nsauto = 1;
                break;
            case 'S':   /* Resolver strategies */
                if (strcasecmp(optarg, "blast") == 0)
                    sdt_dns_setopt(SDT_RES_BLAST, 0);
                else if (strcasecmp(optarg, "rotate") == 0)
                    sdt_dns_setopt(SDT_RES_ROTATE, 0);
                else
                    usage(ss);
                break;
            case 's':   /* forwarded session */
                ss->sess.o.fwd = (u_int8_t)atoi(optarg);
                break;
            case 'T':
                sdt_dns_setopt(SDT_RES_USEVC, (int32_t)atoi(optarg));
                break;
            case 't':   /* DNS message type */
                if (strcasecmp(optarg, "TXT") == 0)
                    ss->type = ns_t_txt;
                else if (strcasecmp(optarg, "CNAME") == 0)
                    ss->type = ns_t_cname;
                else if (strcasecmp(optarg, "NULL") == 0)
                    ss->type = ns_t_null;
                else
                    usage(ss);
                break;
            case 'v':
                ss->verbose++;
                break;
            case 'x':   /* Transmit lookup timeout */
                sdt_dns_setopt(SDT_RES_RETRANS, (int32_t)atoi(optarg));
                break;

            case 'h':
            default:
                usage(ss);
        }
    }

    argc -= optind;
    argv += optind;

    if ( (argc != 1) || (strlen(argv[0]) > NS_MAXCDNAME - 1))
        usage(ss);

    IS_NULL(ss->dname = strdup(argv[0]));

    IS_ERR(nd = open("/dev/null", O_RDWR, 0));

    VERBOSE(1, "session id = %u, opt = %u, session = %d\n", ss->sess.o.id,
            ss->sess.o.opt, ss->sess.o.fwd);

    switch (pid = fork()) {
        case -1:
            err(EXIT_FAILURE, "fork");
        case 0:
            IS_ERR(dup2(fileno(stdin), nd));
            (void)signal(SIGUSR1,wakeup);
            sdt_loop_poll(ss);
            break;
        default:
            IS_ERR(dup2(fileno(stdout), nd));
            (void)signal(SIGHUP,wakeup);
            (void)signal(SIGTERM,wakeup);
            (void)signal(SIGALRM,wakeup);
            (void)signal(SIGCHLD,wakeup);
            ss->child = pid;
            sdt_loop_A(ss);

            if (woken == 1) {
                (void)kill(pid, SIGHUP);
                (void)wait(NULL);
            }

            break;
    }

    free(ss->dname);
    free(ss);
    IS_ERR(close(nd));

    exit(EXIT_SUCCESS);
}

/* Poll DNS server for data. Back off if no data is pending */
    void
sdt_loop_poll(SDT_STATE *ss)
{
    u_int32_t n = 0;

    for ( ; ; n++) {
        if (woken == 1) {
            ss->backoff = 1;
            woken = 0;
        }

        if (n%ss->backoff == 0)
            sdt_send_poll(ss);

        if (ss->maxpollfail > 0 && ss->pollfail > ss->maxpollfail) {
            VERBOSE(1, "*** Exiting from polling (child)\n");
            return;
        }

        usleep(ss->sleep);
    }
}

    void
sdt_send_poll(SDT_STATE *ss)
{
    char *buf = NULL;
    size_t len = 0;
    size_t n = 0;
    size_t t = 0;


    buf = sdt_dns_poll(ss, &len);

    if ( (len <= 0) || (buf == NULL)) {
        ss->backoff *= 3;
        if (ss->backoff > ss->maxbackoff)
            ss->backoff = ss->maxbackoff;

        return;
    }

    /* Ramp up polling */
    VERBOSE(1, "Ramping polling (record type = %d) ...\n",
            ss->type);
    ss->pollfail = 0;
    ss->backoff = 1;

    /* we received data, write to stdout, booyah! */
    while ( (n = write(fileno(stdout), buf + t, len - t)) != (len - t)) {
        if (n == -1)
            err(EXIT_FAILURE, "sdt_send_poll: write");
        t += n;
    }
    ss->sum += len;
    free(buf);
}


/* Read from STDIN and base32 encode the data as A records */
    void
sdt_loop_A(SDT_STATE *ss)
{
    ssize_t n = 0;
    char *buf = NULL;

    int fd = fileno(stdin);

    int flags = 0;

    flags = fcntl(fd, F_GETFL, 0);
    flags |= O_NONBLOCK;
    (void)fcntl(fd, F_SETFL, flags);

    IS_NULL(buf = (char *)calloc(ss->bufsz, 1));

    while ( (n = sdt_read(ss, fd, buf, ss->bufsz)) > 0) {
        VERBOSE(3, "Sending A record: %d of %u bytes\n", (int32_t)n, (u_int32_t)ss->bufsz);
        sdt_send_A(ss, buf, n);
        ss->sum_up += n;
        if (woken != 0)
            break;
        VERBOSE(3, "A record: res timeout = %d, sleep = %f seconds\n",
                ss->backoff, (float)(ss->sleep * ss->backoff)/1000000);
        usleep(ss->sleep);
    }

    free(buf);
    VERBOSE (1, "*** Exiting from A record read loop\n");
}

    ssize_t
sdt_read(SDT_STATE *ss, int fd, char *buf, size_t nbytes)
{   
    size_t n = 0;
    size_t t = 0;

    int rv = 0;
    int breakloop = 0;

    sdt_alarm(ss);
    do {
        fd_set rfds;

        FD_ZERO(&rfds);
        FD_SET(fd, &rfds);

        rv = select(fd+1, &rfds, NULL, NULL, NULL);

        switch (rv) {
            case 0:
                break;
            case -1:
                switch (errno) {
                    case EBADF:
                    case EINVAL:
                        woken = 1;
                        breakloop = 1;
                        break;
                    case EINTR:
                    default:
                        break;
                }
                break;
            default:
                n = read(fd, buf + t, nbytes - t);
                if (n == 0) {
                    woken = 1;
                    breakloop = 1;
                }
                if ( (ss->faststart > 0 || ss->delay == 0) && errno != EAGAIN)
                    breakloop = 1;
                t += n;
                break;
        }

        VERBOSE(3, "Looping in read: %u/%u bytes\n", (u_int32_t)n, (u_int32_t)t);
        if ( (forcesend == 1) || (t == nbytes)) {
            if (t > 0) {
                breakloop = 1;
                VERBOSE(3, " Forcing send Looping in read: %u/%u/%u bytes\n", (u_int32_t)n, (u_int32_t)t, (u_int32_t)nbytes);
            }
            if (forcesend == 1) {
                forcesend = 0;
                if (t == 0) sdt_alarm(ss);
            }
        }

        if (woken != 0)
            breakloop = 1;

    } while (breakloop == 0);

    ualarm(0, 0);
    forcesend = 0;

#define KEYSTROKELEN	32 /* size of one keystroke */
    if (ss->faststart > 0 && t <= KEYSTROKELEN)
        ss->faststart--;

    return (t);
}

    void
sdt_send_A(SDT_STATE *ss, char *buf, ssize_t n)
{
    while (sdt_dns_A(ss, buf, n) < 0) {
        /* Re-send the buffer */
        ss->backoff++;
        VERBOSE(1, "re-sending data (A record): res timeout = %d seconds\n", ss->backoff);
        if (ss->backoff > ss->maxbackoff)
            ss->backoff = ss->maxbackoff;
        sdt_dns_setopt(SDT_RES_RETRANS, ss->backoff);
    }
    /* Inform the child to increase polling of server */
    (void)kill(ss->child, SIGUSR1);
}


    void
sdt_alarm(SDT_STATE *ss)
{
    if (ss->faststart <= 0)
        ualarm(ss->delay, 0);
}

    void
wakeup(int sig)
{
    switch (sig) {
        case SIGUSR1:
        case SIGTERM:
        case SIGHUP:
            woken = 1;
            break;
        case SIGCHLD:
            woken = 2;
            break;
        case SIGALRM:
            forcesend = 1;
            break;
        default:
            break;
    }
}

    void
usage(SDT_STATE *ss)
{
    (void)fprintf(stderr, "%s: [-h|-r|-v] <domain name>\n", __progname);
    (void)fprintf(stderr, "[version %s]\n", SDT_VERSION);
    (void)fprintf(stderr, "-h\tUsage\n");
    (void)fprintf(stderr, "-A\tDelay A queries to force full buffer reads [default: %d microseconds]\n", ss->delay);
    (void)fprintf(stderr, "-B\tSize of read buffer (A queries) [default: %d bytes]\n", (u_int32_t)ss->bufsz);
    (void)fprintf(stderr, "-b\tMaximum backoff for polling server [default: %d]\n", ss->maxbackoff);
    (void)fprintf(stderr, "-F <num>\tFast start, number of small packets to pass w/out buffering (0 to disable) [default: %d]\n", ss->faststart);
    (void)fprintf(stderr, "-M\tMaximum number of polling query failures [default: %d]\n", ss->maxpollfail);
    (void)fprintf(stderr, "-m\tMinimum time to sleep between nameserver queries [default: %d us]\n", ss->sleep);
    (void)fprintf(stderr, "-R <number>\tNumber of retries for lookup\n");
    (void)fprintf(stderr, "-r\tNameserver (or keyword: random, opendns, verizon, speakeasy)\n");
    (void)fprintf(stderr, "-S [rotate|blast]\tResolver strategy\n");
    (void)fprintf(stderr, "-T <number>\tUse TCP [0 = new connection for each request, 1 = pipeline requests]\n");
    (void)fprintf(stderr, "-t <DNS type>\tTXT, CNAME [Default = TXT]\n");
    (void)fprintf(stderr, "-v\tPrint debug messages\n");
    (void)fprintf(stderr, "-x <number>\tResolver transmit timeout\n");
    (void)fprintf(stderr, "\nExample: %s -r 127.0.0.1 sshdns.a.example.com\n\n", __progname);

    if (ss->verbose > 0)
        sdt_dns_print_servers(ss);

    exit(EXIT_FAILURE);
}
