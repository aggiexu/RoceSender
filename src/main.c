#define _POSIX_C_SOURCE 200809L

#include <arpa/inet.h>
#include <errno.h>
#include <getopt.h>
#include <inttypes.h>
#include <mpi.h>
#include <netinet/in.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include <infiniband/verbs.h>

#define DEFAULT_PORT 1
#define DEFAULT_ITERS 10000
#define DEFAULT_SIZE 64
#define DEFAULT_SL 0
#define DEFAULT_QKEY 0x11111111

struct qp_info {
    uint16_t lid;
    uint32_t qpn;
    uint32_t psn;
    uint32_t rkey;
    uint64_t vaddr;
};

struct app_opts {
    const char *dev_name;
    int ib_port;
    int msg_size;
    int iters;
    int sl;
};

struct app_context {
    struct ibv_context *context;
    struct ibv_pd *pd;
    struct ibv_cq *cq;
    struct ibv_qp *qp;
    struct ibv_mr *mr;
    void *buf;
    size_t buf_size;
};

static void die(const char *msg)
{
    fprintf(stderr, "%s: %s\n", msg, strerror(errno));
    MPI_Abort(MPI_COMM_WORLD, 1);
}

static uint32_t random_psn(void)
{
    return (uint32_t)rand() & 0xffffff;
}

static void usage(const char *prog)
{
    fprintf(stderr,
            "Usage: %s [options]\n"
            "  -d, --device <name>   IB device name (default: first device)\n"
            "  -p, --port <num>      IB port (default: %d)\n"
            "  -s, --size <bytes>    message size (default: %d)\n"
            "  -n, --iters <num>     iterations (default: %d)\n"
            "  -l, --sl <num>        service level (default: %d)\n",
            prog, DEFAULT_PORT, DEFAULT_SIZE, DEFAULT_ITERS, DEFAULT_SL);
}

static void parse_opts(int argc, char **argv, struct app_opts *opts)
{
    static struct option long_opts[] = {
        {"device", required_argument, NULL, 'd'},
        {"port", required_argument, NULL, 'p'},
        {"size", required_argument, NULL, 's'},
        {"iters", required_argument, NULL, 'n'},
        {"sl", required_argument, NULL, 'l'},
        {NULL, 0, NULL, 0},
    };

    int c;
    while ((c = getopt_long(argc, argv, "d:p:s:n:l:", long_opts, NULL)) != -1) {
        switch (c) {
        case 'd':
            opts->dev_name = optarg;
            break;
        case 'p':
            opts->ib_port = atoi(optarg);
            break;
        case 's':
            opts->msg_size = atoi(optarg);
            break;
        case 'n':
            opts->iters = atoi(optarg);
            break;
        case 'l':
            opts->sl = atoi(optarg);
            break;
        default:
            usage(argv[0]);
            MPI_Abort(MPI_COMM_WORLD, 1);
        }
    }
}

static struct ibv_context *open_device(const char *dev_name)
{
    int num_devices = 0;
    struct ibv_device **dev_list = ibv_get_device_list(&num_devices);
    if (!dev_list || num_devices == 0) {
        fprintf(stderr, "No IB devices found\n");
        MPI_Abort(MPI_COMM_WORLD, 1);
    }

    struct ibv_device *found = NULL;
    for (int i = 0; i < num_devices; ++i) {
        const char *name = ibv_get_device_name(dev_list[i]);
        if (!dev_name || strcmp(dev_name, name) == 0) {
            found = dev_list[i];
            break;
        }
    }

    if (!found) {
        fprintf(stderr, "Device %s not found\n", dev_name);
        ibv_free_device_list(dev_list);
        MPI_Abort(MPI_COMM_WORLD, 1);
    }

    struct ibv_context *ctx = ibv_open_device(found);
    ibv_free_device_list(dev_list);
    if (!ctx) {
        die("ibv_open_device failed");
    }
    return ctx;
}

static uint16_t get_local_lid(struct ibv_context *ctx, int port)
{
    struct ibv_port_attr attr;
    if (ibv_query_port(ctx, port, &attr)) {
        die("ibv_query_port failed");
    }
    return attr.lid;
}

static void setup_context(struct app_context *app, struct app_opts *opts)
{
    app->context = open_device(opts->dev_name);
    app->pd = ibv_alloc_pd(app->context);
    if (!app->pd) {
        die("ibv_alloc_pd failed");
    }

    app->cq = ibv_create_cq(app->context, opts->iters + 10, NULL, NULL, 0);
    if (!app->cq) {
        die("ibv_create_cq failed");
    }

    struct ibv_qp_init_attr qp_init = {
        .send_cq = app->cq,
        .recv_cq = app->cq,
        .qp_type = IBV_QPT_RC,
        .cap = {
            .max_send_wr = opts->iters + 10,
            .max_recv_wr = opts->iters + 10,
            .max_send_sge = 1,
            .max_recv_sge = 1,
        },
    };

    app->qp = ibv_create_qp(app->pd, &qp_init);
    if (!app->qp) {
        die("ibv_create_qp failed");
    }

    app->buf_size = (size_t)opts->msg_size;
    app->buf = aligned_alloc(64, app->buf_size);
    if (!app->buf) {
        die("aligned_alloc failed");
    }
    memset(app->buf, 0, app->buf_size);

    int mr_flags = IBV_ACCESS_LOCAL_WRITE | IBV_ACCESS_REMOTE_WRITE | IBV_ACCESS_REMOTE_READ;
    app->mr = ibv_reg_mr(app->pd, app->buf, app->buf_size, mr_flags);
    if (!app->mr) {
        die("ibv_reg_mr failed");
    }
}

static void modify_qp_init(struct ibv_qp *qp, int port)
{
    struct ibv_qp_attr attr = {
        .qp_state = IBV_QPS_INIT,
        .pkey_index = 0,
        .port_num = port,
        .qp_access_flags = IBV_ACCESS_REMOTE_READ | IBV_ACCESS_REMOTE_WRITE,
    };

    int flags = IBV_QP_STATE | IBV_QP_PKEY_INDEX | IBV_QP_PORT | IBV_QP_ACCESS_FLAGS;
    if (ibv_modify_qp(qp, &attr, flags)) {
        die("ibv_modify_qp INIT failed");
    }
}

static void modify_qp_rtr(struct ibv_qp *qp, int port, const struct qp_info *remote, int sl)
{
    struct ibv_qp_attr attr = {
        .qp_state = IBV_QPS_RTR,
        .path_mtu = IBV_MTU_1024,
        .dest_qp_num = remote->qpn,
        .rq_psn = remote->psn,
        .max_dest_rd_atomic = 1,
        .min_rnr_timer = 12,
        .ah_attr = {
            .is_global = 0,
            .dlid = remote->lid,
            .sl = sl,
            .src_path_bits = 0,
            .port_num = port,
        },
    };

    int flags = IBV_QP_STATE | IBV_QP_AV | IBV_QP_PATH_MTU |
                IBV_QP_DEST_QPN | IBV_QP_RQ_PSN | IBV_QP_MAX_DEST_RD_ATOMIC |
                IBV_QP_MIN_RNR_TIMER;
    if (ibv_modify_qp(qp, &attr, flags)) {
        die("ibv_modify_qp RTR failed");
    }
}

static void modify_qp_rts(struct ibv_qp *qp, uint32_t psn)
{
    struct ibv_qp_attr attr = {
        .qp_state = IBV_QPS_RTS,
        .timeout = 14,
        .retry_cnt = 7,
        .rnr_retry = 7,
        .sq_psn = psn,
        .max_rd_atomic = 1,
    };

    int flags = IBV_QP_STATE | IBV_QP_TIMEOUT | IBV_QP_RETRY_CNT |
                IBV_QP_RNR_RETRY | IBV_QP_SQ_PSN | IBV_QP_MAX_QP_RD_ATOMIC;
    if (ibv_modify_qp(qp, &attr, flags)) {
        die("ibv_modify_qp RTS failed");
    }
}

static void post_recv(struct app_context *app)
{
    struct ibv_sge sge = {
        .addr = (uintptr_t)app->buf,
        .length = (uint32_t)app->buf_size,
        .lkey = app->mr->lkey,
    };

    struct ibv_recv_wr wr = {
        .wr_id = 1,
        .sg_list = &sge,
        .num_sge = 1,
    };

    struct ibv_recv_wr *bad_wr = NULL;
    if (ibv_post_recv(app->qp, &wr, &bad_wr)) {
        die("ibv_post_recv failed");
    }
}

static void post_send(struct app_context *app)
{
    struct ibv_sge sge = {
        .addr = (uintptr_t)app->buf,
        .length = (uint32_t)app->buf_size,
        .lkey = app->mr->lkey,
    };

    struct ibv_send_wr wr = {
        .wr_id = 2,
        .sg_list = &sge,
        .num_sge = 1,
        .opcode = IBV_WR_SEND,
        .send_flags = IBV_SEND_SIGNALED,
    };

    struct ibv_send_wr *bad_wr = NULL;
    if (ibv_post_send(app->qp, &wr, &bad_wr)) {
        die("ibv_post_send failed");
    }
}

static void poll_cq(struct app_context *app)
{
    struct ibv_wc wc;
    int retries = 0;
    while (ibv_poll_cq(app->cq, 1, &wc) == 0) {
        if (++retries > 1000000) {
            fprintf(stderr, "CQ poll timeout\n");
            MPI_Abort(MPI_COMM_WORLD, 1);
        }
    }

    if (wc.status != IBV_WC_SUCCESS) {
        fprintf(stderr, "CQ completion failed: %s\n", ibv_wc_status_str(wc.status));
        MPI_Abort(MPI_COMM_WORLD, 1);
    }
}

static double timespec_to_usec(const struct timespec *ts)
{
    return (double)ts->tv_sec * 1e6 + (double)ts->tv_nsec / 1e3;
}

int main(int argc, char **argv)
{
    MPI_Init(&argc, &argv);

    int rank = 0;
    int size = 0;
    MPI_Comm_rank(MPI_COMM_WORLD, &rank);
    MPI_Comm_size(MPI_COMM_WORLD, &size);

    if (size != 2) {
        if (rank == 0) {
            fprintf(stderr, "This test requires exactly 2 MPI ranks\n");
        }
        MPI_Finalize();
        return 1;
    }

    struct app_opts opts = {
        .dev_name = NULL,
        .ib_port = DEFAULT_PORT,
        .msg_size = DEFAULT_SIZE,
        .iters = DEFAULT_ITERS,
        .sl = DEFAULT_SL,
    };
    parse_opts(argc, argv, &opts);

    srand((unsigned int)((long)time(NULL) ^ rank));

    struct app_context app = {0};
    setup_context(&app, &opts);

    struct qp_info local = {
        .lid = get_local_lid(app.context, opts.ib_port),
        .qpn = app.qp->qp_num,
        .psn = random_psn(),
        .rkey = app.mr->rkey,
        .vaddr = (uintptr_t)app.buf,
    };

    struct qp_info remote = {0};
    MPI_Sendrecv(&local, sizeof(local), MPI_BYTE, 1 - rank, 0,
                 &remote, sizeof(remote), MPI_BYTE, 1 - rank, 0,
                 MPI_COMM_WORLD, MPI_STATUS_IGNORE);

    modify_qp_init(app.qp, opts.ib_port);
    modify_qp_rtr(app.qp, opts.ib_port, &remote, opts.sl);
    modify_qp_rts(app.qp, local.psn);

    MPI_Barrier(MPI_COMM_WORLD);

    if (rank == 0) {
        struct timespec start = {0};
        struct timespec end = {0};
        double total_us = 0.0;

        for (int i = 0; i < opts.iters; ++i) {
            post_recv(&app);
            clock_gettime(CLOCK_MONOTONIC, &start);
            post_send(&app);
            poll_cq(&app);
            poll_cq(&app);
            clock_gettime(CLOCK_MONOTONIC, &end);
            total_us += (timespec_to_usec(&end) - timespec_to_usec(&start));
        }

        double avg_rtt = total_us / opts.iters;
        double avg_one_way = avg_rtt / 2.0;
        printf("RDMA ping-pong completed: iters=%d size=%d bytes\n", opts.iters, opts.msg_size);
        printf("Average RTT: %.3f us\n", avg_rtt);
        printf("Average one-way latency: %.3f us\n", avg_one_way);
    } else {
        for (int i = 0; i < opts.iters; ++i) {
            post_recv(&app);
            poll_cq(&app);
            post_send(&app);
            poll_cq(&app);
        }
    }

    MPI_Barrier(MPI_COMM_WORLD);

    ibv_destroy_qp(app.qp);
    ibv_destroy_cq(app.cq);
    ibv_dereg_mr(app.mr);
    ibv_dealloc_pd(app.pd);
    ibv_close_device(app.context);
    free(app.buf);

    MPI_Finalize();
    return 0;
}
