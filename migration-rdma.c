/*
 * RDMA protocol and interfaces
 *
 * Copyright IBM, Corp. 2010-2013
 *
 * Authors:
 *  Michael R. Hines <mrhines@us.ibm.com>
 *  Jiuxing Liu <jl@us.ibm.com>
 *
 * This work is licensed under the terms of the GNU GPL, version 2 or
 * later.  See the COPYING file in the top-level directory.
 *
 */
#include "qemu-common.h"
#include "migration/migration.h"
#include "migration/qemu-file.h"
#include "exec/cpu-common.h"
#include "qemu/main-loop.h"
#include "qemu/sockets.h"
#include "qemu/bitmap.h"
#include "block/coroutine.h"
#include <stdio.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <netdb.h>
#include <arpa/inet.h>
#include <string.h>
#include <rdma/rdma_cma.h>

/*
 * Ability to runtime-enable debug statements while inside GDB.
 * Choices are 1, 2, or 3 (so far).
 */
static int rdma_debug = 0;

//#define DEBUG_RDMA
//#define DEBUG_RDMA_VERBOSE
//#define DEBUG_RDMA_REALLY_VERBOSE

#define RPRINTF(fmt, ...) printf("rdma: " fmt, ## __VA_ARGS__)

#ifdef DEBUG_RDMA
#define DPRINTF(fmt, ...) \
    do { RPRINTF(fmt, ## __VA_ARGS__); } while (0)
#else
#define DPRINTF(fmt, ...) \
    do { if (rdma_debug >= 1) RPRINTF(fmt, ## __VA_ARGS__); } while (0)
#endif

#ifdef DEBUG_RDMA_VERBOSE
#define DDPRINTF(fmt, ...) \
    do { RPRINTF(fmt, ## __VA_ARGS__); } while (0)
#else
#define DDPRINTF(fmt, ...) \
    do { if (rdma_debug >= 2) RPRINTF(fmt, ## __VA_ARGS__); } while (0)
#endif

#ifdef DEBUG_RDMA_REALLY_VERBOSE
#define DDDPRINTF(fmt, ...) \
    do { RPRINTF(fmt, ## __VA_ARGS__); } while (0)
#else
#define DDDPRINTF(fmt, ...) \
    do { if (rdma_debug >= 3) RPRINTF(fmt, ## __VA_ARGS__); } while (0)
#endif

/*
 * Print and error on both the Monitor and the Log file.
 */
#define ERROR(errp, fmt, ...) \
    do { \
        Error **e = errp; \
        fprintf(stderr, "RDMA ERROR: " fmt "\n", ## __VA_ARGS__); \
        if (e && ((*e) == NULL)) { \
            error_setg(e, "RDMA ERROR: " fmt, ## __VA_ARGS__); \
        } \
    } while (0)

#define SET_ERROR(rdma, err) if (!rdma->error_state) rdma->error_state = err

#define RDMA_RESOLVE_TIMEOUT_MS 10000

/* Do not merge data if larger than this. */
#define RDMA_MERGE_MAX (2 * 1024 * 1024)
#define RDMA_SEND_MAX (RDMA_MERGE_MAX / 4096)

#define RDMA_REG_CHUNK_SHIFT 20 /* 1 MB */

/*
 * This is only for non-live state being migrated.
 * Instead of RDMA_WRITE messages, we use RDMA_SEND
 * messages for that state, which requires a different
 * delivery design than main memory.
 */
#define RDMA_SEND_INCREMENT 32768

/*
 * Maximum size infiniband SEND message
 */
#define RDMA_CONTROL_MAX_BUFFER (512 * 1024)
#define RDMA_CONTROL_MAX_COMMANDS_PER_MESSAGE 4096
/*
 * Capabilities for negotiation.
 */
#define RDMA_CAPABILITY_PIN_ALL 0x01
#define RDMA_CAPABILITY_KEEPALIVE 0x02

/*
 * Max # missed keepalive before we assume remote side is unavailable.
 */
#define RDMA_CONNECTION_INTERVAL_MS 300
#define RDMA_KEEPALIVE_INTERVAL_MS 300
#define RDMA_KEEPALIVE_FIRST_MISSED_OFFSET 1000
#define RDMA_MAX_LOST_KEEPALIVE 10 
#define RDMA_MAX_STARTUP_MISSED_KEEPALIVE 100

/*
 * Add the other flags above to this list of known capabilities
 * as they are introduced.
 */
static uint32_t known_capabilities = RDMA_CAPABILITY_PIN_ALL
                                   | RDMA_CAPABILITY_KEEPALIVE
                                   ;
static QEMUTimer *connection_timer = NULL;
static QEMUTimer *keepalive_timer = NULL;

#define CHECK_ERROR_STATE() \
    do { \
        if (rdma->error_state) { \
            if (!rdma->error_reported) { \
                fprintf(stderr, "RDMA is in an error state waiting migration" \
                                " to abort!\n"); \
                rdma->error_reported = 1; \
            } \
            return rdma->error_state; \
        } \
    } while (0);

/*
 * A work request ID is 64-bits and we split up these bits
 * into 3 parts:
 *
 * bits 0-15 : type of control message, 2^16
 * bits 16-29: ram block index, 2^14
 * bits 30-63: ram block chunk number, 2^34
 *
 * The last two bit ranges are only used for RDMA writes,
 * in order to track their completion and potentially
 * also track unregistration status of the message.
 */
#define RDMA_WRID_TYPE_SHIFT  0UL
#define RDMA_WRID_BLOCK_SHIFT 16UL
#define RDMA_WRID_CHUNK_SHIFT 30UL

#define RDMA_WRID_TYPE_MASK \
    ((1UL << RDMA_WRID_BLOCK_SHIFT) - 1UL)

#define RDMA_WRID_BLOCK_MASK \
    (~RDMA_WRID_TYPE_MASK & ((1UL << RDMA_WRID_CHUNK_SHIFT) - 1UL))

#define RDMA_WRID_CHUNK_MASK (~RDMA_WRID_BLOCK_MASK & ~RDMA_WRID_TYPE_MASK)

/*
 * RDMA migration protocol:
 * 1. RDMA Writes (data messages, i.e. RAM)
 * 2. IB Send/Recv (control channel messages)
 */
enum {
    RDMA_WRID_NONE = 0,
    RDMA_WRID_RDMA_WRITE_REMOTE = 1,
    RDMA_WRID_RDMA_WRITE_LOCAL = 2,
    RDMA_WRID_RDMA_KEEPALIVE = 3,
    RDMA_WRID_SEND_CONTROL = 2000,
    RDMA_WRID_RECV_CONTROL = 4000,
};

const char *wrid_desc[] = {
    [RDMA_WRID_NONE] = "NONE",
    [RDMA_WRID_RDMA_WRITE_REMOTE] = "WRITE RDMA REMOTE",
    [RDMA_WRID_RDMA_WRITE_LOCAL] = "WRITE RDMA LOCAL",
    [RDMA_WRID_RDMA_KEEPALIVE] = "KEEPALIVE",
    [RDMA_WRID_SEND_CONTROL] = "CONTROL SEND",
    [RDMA_WRID_RECV_CONTROL] = "CONTROL RECV",
};

/*
 * Work request IDs for IB SEND messages only (not RDMA writes).
 * This is used by the migration protocol to transmit
 * control messages (such as device state and registration commands)
 *
 * We could use more WRs, but we have enough for now.
 */
enum {
    RDMA_WRID_READY = 0,
    RDMA_WRID_DATA,
    RDMA_WRID_CONTROL,
    RDMA_WRID_MAX,
};

/*
 * SEND/RECV IB Control Messages.
 */
enum {
    RDMA_CONTROL_NONE = 0,
    RDMA_CONTROL_ERROR,
    RDMA_CONTROL_READY,               /* ready to receive */
    RDMA_CONTROL_QEMU_FILE,           /* QEMUFile-transmitted bytes */
    RDMA_CONTROL_RAM_BLOCKS_REQUEST,  /* RAMBlock synchronization */
    RDMA_CONTROL_RAM_BLOCKS_RESULT,   /* RAMBlock synchronization */
    RDMA_CONTROL_COMPRESS,            /* page contains repeat values */
    RDMA_CONTROL_REGISTER_REQUEST,    /* dynamic page registration */
    RDMA_CONTROL_REGISTER_RESULT,     /* key to use after registration */
    RDMA_CONTROL_REGISTER_FINISHED,   /* current iteration finished */
    RDMA_CONTROL_UNREGISTER_REQUEST,  /* dynamic UN-registration */
    RDMA_CONTROL_UNREGISTER_FINISHED, /* unpinning finished */
};

const char *control_desc[] = {
    [RDMA_CONTROL_NONE] = "NONE",
    [RDMA_CONTROL_ERROR] = "ERROR",
    [RDMA_CONTROL_READY] = "READY",
    [RDMA_CONTROL_QEMU_FILE] = "QEMU FILE",
    [RDMA_CONTROL_RAM_BLOCKS_REQUEST] = "RAM BLOCKS REQUEST",
    [RDMA_CONTROL_RAM_BLOCKS_RESULT] = "RAM BLOCKS RESULT",
    [RDMA_CONTROL_COMPRESS] = "COMPRESS",
    [RDMA_CONTROL_REGISTER_REQUEST] = "REGISTER REQUEST",
    [RDMA_CONTROL_REGISTER_RESULT] = "REGISTER RESULT",
    [RDMA_CONTROL_REGISTER_FINISHED] = "REGISTER FINISHED",
    [RDMA_CONTROL_UNREGISTER_REQUEST] = "UNREGISTER REQUEST",
    [RDMA_CONTROL_UNREGISTER_FINISHED] = "UNREGISTER FINISHED",
};

/*
 * Memory and MR structures used to represent an IB Send/Recv work request.
 * This is *not* used for RDMA writes, only IB Send/Recv.
 */
typedef struct {
    uint8_t  control[RDMA_CONTROL_MAX_BUFFER]; /* actual buffer to register */
    struct   ibv_mr *control_mr;               /* registration metadata */
    size_t   control_len;                      /* length of the message */
    uint8_t *control_curr;                     /* start of unconsumed bytes */
} RDMAWorkRequestData;

/*
 * Negotiate RDMA capabilities during connection-setup time.
 */
typedef struct QEMU_PACKED RDMACapabilities {
    uint32_t version;
    uint32_t flags;
    uint32_t keepalive_rkey;
    uint64_t keepalive_addr;
} RDMACapabilities;

static uint64_t htonll(uint64_t v)
{
    union { uint32_t lv[2]; uint64_t llv; } u;
    u.lv[0] = htonl(v >> 32);
    u.lv[1] = htonl(v & 0xFFFFFFFFULL);
    return u.llv;
}

static uint64_t ntohll(uint64_t v) {
    union { uint32_t lv[2]; uint64_t llv; } u;
    u.llv = v;
    return ((uint64_t)ntohl(u.lv[0]) << 32) | (uint64_t) ntohl(u.lv[1]);
}

static void caps_to_network(RDMACapabilities *cap)
{
    cap->version = htonl(cap->version);
    cap->flags = htonl(cap->flags);
    cap->keepalive_rkey = htonl(cap->keepalive_rkey);
    cap->keepalive_addr = htonll(cap->keepalive_addr);
}

static void network_to_caps(RDMACapabilities *cap)
{
    cap->version = ntohl(cap->version);
    cap->flags = ntohl(cap->flags);
    cap->keepalive_rkey = ntohl(cap->keepalive_rkey);
    cap->keepalive_addr = ntohll(cap->keepalive_addr);
}

/*
 * Representation of a RAMBlock from an RDMA perspective.
 * This is not transmitted, only local.
 * This and subsequent structures cannot be linked lists
 * because we're using a single IB message to transmit
 * the information. It's small anyway, so a list is overkill.
 */
typedef struct RDMALocalBlock {
    uint8_t  *local_host_addr; /* local virtual address */
    uint64_t remote_host_addr; /* remote virtual address */
    uint64_t offset;
    uint64_t length;
    struct ibv_mr **pmr;      /* MRs for remote chunk-level registration */
    struct ibv_mr *mr;        /* MR for non-chunk-level registration */
    struct ibv_mr **pmr_src;  /* MRs for copy chunk-level registration */
    struct ibv_mr *mr_src;    /* MR for copy non-chunk-level registration */
    struct ibv_mr **pmr_dest; /* MRs for copy chunk-level registration */
    struct ibv_mr *mr_dest;   /* MR for copy non-chunk-level registration */
    uint32_t *remote_keys;    /* rkeys for chunk-level registration */
    uint32_t remote_rkey;     /* rkeys for non-chunk-level registration */
    int      index;           /* which block are we */
    bool     is_ram_block;
    int      nb_chunks;
    unsigned long *transit_bitmap;
    unsigned long *unregister_bitmap;
} RDMALocalBlock;

/*
 * Also represents a RAMblock, but only on the dest.
 * This gets transmitted by the dest during connection-time
 * to the source VM and then is used to populate the
 * corresponding RDMALocalBlock with
 * the information needed to perform the actual RDMA.
 */
typedef struct QEMU_PACKED RDMARemoteBlock {
    uint64_t remote_host_addr;
    uint64_t offset;
    uint64_t length;
    uint32_t remote_rkey;
    uint32_t padding;
} RDMARemoteBlock;

static void remote_block_to_network(RDMARemoteBlock *rb)
{
    rb->remote_host_addr = htonll(rb->remote_host_addr);
    rb->offset = htonll(rb->offset);
    rb->length = htonll(rb->length);
    rb->remote_rkey = htonl(rb->remote_rkey);
}

static void network_to_remote_block(RDMARemoteBlock *rb)
{
    rb->remote_host_addr = ntohll(rb->remote_host_addr);
    rb->offset = ntohll(rb->offset);
    rb->length = ntohll(rb->length);
    rb->remote_rkey = ntohl(rb->remote_rkey);
}

/*
 * Virtual address of the above structures used for transmitting
 * the RAMBlock descriptions at connection-time.
 * This structure is *not* transmitted.
 */
typedef struct RDMALocalBlocks {
    int nb_blocks;
    bool     init;             /* main memory init complete */
    RDMALocalBlock *block;
} RDMALocalBlocks;

/*
 * We provide RDMA to QEMU by way of 2 mechanisms:
 *
 * 1. Local copy to remote copy
 * 2. Local copy to local copy - like memcpy().
 *
 * Three instances of this structure are maintained inside of RDMAContext
 * to manage both mechanisms.
 */
typedef struct RDMACurrentChunk {
    /* store info about current buffer so that we can
       merge it with future sends */
    uint64_t current_addr;
    uint64_t current_length;
    /* index of ram block the current buffer belongs to */
    int current_block_idx;
    /* index of the chunk in the current ram block */
    int current_chunk;

    uint64_t block_offset;
    uint64_t offset;

    /* parameters for qemu_rdma_write() */
    uint64_t chunk_idx;
    uint8_t *chunk_start;
    uint8_t *chunk_end;
    RDMALocalBlock *block;
    uint8_t *addr;
    uint64_t chunks;
} RDMACurrentChunk;

/*
 * Three copies of the following strucuture are used to hold the infiniband
 * connection variables for each of the aformentioned mechanisms, one for
 * remote copy and two local copy.
 */
typedef struct RDMALocalContext {
    struct ibv_context *verbs;
    struct ibv_pd *pd;
    struct ibv_comp_channel *comp_chan;
    struct ibv_cq *cq;
    struct ibv_qp_init_attr qp_attr;
    struct ibv_qp *qp;
    union ibv_gid gid;
    struct ibv_port_attr port;
    uint64_t psn;
    int port_num;
    int nb_sent;
    int64_t start_time;
    int max_nb_sent;
    const char * id_str;
} RDMALocalContext;

/*
 * Main data structure for RDMA state.
 * While there is only one copy of this structure being allocated right now,
 * this is the place where one would start if you wanted to consider
 * having more than one RDMA connection open at the same time.
 *
 * It is used for performing both local and remote RDMA operations
 * with a single RDMA connection.
 *
 * Local operations are done by allocating separate queue pairs after
 * the initial RDMA remote connection is initalized.
 */
typedef struct RDMAContext {
    char *host;
    int port;

    RDMAWorkRequestData wr_data[RDMA_WRID_MAX];

    /*
     * This is used by *_exchange_send() to figure out whether or not
     * the initial "READY" message has already been received or not.
     * This is because other functions may potentially poll() and detect
     * the READY message before send() does, in which case we need to
     * know if it completed.
     */
    int control_ready_expected;

    /* number of posts */
    int nb_sent;

    RDMACurrentChunk chunk_remote;
    RDMACurrentChunk chunk_local_src;
    RDMACurrentChunk chunk_local_dest;

    bool pin_all;
    bool do_keepalive;

    /*
     * infiniband-specific variables for opening the device
     * and maintaining connection state and so forth.
     *
     * cm_id also has ibv_context, rdma_event_channel, and ibv_qp in
     * cm_id->verbs, cm_id->channel, and cm_id->qp.
     */
    struct rdma_cm_id *cm_id;               /* connection manager ID */
    struct rdma_cm_id *listen_id;
    bool connected;

    struct ibv_context          *verbs;
    struct rdma_event_channel   *channel;
    struct ibv_qp *qp;                      /* queue pair */
    struct ibv_comp_channel *comp_channel;  /* completion channel */
    struct ibv_pd *pd;                      /* protection domain */
    struct ibv_cq *cq;                      /* completion queue */

    /*
     * If a previous write failed (perhaps because of a failed
     * memory registration, then do not attempt any future work
     * and remember the error state.
     */
    int error_state;
    int error_reported;

    /*
     * Description of ram blocks used throughout the code.
     */
    RDMALocalBlocks local_ram_blocks;
    RDMARemoteBlock *block;

    /*
     * Migration on *destination* started.
     * Then use coroutine yield function.
     * Source runs in a thread, so we don't care.
     */
    bool migration_started;

    int total_registrations;
    int total_writes;

    int unregister_current, unregister_next;
    uint64_t unregistrations[RDMA_SEND_MAX];

    GHashTable *blockmap;

    uint64_t keepalive;
    uint64_t last_keepalive;
    uint64_t nb_missed_keepalive;
    uint64_t next_keepalive;
    struct ibv_mr *keepalive_mr;
    struct ibv_mr *next_keepalive_mr;
    uint32_t keepalive_rkey;
    uint64_t keepalive_addr; 
    bool keepalive_startup; 

    RDMALocalContext lc_src;
    RDMALocalContext lc_dest;
    RDMALocalContext lc_remote;

    /* who are we? */
    bool source;
    bool dest;
} RDMAContext;

static void close_ibv(RDMAContext *rdma, RDMALocalContext *lc)
{

    if (lc->qp) {
        struct ibv_qp_attr attr = {.qp_state = IBV_QPS_ERR };
        ibv_modify_qp(lc->qp, &attr, IBV_QP_STATE);
        ibv_destroy_qp(lc->qp);
        lc->qp = NULL;
    }

    if (lc->cq) {
        ibv_destroy_cq(lc->cq);
        lc->cq = NULL;
    }

    if (lc->comp_chan) {
        ibv_destroy_comp_channel(lc->comp_chan);
        lc->comp_chan = NULL;
    }

    if (lc->pd) {
        ibv_dealloc_pd(lc->pd);
        lc->pd = NULL;
    }

    if (lc->verbs) {
        ibv_close_device(lc->verbs);
        lc->verbs = NULL;
    }
}

/*
 * Create protection domain and completion queues
 */
static int qemu_rdma_alloc_pd_cq(RDMAContext *rdma, RDMALocalContext *lc)
{
    struct rlimit r = { .rlim_cur = RLIM_INFINITY, .rlim_max = RLIM_INFINITY };
     
    if (getrlimit(RLIMIT_MEMLOCK, &r) < 0) {
        perror("getrlimit");
        ERROR(NULL, "getrlimit(RLIMIT_MEMLOCK)");
        goto err_alloc;
    }

    DPRINTF("MemLock Limits cur: %" PRId64 " max: %" PRId64 "\n",
            r.rlim_cur, r.rlim_max);

    lc->pd = ibv_alloc_pd(lc->verbs);
    if (!lc->pd) {
        ERROR(NULL, "allocate protection domain");
        goto err_alloc;
    }

    /* create completion channel */
    lc->comp_chan = ibv_create_comp_channel(lc->verbs);
    if (!lc->comp_chan) {
        ERROR(NULL, "allocate completion channel");
        goto err_alloc;
    }

    /*
     * Completion queue can be filled by both read and write work requests,
     * so must reflect the sum of both possible queue sizes.
     */
    lc->cq = ibv_create_cq(lc->verbs, (RDMA_SEND_MAX * 3), NULL, 
                           lc->comp_chan, 0);
    if (!lc->cq) {
        ERROR(NULL, "allocate completion queue");
        goto err_alloc;
    }

    return 0;

err_alloc:
    close_ibv(rdma, lc);
    return -EINVAL;
}

static int open_local(RDMAContext *rdma, RDMALocalContext *lc)
{
	struct ibv_qp_attr set_attr = {
		.qp_state = IBV_QPS_INIT,
		.pkey_index = 0,
		.qp_access_flags = IBV_ACCESS_REMOTE_WRITE | IBV_ACCESS_REMOTE_READ | IBV_ACCESS_LOCAL_WRITE,
	};
    struct ibv_qp_attr query_attr;
    struct ibv_qp_init_attr query_init_attr;
    int ret;

    lc->psn = lrand48() & 0xffffff;

    ret = ibv_query_qp(rdma->lc_remote.qp, &query_attr, IBV_QP_PORT, &query_init_attr);

    if (ret) {
        ret = EINVAL;
        ERROR(NULL, "query original QP state");
        goto err;
    }

    lc->port_num = query_attr.port_num;
    set_attr.port_num = lc->port_num;

    lc->verbs = ibv_open_device(rdma->lc_remote.verbs->device);

	if(lc->verbs == NULL) {
        ret = EINVAL;
        ERROR(NULL, "open device!");
        goto err;
	}

    ret = qemu_rdma_alloc_pd_cq(rdma, lc);

    if (ret) {
        ret = -ret;
        ERROR(NULL, "Local ibv structure allocations");
        goto err;
    }

    if (rdma->dest) {
        qemu_set_nonblock(lc->comp_chan->fd);
    }

	lc->qp_attr.cap.max_send_wr	 = RDMA_SEND_MAX;
	lc->qp_attr.cap.max_recv_wr	 = 3;
	lc->qp_attr.cap.max_send_sge = 1;
	lc->qp_attr.cap.max_recv_sge = 1;
    lc->qp_attr.send_cq          = lc->cq;
	lc->qp_attr.recv_cq		     = lc->cq;
	lc->qp_attr.qp_type		     = IBV_QPT_RC;

	lc->qp = ibv_create_qp(lc->pd, &lc->qp_attr);
	if (!lc->qp) {
        ret = EINVAL;
        ERROR(NULL, "create queue pair!");
        goto err;
    }

	ret = ibv_modify_qp(lc->qp, &set_attr,
		IBV_QP_STATE | IBV_QP_PKEY_INDEX | IBV_QP_PORT | IBV_QP_ACCESS_FLAGS);

    if (ret) {
		ERROR(NULL, "verbs to init!");
        goto err;
	}	
	
	ret = ibv_query_port(lc->verbs, lc->port_num, &lc->port);

    if (ret) {
		ERROR(NULL, "query port attributes!");
        goto err;
    }
	
	ret = ibv_query_gid(lc->verbs, 1, 0, &lc->gid);

    if (ret) {
		ERROR(NULL, "Failed to query gid!");
        goto err;
    }

    return 0;
err:
    SET_ERROR(rdma, -ret);
    return rdma->error_state;
}

/*
 * Interface to the rest of the migration call stack.
 */
typedef struct QEMUFileRDMA {
    RDMAContext *rdma;
    size_t len;
    void *file;
} QEMUFileRDMA;

/*
 * Main structure for IB Send/Recv control messages.
 * This gets prepended at the beginning of every Send/Recv.
 */
typedef struct QEMU_PACKED {
    uint32_t len;     /* Total length of data portion */
    uint32_t type;    /* which control command to perform */
    uint32_t repeat;  /* number of commands in data portion of same type */
    uint32_t padding;
} RDMAControlHeader;

static void control_to_network(RDMAControlHeader *control)
{
    control->type = htonl(control->type);
    control->len = htonl(control->len);
    control->repeat = htonl(control->repeat);
}

static void network_to_control(RDMAControlHeader *control)
{
    control->type = ntohl(control->type);
    control->len = ntohl(control->len);
    control->repeat = ntohl(control->repeat);
}

/*
 * Register a single Chunk.
 * Information sent by the source VM to inform the dest
 * to register an single chunk of memory before we can perform
 * the actual RDMA operation.
 */
typedef struct QEMU_PACKED {
    union QEMU_PACKED {
        uint64_t current_addr;  /* offset into the ramblock of the chunk */
        uint64_t chunk;         /* chunk to lookup if unregistering */
    } key;
    uint32_t current_block_idx;     /* which ramblock the chunk belongs to */
    uint32_t padding;
    uint64_t chunks;            /* how many sequential chunks to register */
} RDMARegister;

static void register_to_network(RDMARegister *reg)
{
    reg->key.current_addr = htonll(reg->key.current_addr);
    reg->current_block_idx = htonl(reg->current_block_idx);
    reg->chunks = htonll(reg->chunks);
}

static void network_to_register(RDMARegister *reg)
{
    reg->key.current_addr = ntohll(reg->key.current_addr);
    reg->current_block_idx = ntohl(reg->current_block_idx);
    reg->chunks = ntohll(reg->chunks);
}

typedef struct QEMU_PACKED {
    uint32_t value;     /* if zero, we will madvise() */
    uint32_t block_idx; /* which ram block index */
    uint64_t offset;    /* where in the remote ramblock this chunk */
    uint64_t length;    /* length of the chunk */
} RDMACompress;

static void compress_to_network(RDMACompress *comp)
{
    comp->value = htonl(comp->value);
    comp->block_idx = htonl(comp->block_idx);
    comp->offset = htonll(comp->offset);
    comp->length = htonll(comp->length);
}

static void network_to_compress(RDMACompress *comp)
{
    comp->value = ntohl(comp->value);
    comp->block_idx = ntohl(comp->block_idx);
    comp->offset = ntohll(comp->offset);
    comp->length = ntohll(comp->length);
}

/*
 * The result of the dest's memory registration produces an "rkey"
 * which the source VM must reference in order to perform
 * the RDMA operation.
 */
typedef struct QEMU_PACKED {
    uint32_t rkey;
    uint32_t padding;
    uint64_t host_addr;
} RDMARegisterResult;

static void result_to_network(RDMARegisterResult *result)
{
    result->rkey = htonl(result->rkey);
    result->host_addr = htonll(result->host_addr);
};

static void network_to_result(RDMARegisterResult *result)
{
    result->rkey = ntohl(result->rkey);
    result->host_addr = ntohll(result->host_addr);
};

const char *print_wrid(int wrid);
static int qemu_rdma_exchange_send(RDMAContext *rdma, RDMAControlHeader *head,
                                   uint8_t *data, RDMAControlHeader *resp,
                                   int *resp_idx,
                                   int (*callback)(RDMAContext *rdma));

static inline uint64_t ram_chunk_index(const uint8_t *start,
                                       const uint8_t *host)
{
    return ((uintptr_t) host - (uintptr_t) start) >> RDMA_REG_CHUNK_SHIFT;
}

static inline uint8_t *ram_chunk_start(const RDMALocalBlock *rdma_ram_block,
                                       uint64_t i)
{
    return (uint8_t *) (((uintptr_t) rdma_ram_block->local_host_addr)
                                    + (i << RDMA_REG_CHUNK_SHIFT));
}

static inline uint8_t *ram_chunk_end(const RDMALocalBlock *rdma_ram_block,
                                     uint64_t i)
{
    uint8_t *result = ram_chunk_start(rdma_ram_block, i) +
                                         (1UL << RDMA_REG_CHUNK_SHIFT);

    if (result > (rdma_ram_block->local_host_addr + rdma_ram_block->length)) {
        result = rdma_ram_block->local_host_addr + rdma_ram_block->length;
    }

    return result;
}

static int __qemu_rdma_add_block(RDMAContext *rdma, void *host_addr,
                         ram_addr_t block_offset, uint64_t length)
{
    RDMALocalBlocks *local = &rdma->local_ram_blocks;
    RDMALocalBlock *block = g_hash_table_lookup(rdma->blockmap,
        (void *) block_offset);
    RDMALocalBlock *old = local->block;

    assert(block == NULL);

    local->block = g_malloc0(sizeof(RDMALocalBlock) * (local->nb_blocks + 1));

    if (local->nb_blocks) {
        int x;

        for (x = 0; x < local->nb_blocks; x++) {
            g_hash_table_remove(rdma->blockmap, (void *)old[x].offset);
            g_hash_table_insert(rdma->blockmap, (void *)old[x].offset,
                                                &local->block[x]);
        }
        memcpy(local->block, old, sizeof(RDMALocalBlock) * local->nb_blocks);
        g_free(old);
    }

    block = &local->block[local->nb_blocks];

    block->local_host_addr = host_addr;
    block->offset = block_offset;
    block->length = length;
    block->index = local->nb_blocks;
    block->nb_chunks = ram_chunk_index(host_addr, host_addr + length) + 1UL;
    block->transit_bitmap = bitmap_new(block->nb_chunks);
    bitmap_clear(block->transit_bitmap, 0, block->nb_chunks);
    block->unregister_bitmap = bitmap_new(block->nb_chunks);
    bitmap_clear(block->unregister_bitmap, 0, block->nb_chunks);
    block->remote_keys = g_malloc0(block->nb_chunks * sizeof(uint32_t));

    block->is_ram_block = local->init ? false : true;

    g_hash_table_insert(rdma->blockmap, (void *) block_offset, block);

    DDPRINTF("Added Block: %d, addr: %p, offset: %" PRIu64
           " length: %" PRIu64 " end: %p bits %" PRIu64 " chunks %d\n",
            local->nb_blocks, block->local_host_addr, block->offset,
            block->length, block->local_host_addr + block->length,
                BITS_TO_LONGS(block->nb_chunks) *
                    sizeof(unsigned long) * 8, block->nb_chunks);

    local->nb_blocks++;

    return 0;
}

/*
 * Memory regions need to be registered with the device and queue pairs setup
 * in advanced before the migration starts. This tells us where the RAM blocks
 * are so that we can register them individually.
 */
static void qemu_rdma_init_one_block(void *host_addr,
    ram_addr_t block_offset, ram_addr_t length, void *opaque)
{
    __qemu_rdma_add_block(opaque, host_addr, block_offset, length);
}

/*
 * Identify the RAMBlocks and their quantity. They will be references to
 * identify chunk boundaries inside each RAMBlock and also be referenced
 * during dynamic page registration.
 */
static int qemu_rdma_init_ram_blocks(RDMAContext *rdma)
{
    RDMALocalBlocks *local = &rdma->local_ram_blocks;

    assert(rdma->blockmap == NULL);
    rdma->blockmap = g_hash_table_new(g_direct_hash, g_direct_equal);
    memset(local, 0, sizeof *local);
    qemu_ram_foreach_block(qemu_rdma_init_one_block, rdma);
    DPRINTF("Allocated %d local ram block structures\n", local->nb_blocks);
    rdma->block = (RDMARemoteBlock *) g_malloc0(sizeof(RDMARemoteBlock) *
                        rdma->local_ram_blocks.nb_blocks);
    local->init = true;
    return 0;
}

static void qemu_rdma_free_pmrs(RDMAContext *rdma, RDMALocalBlock *block,
                               struct ibv_mr ***mrs)
{
    if (*mrs) {
        int j;

        for (j = 0; j < block->nb_chunks; j++) {
            if (!(*mrs)[j]) {
                continue;
            }
            ibv_dereg_mr((*mrs)[j]);
            rdma->total_registrations--;
        }
        g_free(*mrs);

        *mrs = NULL;
    }
}

static void qemu_rdma_free_mr(RDMAContext *rdma, struct ibv_mr **mr)
{
    if (*mr) {
        ibv_dereg_mr(*mr);
        rdma->total_registrations--;
        *mr = NULL;
    }
}

static int __qemu_rdma_delete_block(RDMAContext *rdma, ram_addr_t block_offset)
{
    RDMALocalBlocks *local = &rdma->local_ram_blocks;
    RDMALocalBlock *block = g_hash_table_lookup(rdma->blockmap,
        (void *) block_offset);
    RDMALocalBlock *old = local->block;
    int x;

    assert(block);

    qemu_rdma_free_pmrs(rdma, block, &block->pmr);
    qemu_rdma_free_pmrs(rdma, block, &block->pmr_src);
    qemu_rdma_free_pmrs(rdma, block, &block->pmr_dest);

    qemu_rdma_free_mr(rdma, &block->mr);
    qemu_rdma_free_mr(rdma, &block->mr_src);
    qemu_rdma_free_mr(rdma, &block->mr_dest);

    g_free(block->transit_bitmap);
    block->transit_bitmap = NULL;

    g_free(block->unregister_bitmap);
    block->unregister_bitmap = NULL;

    g_free(block->remote_keys);
    block->remote_keys = NULL;

    for (x = 0; x < local->nb_blocks; x++) {
        g_hash_table_remove(rdma->blockmap, (void *)old[x].offset);
    }

    if (local->nb_blocks > 1) {

        local->block = g_malloc0(sizeof(RDMALocalBlock) *
                                    (local->nb_blocks - 1));

        if (block->index) {
            memcpy(local->block, old, sizeof(RDMALocalBlock) * block->index);
        }

        if (block->index < (local->nb_blocks - 1)) {
            RDMALocalBlock * end = old + (block->index + 1);
            for (x = 0; x < (local->nb_blocks - (block->index + 1)); x++) {
                end[x].index--;
            }

            memcpy(local->block + block->index, end,
                sizeof(RDMALocalBlock) *
                    (local->nb_blocks - (block->index + 1)));
        }
    } else {
        assert(block == local->block);
        local->block = NULL;
    }

    g_free(old);

    local->nb_blocks--;

    DDPRINTF("Deleted Block: %d, addr: %" PRIu64 ", offset: %" PRIu64
           " length: %" PRIu64 " end: %" PRIu64 " bits %" PRIu64 " chunks %d\n",
            local->nb_blocks, (uint64_t) block->local_host_addr, block->offset,
            block->length, (uint64_t) (block->local_host_addr + block->length),
                BITS_TO_LONGS(block->nb_chunks) *
                    sizeof(unsigned long) * 8, block->nb_chunks);

    if (local->nb_blocks) {
        for (x = 0; x < local->nb_blocks; x++) {
            g_hash_table_insert(rdma->blockmap, (void *)local->block[x].offset,
                                                &local->block[x]);
        }
    }

    return 0;
}

/*
 * Put in the log file which RDMA device was opened and the details
 * associated with that device.
 */
static void qemu_rdma_dump_id(const char *who, struct ibv_context *verbs)
{
    struct ibv_port_attr port;

    if (ibv_query_port(verbs, 1, &port)) {
        fprintf(stderr, "FAILED TO QUERY PORT INFORMATION!\n");
        return;
    }

    printf("%s RDMA Device opened: kernel name %s "
           "uverbs device name %s, "
           "infiniband_verbs class device path %s, "
           "infiniband class device path %s, "
           "transport: (%d) %s\n",
                who,
                verbs->device->name,
                verbs->device->dev_name,
                verbs->device->dev_path,
                verbs->device->ibdev_path,
                port.link_layer,
                (port.link_layer == IBV_LINK_LAYER_INFINIBAND) ? "Infiniband" :
                 ((port.link_layer == IBV_LINK_LAYER_ETHERNET) 
                    ? "Ethernet" : "Unknown"));
}

/*
 * Put in the log file the RDMA gid addressing information,
 * useful for folks who have trouble understanding the
 * RDMA device hierarchy in the kernel.
 */
static void qemu_rdma_dump_gid(const char *who, struct rdma_cm_id *id)
{
    char sgid[33];
    char dgid[33];
    inet_ntop(AF_INET6, &id->route.addr.addr.ibaddr.sgid, sgid, sizeof sgid);
    inet_ntop(AF_INET6, &id->route.addr.addr.ibaddr.dgid, dgid, sizeof dgid);
    DPRINTF("%s Source GID: %s, Dest GID: %s\n", who, sgid, dgid);
}

/*
 * As of now, IPv6 over RoCE / iWARP is not supported by linux.
 * We will try the next addrinfo struct, and fail if there are
 * no other valid addresses to bind against.
 *
 * If user is listening on '[::]', then we will not have a opened a device
 * yet and have no way of verifying if the device is RoCE or not.
 *
 * In this case, the source VM will throw an error for ALL types of
 * connections (both IPv4 and IPv6) if the destination machine does not have
 * a regular infiniband network available for use.
 *
 * The only way to guarantee that an error is thrown for broken kernels is
 * for the management software to choose a *specific* interface at bind time
 * and validate what time of hardware it is.
 *
 * Unfortunately, this puts the user in a fix:
 * 
 *  If the source VM connects with an IPv4 address without knowing that the
 *  destination has bound to '[::]' the migration will unconditionally fail
 *  unless the management software is explicitly listening on the the IPv4
 *  address while using a RoCE-based device.
 *
 *  If the source VM connects with an IPv6 address, then we're OK because we can
 *  throw an error on the source (and similarly on the destination).
 * 
 *  But in mixed environments, this will be broken for a while until it is fixed
 *  inside linux.
 *
 * We do provide a *tiny* bit of help in this function: We can list all of the
 * devices in the system and check to see if all the devices are RoCE or
 * Infiniband. 
 *
 * If we detect that we have a *pure* RoCE environment, then we can safely
 * thrown an error even if the management software has specified '[::]' as the
 * bind address.
 *
 * However, if there is are multiple hetergeneous devices, then we cannot make
 * this assumption and the user just has to be sure they know what they are
 * doing.
 *
 * Patches are being reviewed on linux-rdma.
 */
static int qemu_rdma_broken_ipv6_kernel(Error **errp, struct ibv_context *verbs)
{
    struct ibv_port_attr port_attr;

    /* This bug only exists in linux, to our knowledge. */
#ifdef CONFIG_LINUX

    /* 
     * Verbs are only NULL if management has bound to '[::]'.
     * 
     * Let's iterate through all the devices and see if there any pure IB
     * devices (non-ethernet).
     * 
     * If not, then we can safely proceed with the migration.
     * Otherwise, there are no guarantees until the bug is fixed in linux.
     */
    if (!verbs) {
	    int num_devices, x;
        struct ibv_device ** dev_list = ibv_get_device_list(&num_devices);
        bool roce_found = false;
        bool ib_found = false;

        for (x = 0; x < num_devices; x++) {
            verbs = ibv_open_device(dev_list[x]);

            if (ibv_query_port(verbs, 1, &port_attr)) {
                ibv_close_device(verbs);
                ERROR(errp, "Could not query initial IB port");
                return -EINVAL;
            }

            if (port_attr.link_layer == IBV_LINK_LAYER_INFINIBAND) {
                ib_found = true;
            } else if (port_attr.link_layer == IBV_LINK_LAYER_ETHERNET) {
                roce_found = true;
            }

            ibv_close_device(verbs);

        }

        if (roce_found) {
            if (ib_found) {
                fprintf(stderr, "WARN: migrations may fail:"
                                " IPv6 over RoCE / iWARP in linux"
                                " is broken. But since you appear to have a"
                                " mixed RoCE / IB environment, be sure to only"
                                " migrate over the IB fabric until the kernel "
                                " fixes the bug.\n");
            } else {
                ERROR(errp, "You only have RoCE / iWARP devices in your systems"
                            " and your management software has specified '[::]'"
                            ", but IPv6 over RoCE / iWARP is not supported in Linux.");
                return -ENONET;
            }
        }

        return 0;
    }

    /*
     * If we have a verbs context, that means that some other than '[::]' was
     * used by the management software for binding. In which case we can actually 
     * warn the user about a potential broken kernel;
     */

    /* IB ports start with 1, not 0 */
    if (ibv_query_port(verbs, 1, &port_attr)) {
        ERROR(errp, "Could not query initial IB port");
        return -EINVAL;
    }

    if (port_attr.link_layer == IBV_LINK_LAYER_ETHERNET) {
        ERROR(errp, "Linux kernel's RoCE / iWARP does not support IPv6 "
                    "(but patches on linux-rdma in progress)");
        return -ENONET;
    }

#endif

    return 0;
}

/*
 * Figure out which RDMA device corresponds to the requested IP hostname
 * Also create the initial connection manager identifiers for opening
 * the connection.
 */
static int qemu_rdma_resolve_host(RDMAContext *rdma, Error **errp)
{
    int ret;
    struct rdma_addrinfo *res;
    char port_str[16];
    struct rdma_cm_event *cm_event;
    char ip[40] = "unknown";
    struct rdma_addrinfo *e;

    if (rdma->host == NULL || !strcmp(rdma->host, "")) {
        ERROR(errp, "RDMA hostname has not been set");
        return -EINVAL;
    }

    /* create CM channel */
    rdma->channel = rdma_create_event_channel();
    if (!rdma->channel) {
        ERROR(errp, "could not create CM channel");
        return -EINVAL;
    }

    /* create CM id */
    ret = rdma_create_id(rdma->channel, &rdma->cm_id, NULL, RDMA_PS_TCP);
    if (ret) {
        ERROR(errp, "could not create channel id");
        goto err_resolve_create_id;
    }

    snprintf(port_str, 16, "%d", rdma->port);
    port_str[15] = '\0';

    ret = rdma_getaddrinfo(rdma->host, port_str, NULL, &res);
    if (ret < 0) {
        ERROR(errp, "could not rdma_getaddrinfo address %s", rdma->host);
        goto err_resolve_get_addr;
    }

    for (e = res; e != NULL; e = e->ai_next) {
        inet_ntop(e->ai_family,
            &((struct sockaddr_in *) e->ai_dst_addr)->sin_addr, ip, sizeof ip);
        DPRINTF("Trying %s => %s\n", rdma->host, ip);

        ret = rdma_resolve_addr(rdma->cm_id, NULL, e->ai_dst_addr,
                RDMA_RESOLVE_TIMEOUT_MS);
        if (!ret) {
            if (e->ai_family == AF_INET6) {
                ret = qemu_rdma_broken_ipv6_kernel(errp, rdma->cm_id->verbs);
                if (ret) {
                    continue;
                }
            }
            goto route;
        }
    }

    ERROR(errp, "could not resolve address %s", rdma->host);
    goto err_resolve_get_addr;

route:
    qemu_rdma_dump_gid("source_resolve_addr", rdma->cm_id);

    ret = rdma_get_cm_event(rdma->channel, &cm_event);
    if (ret) {
        ERROR(errp, "could not perform event_addr_resolved");
        goto err_resolve_get_addr;
    }

    if (cm_event->event != RDMA_CM_EVENT_ADDR_RESOLVED) {
        ERROR(errp, "result not equal to event_addr_resolved %s",
                rdma_event_str(cm_event->event));
        perror("rdma_resolve_addr");
        ret = -EINVAL;
        goto err_resolve_get_addr;
    }
    rdma_ack_cm_event(cm_event);

    /* resolve route */
    ret = rdma_resolve_route(rdma->cm_id, RDMA_RESOLVE_TIMEOUT_MS);
    if (ret) {
        ERROR(errp, "could not resolve rdma route");
        goto err_resolve_get_addr;
    }

    ret = rdma_get_cm_event(rdma->channel, &cm_event);
    if (ret) {
        ERROR(errp, "could not perform event_route_resolved");
        goto err_resolve_get_addr;
    }
    if (cm_event->event != RDMA_CM_EVENT_ROUTE_RESOLVED) {
        ERROR(errp, "result not equal to event_route_resolved: %s",
                        rdma_event_str(cm_event->event));
        rdma_ack_cm_event(cm_event);
        ret = -EINVAL;
        goto err_resolve_get_addr;
    }
    rdma_ack_cm_event(cm_event);
    rdma->lc_remote.verbs = rdma->cm_id->verbs;
    qemu_rdma_dump_id("source_resolve_host", rdma->cm_id->verbs);
    qemu_rdma_dump_gid("source_resolve_host", rdma->cm_id);
    return 0;

err_resolve_get_addr:
    rdma_destroy_id(rdma->cm_id);
    rdma->cm_id = NULL;
err_resolve_create_id:
    rdma_destroy_event_channel(rdma->channel);
    rdma->channel = NULL;
    return ret;
}

static int qemu_rdma_alloc_keepalive(RDMAContext *rdma)
{
    rdma->keepalive_mr = ibv_reg_mr(rdma->lc_remote.pd,
            &rdma->keepalive, sizeof(rdma->keepalive),
            IBV_ACCESS_LOCAL_WRITE | IBV_ACCESS_REMOTE_WRITE);

    if (!rdma->keepalive_mr) {
        perror("Failed to register keepalive location!");
        SET_ERROR(rdma, -ENOMEM);
        goto err_alloc;
    }

    rdma->next_keepalive_mr = ibv_reg_mr(rdma->lc_remote.pd,
            &rdma->next_keepalive, sizeof(rdma->next_keepalive),
            IBV_ACCESS_LOCAL_WRITE | IBV_ACCESS_REMOTE_WRITE);

    if (!rdma->next_keepalive_mr) {
        perror("Failed to register next keepalive location!");
        SET_ERROR(rdma, -ENOMEM);
        goto err_alloc;
    }

    return 0;

err_alloc:

    if (rdma->keepalive_mr) {
        ibv_dereg_mr(rdma->keepalive_mr);
        rdma->keepalive_mr = NULL;
    }

    if (rdma->next_keepalive_mr) {
        ibv_dereg_mr(rdma->next_keepalive_mr);
        rdma->next_keepalive_mr = NULL;
    }

    return -1;
}

/*
 * Create queue pairs.
 */
static int qemu_rdma_alloc_qp(RDMAContext *rdma)
{
    struct ibv_qp_init_attr attr = { 0 };
    int ret;

    attr.cap.max_send_wr = RDMA_SEND_MAX;
    attr.cap.max_recv_wr = 3;
    attr.cap.max_send_sge = 1;
    attr.cap.max_recv_sge = 1;
    attr.send_cq = rdma->lc_remote.cq;
    attr.recv_cq = rdma->lc_remote.cq;
    attr.qp_type = IBV_QPT_RC;

    ret = rdma_create_qp(rdma->cm_id, rdma->lc_remote.pd, &attr);
    if (ret) {
        return -1;
    }

    rdma->lc_remote.qp = rdma->cm_id->qp;
    return 0;
}

static int qemu_rdma_reg_whole_mr(RDMAContext *rdma, 
                                  struct ibv_pd *pd,
                                  struct ibv_mr **mr,
                                  int index)
{
    RDMALocalBlocks *local = &rdma->local_ram_blocks;

    *mr = ibv_reg_mr(pd,
                local->block[index].local_host_addr,
                local->block[index].length,
                IBV_ACCESS_LOCAL_WRITE |
                IBV_ACCESS_REMOTE_WRITE
                );
    if (!(*mr)) {
        perror("Failed to register local dest ram block!\n");
        return -1;
    }
    rdma->total_registrations++;

    return 0;
};

static int qemu_rdma_reg_whole_ram_blocks(RDMAContext *rdma)
{
    int i;
    RDMALocalBlocks *local = &rdma->local_ram_blocks;

    for (i = 0; i < local->nb_blocks; i++) {
        if (qemu_rdma_reg_whole_mr(rdma, rdma->lc_remote.pd, &local->block[i].mr, i)) {
            break;
        }

        /* TODO: make this optional if MC is disabled */
        if (rdma->source) {
            if (qemu_rdma_reg_whole_mr(rdma, rdma->lc_src.pd, 
                    &local->block[i].mr_src, i)) {
                break;
            }
        } else {
            if (qemu_rdma_reg_whole_mr(rdma, rdma->lc_dest.pd, 
                    &local->block[i].mr_dest, i)) {
                break;
            }
        }

    }

    if (i >= local->nb_blocks) {
        return 0;
    }

    for (i--; i >= 0; i--) {
        qemu_rdma_free_mr(rdma, &local->block[i].mr);
        qemu_rdma_free_mr(rdma, rdma->source ?
                                &local->block[i].mr_src :
                                &local->block[i].mr_dest);
    }

    return -1;

}

/*
 * Find the ram block that corresponds to the page requested to be
 * transmitted by QEMU.
 *
 * Once the block is found, also identify which 'chunk' within that
 * block that the page belongs to.
 *
 * This search cannot fail or the migration will fail.
 */
static int qemu_rdma_search_ram_block(RDMAContext *rdma,
                                      uint64_t block_offset,
                                      uint64_t offset,
                                      uint64_t length,
                                      uint64_t *block_index,
                                      uint64_t *chunk_index)
{
    uint64_t current_addr = block_offset + offset;
    RDMALocalBlock *block = g_hash_table_lookup(rdma->blockmap,
                                                (void *) block_offset);
    assert(block);
    assert(current_addr >= block->offset);
    assert((current_addr + length) <= (block->offset + block->length));

    *block_index = block->index;
    *chunk_index = ram_chunk_index(block->local_host_addr,
                block->local_host_addr + (current_addr - block->offset));

    return 0;
}

/*
 * Register a chunk with IB. If the chunk was already registered
 * previously, then skip.
 *
 * Also return the keys associated with the registration needed
 * to perform the actual RDMA operation.
 */
static int qemu_rdma_register_and_get_keys(RDMAContext *rdma,
                                           RDMACurrentChunk *cc,
                                           RDMALocalContext *lc,
                                           bool copy,
                                           uint32_t *lkey, 
                                           uint32_t *rkey)
{
    struct ibv_mr ***pmr = copy ? (rdma->source ? &cc->block->pmr_src : 
                           &cc->block->pmr_dest) : &cc->block->pmr;
    struct ibv_mr **mr = copy ? (rdma->source ? &cc->block->mr_src :
                         &cc->block->mr_dest) : &cc->block->mr;

    /*
     * Use pre-registered keys for the entire VM, if available.
     */
    if (*mr) {
        if (lkey) {
            *lkey = (*mr)->lkey;
        }
        if (rkey) {
            *rkey = (*mr)->rkey;
        }
        return 0;
    }

    /* allocate memory to store chunk MRs */
    if (!(*pmr)) {
        *pmr = g_malloc0(cc->block->nb_chunks * sizeof(struct ibv_mr *));
        if (!(*pmr)) {
            return -1;
        }
    }

    /*
     * If 'rkey', then we're the destination, so grant access to the source.
     *
     * If 'lkey', then we're the source, so grant access only to ourselves.
     */
    if (!(*pmr)[cc->chunk_idx]) {
        uint64_t len = cc->chunk_end - cc->chunk_start;

        DDPRINTF("Registering %" PRIu64 " bytes @ %p\n",
                 len, cc->chunk_start);

        (*pmr)[cc->chunk_idx] = ibv_reg_mr(lc->pd, cc->chunk_start, len,
                    (rkey ? (IBV_ACCESS_LOCAL_WRITE |
                            IBV_ACCESS_REMOTE_WRITE) : 0));

        if (!(*pmr)[cc->chunk_idx]) {
            perror("Failed to register chunk!");
            fprintf(stderr, "Chunk details: block: %d chunk index %" PRIu64
                            " start %" PRIu64 " end %" PRIu64 " host %" PRIu64
                            " local %" PRIu64 " registrations: %d\n",
                            cc->block->index, cc->chunk_idx, (uint64_t) cc->chunk_start,
                            (uint64_t) cc->chunk_end, (uint64_t) cc->addr,
                            (uint64_t) cc->block->local_host_addr,
                            rdma->total_registrations);
            return -1;
        }

        rdma->total_registrations++;
    }

    if (lkey) {
        *lkey = (*pmr)[cc->chunk_idx]->lkey;
    }
    if (rkey) {
        *rkey = (*pmr)[cc->chunk_idx]->rkey;
    }
    return 0;
}

/*
 * Register (at connection time) the memory used for control
 * channel messages.
 */
static int qemu_rdma_reg_control(RDMAContext *rdma, int idx)
{
    rdma->wr_data[idx].control_mr = ibv_reg_mr(rdma->lc_remote.pd,
            rdma->wr_data[idx].control, RDMA_CONTROL_MAX_BUFFER,
            IBV_ACCESS_LOCAL_WRITE | IBV_ACCESS_REMOTE_WRITE);
    if (rdma->wr_data[idx].control_mr) {
        rdma->total_registrations++;
        return 0;
    }
    fprintf(stderr, "qemu_rdma_reg_control failed!\n");
    return -1;
}

const char *print_wrid(int wrid)
{
    if (wrid >= RDMA_WRID_RECV_CONTROL) {
        return wrid_desc[RDMA_WRID_RECV_CONTROL];
    }
    return wrid_desc[wrid];
}

/*
 * RDMA requires memory registration (mlock/pinning), but this is not good for
 * overcommitment.
 *
 * In preparation for the future where LRU information or workload-specific
 * writable writable working set memory access behavior is available to QEMU
 * it would be nice to have in place the ability to UN-register/UN-pin
 * particular memory regions from the RDMA hardware when it is determine that
 * those regions of memory will likely not be accessed again in the near future.
 *
 * While we do not yet have such information right now, the following
 * compile-time option allows us to perform a non-optimized version of this
 * behavior.
 *
 * By uncommenting this option, you will cause *all* RDMA transfers to be
 * unregistered immediately after the transfer completes on both sides of the
 * connection. This has no effect in 'rdma-pin-all' mode, only regular mode.
 *
 * This will have a terrible impact on migration performance, so until future
 * workload information or LRU information is available, do not attempt to use
 * this feature except for basic testing.
 */
//#define RDMA_UNREGISTRATION_EXAMPLE

/*
 * Perform a non-optimized memory unregistration after every transfer
 * for demonsration purposes, only if pin-all is not requested.
 *
 * Potential optimizations:
 * 1. Start a new thread to run this function continuously
        - for bit clearing
        - and for receipt of unregister messages
 * 2. Use an LRU.
 * 3. Use workload hints.
 */
static int qemu_rdma_unregister_waiting(RDMAContext *rdma)
{
    while (rdma->unregistrations[rdma->unregister_current]) {
        int ret;
        uint64_t wr_id = rdma->unregistrations[rdma->unregister_current];
        uint64_t chunk =
            (wr_id & RDMA_WRID_CHUNK_MASK) >> RDMA_WRID_CHUNK_SHIFT;
        uint64_t block_index =
            (wr_id & RDMA_WRID_BLOCK_MASK) >> RDMA_WRID_BLOCK_SHIFT;
        RDMALocalBlock *block =
            &(rdma->local_ram_blocks.block[block_index]);
        RDMARegister reg = { .current_block_idx = block_index };
        RDMAControlHeader resp = { .type = RDMA_CONTROL_UNREGISTER_FINISHED,
                                 };
        RDMAControlHeader head = { .len = sizeof(RDMARegister),
                                   .type = RDMA_CONTROL_UNREGISTER_REQUEST,
                                   .repeat = 1,
                                 };

        DDPRINTF("Processing unregister for chunk: %" PRIu64
                 " at position %d\n", chunk, rdma->unregister_current);

        rdma->unregistrations[rdma->unregister_current] = 0;
        rdma->unregister_current++;

        if (rdma->unregister_current == RDMA_SEND_MAX) {
            rdma->unregister_current = 0;
        }


        /*
         * Unregistration is speculative (because migration is single-threaded
         * and we cannot break the protocol's inifinband message ordering).
         * Thus, if the memory is currently being used for transmission,
         * then abort the attempt to unregister and try again
         * later the next time a completion is received for this memory.
         */
        clear_bit(chunk, block->unregister_bitmap);

        if (test_bit(chunk, block->transit_bitmap)) {
            DDPRINTF("Cannot unregister inflight chunk: %" PRIu64 "\n", chunk);
            continue;
        }

        DDPRINTF("Sending unregister for chunk: %" PRIu64 "\n", chunk);

        ret = ibv_dereg_mr(block->pmr[chunk]);
        block->pmr[chunk] = NULL;
        block->remote_keys[chunk] = 0;

        if (ret != 0) {
            perror("unregistration chunk failed");
            return -ret;
        }
        rdma->total_registrations--;

        reg.key.chunk = chunk;
        register_to_network(&reg);
        ret = qemu_rdma_exchange_send(rdma, &head, (uint8_t *) &reg,
                                &resp, NULL, NULL);
        if (ret < 0) {
            return ret;
        }

        DDPRINTF("Unregister for chunk: %" PRIu64 " complete.\n", chunk);
    }

    return 0;
}

static uint64_t qemu_rdma_make_wrid(uint64_t wr_id, uint64_t index,
                                         uint64_t chunk)
{
    uint64_t result = wr_id & RDMA_WRID_TYPE_MASK;

    result |= (index << RDMA_WRID_BLOCK_SHIFT);
    result |= (chunk << RDMA_WRID_CHUNK_SHIFT);

    return result;
}

/*
 * Set bit for unregistration in the next iteration.
 * We cannot transmit right here, but will unpin later.
 */
static void qemu_rdma_signal_unregister(RDMAContext *rdma, uint64_t index,
                                        uint64_t chunk, uint64_t wr_id)
{
    if (rdma->unregistrations[rdma->unregister_next] != 0) {
        ERROR(NULL, "queue is full!");
    } else {
        RDMALocalBlock *block = &(rdma->local_ram_blocks.block[index]);

        if (!test_and_set_bit(chunk, block->unregister_bitmap)) {
            DDPRINTF("Appending unregister chunk %" PRIu64
                    " at position %d\n", chunk, rdma->unregister_next);

            rdma->unregistrations[rdma->unregister_next++] =
                    qemu_rdma_make_wrid(wr_id, index, chunk);

            if (rdma->unregister_next == RDMA_SEND_MAX) {
                rdma->unregister_next = 0;
            }
        } else {
            DDPRINTF("Unregister chunk %" PRIu64 " already in queue.\n",
                    chunk);
        }
    }
}

/*
 * Consult the connection manager to see a work request
 * (of any kind) has completed.
 * Return the work request ID that completed.
 */
static uint64_t qemu_rdma_poll(RDMAContext *rdma,
                               RDMALocalContext *lc, 
                               uint64_t *wr_id_out,
                               uint32_t *byte_len)
{
    int64_t current_time;
    int ret;
    struct ibv_wc wc;
    uint64_t wr_id;

    if (!lc->start_time) {
        lc->start_time = qemu_clock_get_ms(QEMU_CLOCK_REALTIME);
    }

    ret = ibv_poll_cq(lc->cq, 1, &wc);

    if (!ret) {
        *wr_id_out = RDMA_WRID_NONE;
        return 0;
    }

    if (ret < 0) {
        fprintf(stderr, "ibv_poll_cq return %d!\n", ret);
        return ret;
    }

    wr_id = wc.wr_id & RDMA_WRID_TYPE_MASK;

    if (wc.status != IBV_WC_SUCCESS) {
        fprintf(stderr, "ibv_poll_cq wc.status=%d %s!\n",
                        wc.status, ibv_wc_status_str(wc.status));
        fprintf(stderr, "ibv_poll_cq wrid=%s!\n", wrid_desc[wr_id]);

        return -1;
    }

    if (rdma->control_ready_expected &&
        (wr_id >= RDMA_WRID_RECV_CONTROL)) {
        DDDPRINTF("completion %s #%" PRId64 " received (%" PRId64 ")"
                  " left %d (per qp %d)\n", 
                  wrid_desc[RDMA_WRID_RECV_CONTROL],
                  wr_id - RDMA_WRID_RECV_CONTROL, wr_id, 
                  rdma->nb_sent, lc->nb_sent);
        rdma->control_ready_expected = 0;
    }

    if (wr_id == RDMA_WRID_RDMA_WRITE_REMOTE) {
        uint64_t chunk =
            (wc.wr_id & RDMA_WRID_CHUNK_MASK) >> RDMA_WRID_CHUNK_SHIFT;
        uint64_t block_idx =
            (wc.wr_id & RDMA_WRID_BLOCK_MASK) >> RDMA_WRID_BLOCK_SHIFT;
        RDMALocalBlock *block = &(rdma->local_ram_blocks.block[block_idx]);

        clear_bit(chunk, block->transit_bitmap);

        if (lc->nb_sent > lc->max_nb_sent) {
            lc->max_nb_sent = lc->nb_sent;
        }

        current_time = qemu_clock_get_ms(QEMU_CLOCK_REALTIME);
        
        if ((current_time - lc->start_time) > 1000) {
            lc->start_time = current_time;
            DDPRINTF("outstanding %s total: %d context: %d max %d\n",
                lc->id_str, rdma->nb_sent, lc->nb_sent, lc->max_nb_sent);
        }

        if (rdma->nb_sent > 0) {
            rdma->nb_sent--;
        }

        if (lc->nb_sent > 0) {
            lc->nb_sent--;
        }

        DDDPRINTF("completions %s (%" PRId64 ") left %d (per qp %d), "
                 "block %" PRIu64 ", chunk: %" PRIu64 " %p %p\n",
                 print_wrid(wr_id), wr_id, rdma->nb_sent, lc->nb_sent,
                 block_idx, chunk, block->local_host_addr, 
                 (void *)block->remote_host_addr);

        if (!rdma->pin_all) {
            /*
             * FYI: If one wanted to signal a specific chunk to be unregistered
             * using LRU or workload-specific information, this is the function
             * you would call to do so. That chunk would then get asynchronously
             * unregistered later.
             */
#ifdef RDMA_UNREGISTRATION_EXAMPLE
             if (block->pmr[chunk]) { 
                 qemu_rdma_signal_unregister(rdma, block_idx, chunk, wc.wr_id);
             }
#endif
        }
    } else {
        DDDPRINTF("other completion %s (%" 
                  PRId64 ") received left %d (per qp %d)\n",
            print_wrid(wr_id), wr_id, rdma->nb_sent, lc->nb_sent);
    }

    *wr_id_out = wc.wr_id;
    if (byte_len) {
        *byte_len = wc.byte_len;
    }

    return  0;
}

/*
 * Block until the next work request has completed.
 *
 * First poll to see if a work request has already completed,
 * otherwise block.
 *
 * If we encounter completed work requests for IDs other than
 * the one we're interested in, then that's generally an error.
 *
 * The only exception is actual RDMA Write completions. These
 * completions only need to be recorded, but do not actually
 * need further processing.
 */
static int qemu_rdma_block_for_wrid(RDMAContext *rdma, 
                                    RDMALocalContext *lc,
                                    int wrid_requested,
                                    uint32_t *byte_len)
{
    int num_cq_events = 0, ret = 0;
    struct ibv_cq *cq;
    void *cq_ctx;
    uint64_t wr_id = RDMA_WRID_NONE, wr_id_in;

    ret = ibv_req_notify_cq(lc->cq, 0);
    if (ret) {
        perror("ibv_req_notify_cq");
        return -ret;
    }

    /* poll cq first */
    while (wr_id != wrid_requested) {
        ret = qemu_rdma_poll(rdma, lc, &wr_id_in, byte_len);
        if (ret < 0) {
            return ret;
        }

        wr_id = wr_id_in & RDMA_WRID_TYPE_MASK;

        if (wr_id == RDMA_WRID_NONE) {
            break;
        }
        if (wr_id != wrid_requested) {
            DDDPRINTF("A Wanted wrid %s (%d) but got %s (%" PRIu64 ")\n",
                print_wrid(wrid_requested),
                wrid_requested, print_wrid(wr_id), wr_id);
        }
    }

    if (wr_id == wrid_requested) {
        return 0;
    }

    while (1) {
        /*
         * Coroutine doesn't start until process_incoming_migration()
         * so don't yield unless we know we're running inside of a coroutine.
         */
        if (qemu_in_coroutine()) {
            yield_until_fd_readable(lc->comp_chan->fd);
        }

        ret = ibv_get_cq_event(lc->comp_chan, &cq, &cq_ctx);
        if (ret < 0) {
            perror("ibv_get_cq_event");
            goto err_block_for_wrid;
        }

        num_cq_events++;

        ret = ibv_req_notify_cq(cq, 0);
        if (ret) {
            ret = -ret;
            perror("ibv_req_notify_cq");
            goto err_block_for_wrid;
        }

        while (wr_id != wrid_requested) {
            ret = qemu_rdma_poll(rdma, lc, &wr_id_in, byte_len);
            if (ret < 0) {
                goto err_block_for_wrid;
            }

            wr_id = wr_id_in & RDMA_WRID_TYPE_MASK;

            if (wr_id == RDMA_WRID_NONE) {
                break;
            }
            if (wr_id != wrid_requested) {
                DDDPRINTF("B Wanted wrid %s (%d) but got %s (%" PRIu64 ")\n",
                    print_wrid(wrid_requested), wrid_requested,
                    print_wrid(wr_id), wr_id);
            }
        }

        if (wr_id == wrid_requested) {
            goto success_block_for_wrid;
        }
    }

success_block_for_wrid:
    if (num_cq_events) {
        ibv_ack_cq_events(cq, num_cq_events);
    }
    return 0;

err_block_for_wrid:
    if (num_cq_events) {
        ibv_ack_cq_events(cq, num_cq_events);
    }
    return ret;
}

/*
 * Post a SEND message work request for the control channel
 * containing some data and block until the post completes.
 */
static int qemu_rdma_post_send_control(RDMAContext *rdma, uint8_t *buf,
                                       RDMAControlHeader *head)
{
    int ret = 0;
    RDMAWorkRequestData *wr = &rdma->wr_data[RDMA_WRID_CONTROL];
    struct ibv_send_wr *bad_wr;
    struct ibv_sge sge = {
                           .addr = (uint64_t)(wr->control),
                           .length = head->len + sizeof(RDMAControlHeader),
                           .lkey = wr->control_mr->lkey,
                         };
    struct ibv_send_wr send_wr = {
                                   .wr_id = RDMA_WRID_SEND_CONTROL,
                                   .opcode = IBV_WR_SEND,
                                   .send_flags = IBV_SEND_SIGNALED,
                                   .sg_list = &sge,
                                   .num_sge = 1,
                                };

    DDDPRINTF("CONTROL: sending %s..\n", control_desc[head->type]);

    /*
     * We don't actually need to do a memcpy() in here if we used
     * the "sge" properly, but since we're only sending control messages
     * (not RAM in a performance-critical path), then its OK for now.
     *
     * The copy makes the RDMAControlHeader simpler to manipulate
     * for the time being.
     */
    assert(head->len <= RDMA_CONTROL_MAX_BUFFER - sizeof(*head));
    memcpy(wr->control, head, sizeof(RDMAControlHeader));
    control_to_network((void *) wr->control);

    if (buf) {
        memcpy(wr->control + sizeof(RDMAControlHeader), buf, head->len);
    }


    if (ibv_post_send(rdma->lc_remote.qp, &send_wr, &bad_wr)) {
        return -1;
    }

    if (ret < 0) {
        ERROR(NULL, "using post IB SEND for control!");
        return ret;
    }

    ret = qemu_rdma_block_for_wrid(rdma, &rdma->lc_remote,
                                   RDMA_WRID_SEND_CONTROL, NULL);
    if (ret < 0) {
        ERROR(NULL, "send polling control!");
    }

    return ret;
}

/*
 * Post a RECV work request in anticipation of some future receipt
 * of data on the control channel.
 */
static int qemu_rdma_post_recv_control(RDMAContext *rdma, int idx)
{
    struct ibv_recv_wr *bad_wr;
    struct ibv_sge sge = {
                            .addr = (uint64_t)(rdma->wr_data[idx].control),
                            .length = RDMA_CONTROL_MAX_BUFFER,
                            .lkey = rdma->wr_data[idx].control_mr->lkey,
                         };

    struct ibv_recv_wr recv_wr = {
                                    .wr_id = RDMA_WRID_RECV_CONTROL + idx,
                                    .sg_list = &sge,
                                    .num_sge = 1,
                                 };


    if (ibv_post_recv(rdma->lc_remote.qp, &recv_wr, &bad_wr)) {
        return -1;
    }

    return 0;
}

/*
 * Block and wait for a RECV control channel message to arrive.
 */
static int qemu_rdma_exchange_get_response(RDMAContext *rdma,
                RDMAControlHeader *head, int expecting, int idx)
{
    uint32_t byte_len;
    int ret = qemu_rdma_block_for_wrid(rdma, &rdma->lc_remote,
                                       RDMA_WRID_RECV_CONTROL + idx,
                                       &byte_len);

    if (ret < 0) {
        ERROR(NULL, "recv polling control!");
        return ret;
    }

    network_to_control((void *) rdma->wr_data[idx].control);
    memcpy(head, rdma->wr_data[idx].control, sizeof(RDMAControlHeader));

    DDDPRINTF("CONTROL: %s receiving...\n", control_desc[expecting]);

    if (expecting == RDMA_CONTROL_NONE) {
        DDDPRINTF("Surprise: got %s (%d)\n",
                  control_desc[head->type], head->type);
    } else if (head->type != expecting || head->type == RDMA_CONTROL_ERROR) {
        fprintf(stderr, "Was expecting a %s (%d) control message"
                ", but got: %s (%d), length: %d\n",
                control_desc[expecting], expecting,
                control_desc[head->type], head->type, head->len);
        return -EIO;
    }
    if (head->len > RDMA_CONTROL_MAX_BUFFER - sizeof(*head)) {
        fprintf(stderr, "too long length: %d\n", head->len);
        return -EINVAL;
    }
    if (sizeof(*head) + head->len != byte_len) {
        fprintf(stderr, "Malformed length: %d byte_len %d\n",
                head->len, byte_len);
        return -EINVAL;
    }

    return 0;
}

/*
 * When a RECV work request has completed, the work request's
 * buffer is pointed at the header.
 *
 * This will advance the pointer to the data portion
 * of the control message of the work request's buffer that
 * was populated after the work request finished.
 */
static void qemu_rdma_move_header(RDMAContext *rdma, int idx,
                                  RDMAControlHeader *head)
{
    rdma->wr_data[idx].control_len = head->len;
    rdma->wr_data[idx].control_curr =
        rdma->wr_data[idx].control + sizeof(RDMAControlHeader);
}

/*
 * This is an 'atomic' high-level operation to deliver a single, unified
 * control-channel message.
 *
 * Additionally, if the user is expecting some kind of reply to this message,
 * they can request a 'resp' response message be filled in by posting an
 * additional work request on behalf of the user and waiting for an additional
 * completion.
 *
 * The extra (optional) response is used during registration to us from having
 * to perform an *additional* exchange of message just to provide a response by
 * instead piggy-backing on the acknowledgement.
 */
static int qemu_rdma_exchange_send(RDMAContext *rdma, RDMAControlHeader *head,
                                   uint8_t *data, RDMAControlHeader *resp,
                                   int *resp_idx,
                                   int (*callback)(RDMAContext *rdma))
{
    int ret = 0;

    /*
     * Wait until the dest is ready before attempting to deliver the message
     * by waiting for a READY message.
     */
    if (rdma->control_ready_expected) {
        RDMAControlHeader resp;
        ret = qemu_rdma_exchange_get_response(rdma,
                                    &resp, RDMA_CONTROL_READY, RDMA_WRID_READY);
        if (ret < 0) {
            return ret;
        }
    }

    /*
     * If the user is expecting a response, post a WR in anticipation of it.
     */
    if (resp) {
        ret = qemu_rdma_post_recv_control(rdma, RDMA_WRID_DATA);
        if (ret) {
            ERROR(NULL, "posting extra control recv for anticipated result!");
            return ret;
        }
    }

    /*
     * Post a WR to replace the one we just consumed for the READY message.
     */
    ret = qemu_rdma_post_recv_control(rdma, RDMA_WRID_READY);
    if (ret) {
        ERROR(NULL, "posting first control recv!");
        return ret;
    }

    /*
     * Deliver the control message that was requested.
     */
    ret = qemu_rdma_post_send_control(rdma, data, head);

    if (ret < 0) {
        ERROR(NULL, "sending control buffer!");
        return ret;
    }

    /*
     * If we're expecting a response, block and wait for it.
     */
    if (resp) {
        if (callback) {
            DDPRINTF("Issuing callback before receiving response...\n");
            ret = callback(rdma);
            if (ret < 0) {
                return ret;
            }
        }

        DDPRINTF("Waiting for response %s\n", control_desc[resp->type]);
        ret = qemu_rdma_exchange_get_response(rdma, resp,
                                              resp->type, RDMA_WRID_DATA);

        if (ret < 0) {
            return ret;
        }

        qemu_rdma_move_header(rdma, RDMA_WRID_DATA, resp);
        if (resp_idx) {
            *resp_idx = RDMA_WRID_DATA;
        }
        DDPRINTF("Response %s received.\n", control_desc[resp->type]);
    }

    rdma->control_ready_expected = 1;

    return 0;
}

/*
 * This is an 'atomic' high-level operation to receive a single, unified
 * control-channel message.
 */
static int qemu_rdma_exchange_recv(RDMAContext *rdma, RDMAControlHeader *head,
                                int expecting)
{
    RDMAControlHeader ready = {
                                .len = 0,
                                .type = RDMA_CONTROL_READY,
                                .repeat = 1,
                              };
    int ret;

    /*
     * Inform the source that we're ready to receive a message.
     */
    ret = qemu_rdma_post_send_control(rdma, NULL, &ready);

    if (ret < 0) {
        fprintf(stderr, "Failed to send control buffer!\n");
        return ret;
    }

    /*
     * Block and wait for the message.
     */
    ret = qemu_rdma_exchange_get_response(rdma, head,
                                          expecting, RDMA_WRID_READY);

    if (ret < 0) {
        return ret;
    }

    qemu_rdma_move_header(rdma, RDMA_WRID_READY, head);

    /*
     * Post a new RECV work request to replace the one we just consumed.
     */
    ret = qemu_rdma_post_recv_control(rdma, RDMA_WRID_READY);
    if (ret) {
        ERROR(NULL, "posting second control recv!");
        return ret;
    }

    return 0;
}

static inline void install_boundaries(RDMAContext *rdma, RDMACurrentChunk *cc)
{
    uint64_t len = cc->block->is_ram_block ? 
                   cc->current_length : cc->block->length;

    cc->chunks = len / (1UL << RDMA_REG_CHUNK_SHIFT);

    if (cc->chunks && ((len % (1UL << RDMA_REG_CHUNK_SHIFT)) == 0)) {
        cc->chunks--;
    }

    cc->addr = (uint8_t *) (uint64_t)(cc->block->local_host_addr +
                                 (cc->current_addr - cc->block->offset));

    cc->chunk_idx = ram_chunk_index(cc->block->local_host_addr, cc->addr);
    cc->chunk_start = ram_chunk_start(cc->block, cc->chunk_idx);
    cc->chunk_end = ram_chunk_end(cc->block, cc->chunk_idx + cc->chunks);

    DDPRINTF("Block %d chunk %" PRIu64 " has %" PRIu64
             " chunks, (%" PRIu64 " MB)\n", cc->block->index, cc->chunk_idx,
                cc->chunks + 1, (cc->chunks + 1) * 
                    (1UL << RDMA_REG_CHUNK_SHIFT) / 1024 / 1024);

}

/*
 * Push out any unwritten RDMA operations.
 */
static int qemu_rdma_write(QEMUFile *f, RDMAContext *rdma,
                                 RDMACurrentChunk *src,
                                 RDMACurrentChunk *dest)
{
    struct ibv_sge sge;
    struct ibv_send_wr send_wr = { 0 };
    struct ibv_send_wr *bad_wr;
    int reg_result_idx, ret, count = 0;
    bool copy;
    RDMALocalContext *lc;
    RDMARegister reg;
    RDMARegisterResult *reg_result;
    RDMAControlHeader resp = { .type = RDMA_CONTROL_REGISTER_RESULT };
    RDMAControlHeader head = { .len = sizeof(RDMARegister),
                               .type = RDMA_CONTROL_REGISTER_REQUEST,
                               .repeat = 1,
                             };

    if (!src->current_length) {
        return 0;
    }

    if (dest == src) {
        dest = NULL;
    }

    copy = dest ? true : false;

    lc = copy ? 
        (rdma->source ? &rdma->lc_src : &rdma->lc_dest) : &rdma->lc_remote;

retry:
    src->block = &(rdma->local_ram_blocks.block[src->current_block_idx]);
    install_boundaries(rdma, src);

    if (dest) {
        dest->block = &(rdma->local_ram_blocks.block[dest->current_block_idx]);
        install_boundaries(rdma, dest);
    }

    if (!rdma->pin_all) {
#ifdef RDMA_UNREGISTRATION_EXAMPLE
        qemu_rdma_unregister_waiting(rdma);
#endif
    }

    while (test_bit(src->chunk_idx, src->block->transit_bitmap)) {
        (void)count;
        DDPRINTF("(%d) Not clobbering: block: %d chunk %" PRIu64
                " current %" PRIu64 " len %" PRIu64 " left %d (per qp %d) %d\n",
                count++, src->current_block_idx, src->chunk_idx,
                (uint64_t) src->addr, src->current_length, 
                rdma->nb_sent, lc->nb_sent, src->block->nb_chunks);

        ret = qemu_rdma_block_for_wrid(rdma, lc, 
                                       RDMA_WRID_RDMA_WRITE_REMOTE, NULL);

        if (ret < 0) {
            fprintf(stderr, "Failed to Wait for previous write to complete "
                    "block %d chunk %" PRIu64
                    " current %" PRIu64 " len %" PRIu64 " %d (per qp %d)\n",
                    src->current_block_idx, src->chunk_idx, (uint64_t) src->addr, 
                    src->current_length, rdma->nb_sent, lc->nb_sent);
            return ret;
        }
    }

    if (!rdma->pin_all || !src->block->is_ram_block) {
        if (!src->block->remote_keys[src->chunk_idx]) {
            /*
             * This chunk has not yet been registered, so first check to see
             * if the entire chunk is zero. If so, tell the other size to
             * memset() + madvise() the entire chunk without RDMA.
             */

            if (src->block->is_ram_block &&
                   can_use_buffer_find_nonzero_offset(src->addr, src->current_length)
                   && buffer_find_nonzero_offset(src->addr,
                                                    src->current_length) == src->current_length) {
                RDMACompress comp = {
                                        .offset = src->current_addr,
                                        .value = 0,
                                        .block_idx = src->current_block_idx,
                                        .length = src->current_length,
                                    };

                head.len = sizeof(comp);
                head.type = RDMA_CONTROL_COMPRESS;

                DDPRINTF("Entire chunk is zero, sending compress: %" PRIu64 
                         " for %" PRIu64 " bytes, index: %d"
                         ", offset: %" PRId64 "...\n",
                         src->chunk_idx, src->current_length, 
                         src->current_block_idx, src->current_addr);

                compress_to_network(&comp);
                ret = qemu_rdma_exchange_send(rdma, &head,
                                (uint8_t *) &comp, NULL, NULL, NULL);

                if (ret < 0) {
                    return -EIO;
                }

                acct_update_position(f, src->current_length, true);

                return 1;
            }

            /*
             * Otherwise, tell other side to register. (Only for remote RDMA)
             */
            if (!dest) {
                reg.current_block_idx = src->current_block_idx;
                if (src->block->is_ram_block) {
                    reg.key.current_addr = src->current_addr;
                } else {
                    reg.key.chunk = src->chunk_idx;
                }
                reg.chunks = src->chunks;

                DDPRINTF("Sending registration request chunk %" PRIu64 
                         " for %" PRIu64 " bytes, index: %d, offset: %" 
                         PRId64 "...\n",
                         src->chunk_idx, src->current_length, 
                         src->current_block_idx, src->current_addr);

                register_to_network(&reg);
                ret = qemu_rdma_exchange_send(rdma, &head, (uint8_t *) &reg,
                                        &resp, &reg_result_idx, NULL);
                if (ret < 0) {
                    return ret;
                }
            }

            /* try to overlap this single registration with the one we sent. */
            if (qemu_rdma_register_and_get_keys(rdma, src, lc, copy, 
                                                &sge.lkey, NULL)) {
                fprintf(stderr, "cannot get lkey!\n");
                return -EINVAL;
            }

            if (!dest) {
                reg_result = (RDMARegisterResult *)
                        rdma->wr_data[reg_result_idx].control_curr;

                network_to_result(reg_result);

                DDPRINTF("Received registration result:"
                        " my key: %x their key %x, chunk %" PRIu64 "\n",
                        src->block->remote_keys[src->chunk_idx], 
                        reg_result->rkey, src->chunk_idx);

                src->block->remote_keys[src->chunk_idx] = reg_result->rkey;
                src->block->remote_host_addr = reg_result->host_addr;
            }
        } else {
            /* already registered before */
            if (qemu_rdma_register_and_get_keys(rdma, src, lc, copy,
                                                &sge.lkey, NULL)) {
                fprintf(stderr, "cannot get lkey!\n");
                return -EINVAL;
            }
        }

        send_wr.wr.rdma.rkey = src->block->remote_keys[src->chunk_idx];
    } else {
        send_wr.wr.rdma.rkey = src->block->remote_rkey;

        if (qemu_rdma_register_and_get_keys(rdma, src, lc, copy, 
                                            &sge.lkey, NULL)) {
            fprintf(stderr, "cannot get lkey!\n");
            return -EINVAL;
        }
    }

    if (dest) {
        if (qemu_rdma_register_and_get_keys(rdma, dest,
                                            &rdma->lc_dest, copy,
                                            NULL, &send_wr.wr.rdma.rkey)) {
            fprintf(stderr, "cannot get rkey!\n");
            return -EINVAL;
        }
    }

    /*
     * Encode the ram block index and chunk within this wrid.
     * We will use this information at the time of completion
     * to figure out which bitmap to check against and then which
     * chunk in the bitmap to look for.
     */
    send_wr.wr_id = qemu_rdma_make_wrid(RDMA_WRID_RDMA_WRITE_REMOTE,
                                        src->current_block_idx, src->chunk_idx);

    sge.length = src->current_length;
    sge.addr = (uint64_t) src->addr;
    send_wr.opcode = IBV_WR_RDMA_WRITE;
    send_wr.send_flags = IBV_SEND_SIGNALED;
    send_wr.sg_list = &sge;
    send_wr.num_sge = 1;
    send_wr.wr.rdma.remote_addr = (dest ? (uint64_t) dest->addr : 
                (src->block->remote_host_addr + 
                    (src->current_addr - src->block->offset)));

    DDPRINTF("Posting chunk: %" PRIu64 ", addr: %lx"
             " remote: %lx, bytes %" PRIu32 " lkey %" PRIu32 
             " rkey %" PRIu32 "\n",
             src->chunk_idx, sge.addr, 
             send_wr.wr.rdma.remote_addr, sge.length,
             sge.lkey, send_wr.wr.rdma.rkey);

    /*
     * ibv_post_send() does not return negative error numbers,
     * per the specification they are positive - no idea why.
     */
    ret = ibv_post_send(lc->qp, &send_wr, &bad_wr);

    if (ret == ENOMEM) {
        DDPRINTF("send queue is full. wait a little....\n");
        ret = qemu_rdma_block_for_wrid(rdma, lc,
                                       RDMA_WRID_RDMA_WRITE_REMOTE, NULL);
        if (ret < 0) {
            ERROR(NULL, "could not make room in full send queue! %d", ret);
            return ret;
        }

        goto retry;

    } else if (ret > 0) {
        perror("rdma migration: post rdma write failed");
        return -ret;
    }

    set_bit(src->chunk_idx, src->block->transit_bitmap);

    if (!dest) {
        acct_update_position(f, sge.length, false);
    }

    rdma->total_writes++;
    rdma->nb_sent++;
    lc->nb_sent++;

    DDDPRINTF("sent total: %d sent lc: %d\n", rdma->nb_sent, lc->nb_sent);

    src->current_length = 0;
    src->current_addr = 0;

    if (dest) {
        dest->current_length = 0;
        dest->current_addr = 0;
    }

    return 0;
}

static inline int qemu_rdma_buffer_mergable(RDMAContext *rdma,
                                            RDMACurrentChunk *cc,
                                            uint64_t current_addr, 
                                            uint64_t len)
{
    RDMALocalBlock *block;
    uint8_t *host_addr;
    uint8_t *chunk_end;

    if (cc->current_block_idx < 0) {
        return 0;
    }

    if (cc->current_chunk < 0) {
        return 0;
    }

    block = &(rdma->local_ram_blocks.block[cc->current_block_idx]);
    host_addr = block->local_host_addr + (current_addr - block->offset);
    chunk_end = ram_chunk_end(block, cc->current_chunk);

    if (cc->current_length == 0) {
        return 0;
    }

    /*
     * Only merge into chunk sequentially.
     */
    if (current_addr != (cc->current_addr + cc->current_length)) {
        return 0;
    }

    if (current_addr < block->offset) {
        return 0;
    }

    if ((current_addr + len) > (block->offset + block->length)) {
        return 0;
    }

    if ((host_addr + len) > chunk_end) {
        return 0;
    }

    return 1;
}

static int write_start(RDMAContext *rdma,
                        RDMACurrentChunk *cc,
                        uint64_t len,
                        uint64_t current_addr)
{
    int ret;
    uint64_t block_idx, chunk;

    cc->current_addr = current_addr;
    block_idx = cc->current_block_idx;
    chunk = cc->current_chunk;

    ret = qemu_rdma_search_ram_block(rdma, cc->block_offset,
                                     cc->offset, len, &block_idx, &chunk);
    if (ret) {
        ERROR(NULL, "ram block search failed");
        return ret;
    }

    cc->current_block_idx = block_idx;
    cc->current_chunk = chunk;

    return 0;
}

/* 
 * If we cannot merge it, we flush the current buffer first.
 */
static int qemu_rdma_flush_unmergable(RDMAContext *rdma,
                                      RDMACurrentChunk *src,
                                      RDMACurrentChunk *dest,
                                      QEMUFile *f, uint64_t len)
{
    uint64_t current_addr_src;
    uint64_t current_addr_dest;
    int ret;

    current_addr_src = src->block_offset + src->offset;

    if (dest) {
        current_addr_dest = dest->block_offset + dest->offset;
    }

    if (qemu_rdma_buffer_mergable(rdma, src, current_addr_src, len)) {
        if (dest) {
            if (qemu_rdma_buffer_mergable(rdma, dest, current_addr_dest, len)) {
                goto merge;
            }
        } else {
            goto merge;
        }
    }

    ret = qemu_rdma_write(f, rdma, src, dest);

    if (ret) {
        return ret;
    }

    ret = write_start(rdma, src, len, current_addr_src);

    if (ret) {
        return ret;
    }

    if (dest) {
        ret = write_start(rdma, dest, len, current_addr_dest);

        if (ret) {
            return ret;
        }
    }

merge:
    src->current_length += len;
    if (dest) {
        dest->current_length += len;
    }

    return 0;
}

static void qemu_rdma_cleanup(RDMAContext *rdma, bool force)
{
    struct rdma_cm_event *cm_event;
    int ret, idx;

    if (connection_timer) {
        timer_del(connection_timer);
        timer_free(connection_timer);
        connection_timer = NULL;
    }

    if (keepalive_timer) {
        timer_del(keepalive_timer);
        timer_free(keepalive_timer);
        keepalive_timer = NULL;
    }

    if (rdma->cm_id && rdma->connected) {
        if (rdma->error_state) {
            if (rdma->error_state != -ENETUNREACH) {
                RDMAControlHeader head = { .len = 0,
                                           .type = RDMA_CONTROL_ERROR,
                                           .repeat = 1,
                                         };
                fprintf(stderr, "Early error. Sending error.\n");
                qemu_rdma_post_send_control(rdma, NULL, &head);
            } else {
                fprintf(stderr, "Early error.\n");
            }
        }

        ret = rdma_disconnect(rdma->cm_id);
        if (!ret && !force && (rdma->error_state != -ENETUNREACH)) {
            DDPRINTF("waiting for disconnect\n");
            ret = rdma_get_cm_event(rdma->channel, &cm_event);
            if (!ret) {
                rdma_ack_cm_event(cm_event);
            }
        }
        DDPRINTF("Disconnected.\n");
        rdma->lc_remote.verbs = NULL;
        rdma->connected = false;
    }

    g_free(rdma->block);
    rdma->block = NULL;

    for (idx = 0; idx < RDMA_WRID_MAX; idx++) {
        if (rdma->wr_data[idx].control_mr) {
            rdma->total_registrations--;
            ibv_dereg_mr(rdma->wr_data[idx].control_mr);
        }
        rdma->wr_data[idx].control_mr = NULL;
    }

    if (rdma->local_ram_blocks.block) {
        while (rdma->local_ram_blocks.nb_blocks) {
            __qemu_rdma_delete_block(rdma,
                    rdma->local_ram_blocks.block->offset);
        }
    }

    close_ibv(rdma, &rdma->lc_remote);
    close_ibv(rdma, &rdma->lc_src);
    close_ibv(rdma, &rdma->lc_dest);

    if (rdma->listen_id) {
        rdma_destroy_id(rdma->listen_id);
        rdma->listen_id = NULL;
    }
    if (rdma->cm_id) {
        rdma_destroy_id(rdma->cm_id);
        rdma->cm_id = NULL;
    }
    if (rdma->channel) {
        rdma_destroy_event_channel(rdma->channel);
        rdma->channel = NULL;
    }

    g_free(rdma->host);
    rdma->host = NULL;

    if (rdma->keepalive_mr) {
        ibv_dereg_mr(rdma->keepalive_mr);
        rdma->keepalive_mr = NULL;
    }
    if (rdma->next_keepalive_mr) {
        ibv_dereg_mr(rdma->next_keepalive_mr);
        rdma->next_keepalive_mr = NULL;
    }
}


static int qemu_rdma_source_init(RDMAContext *rdma,
                                 Error **errp,
                                 MigrationState *s)
{
    int ret, idx;
    Error *local_err = NULL, **temp = &local_err;

    /*
     * Will be validated against destination's actual capabilities
     * after the connect() completes.
     */
    rdma->pin_all = s->enabled_capabilities[MIGRATION_CAPABILITY_X_RDMA_PIN_ALL];
    rdma->do_keepalive = s->enabled_capabilities[MIGRATION_CAPABILITY_RDMA_KEEPALIVE];

    ret = qemu_rdma_resolve_host(rdma, temp);
    if (ret) {
        goto err_rdma_source_init;
    }

    ret = qemu_rdma_alloc_pd_cq(rdma, &rdma->lc_remote);
    if (ret) {
        ERROR(temp, "allocating pd and cq! Your mlock()"
                    " limits may be too low. Please check $ ulimit -a # and "
                    "search for 'ulimit -l' in the output");
        goto err_rdma_source_init;
    }

    ret = qemu_rdma_alloc_keepalive(rdma);

    if (ret) {
        ERROR(temp, "allocating keepalive structures");
        goto err_rdma_source_init;
    }

    ret = qemu_rdma_alloc_qp(rdma);
    if (ret) {
        ERROR(temp, "allocating qp!");
        goto err_rdma_source_init;
    }

    ret = qemu_rdma_init_ram_blocks(rdma);
    if (ret) {
        ERROR(temp, "initializing ram blocks!");
        goto err_rdma_source_init;
    }

    for (idx = 0; idx < RDMA_WRID_MAX; idx++) {
        ret = qemu_rdma_reg_control(rdma, idx);
        if (ret) {
            ERROR(temp, "registering %d control!", idx);
            goto err_rdma_source_init;
        }
    }

    return 0;

err_rdma_source_init:
    error_propagate(errp, local_err);
    qemu_rdma_cleanup(rdma, false);
    return -1;
}

static int qemu_rdma_connect(RDMAContext *rdma, Error **errp)
{
    RDMACapabilities cap = {
                                .version = RDMA_CONTROL_VERSION_CURRENT,
                                .flags = 0,
                                .keepalive_rkey = rdma->keepalive_mr->rkey,
                                .keepalive_addr = (uint64_t) &rdma->keepalive,
                           };
    struct rdma_conn_param conn_param = { .initiator_depth = 2,
                                          .retry_count = 5,
                                          .private_data = &cap,
                                          .private_data_len = sizeof(cap),
                                        };
    struct rdma_cm_event *cm_event;
    int ret;

    /*
     * Only negotiate the capability with destination if the user
     * on the source first requested the capability.
     */
    if (rdma->pin_all) {
        DPRINTF("Server pin-all memory requested.\n");
        cap.flags |= RDMA_CAPABILITY_PIN_ALL;
    }

    if (rdma->do_keepalive) {
        DPRINTF("Keepalives requested.\n");
        cap.flags |= RDMA_CAPABILITY_KEEPALIVE;
    }

    DDPRINTF("Sending keepalive params: key %x addr: %" PRIx64 "\n",
            cap.keepalive_rkey, cap.keepalive_addr);
    caps_to_network(&cap);

    ret = rdma_connect(rdma->cm_id, &conn_param);
    if (ret) {
        perror("rdma_connect");
        ERROR(errp, "connecting to destination!");
        rdma_destroy_id(rdma->cm_id);
        rdma->cm_id = NULL;
        goto err_rdma_source_connect;
    }

    ret = rdma_get_cm_event(rdma->channel, &cm_event);
    if (ret) {
        perror("rdma_get_cm_event after rdma_connect");
        ERROR(errp, "connecting to destination!");
        rdma_ack_cm_event(cm_event);
        rdma_destroy_id(rdma->cm_id);
        rdma->cm_id = NULL;
        goto err_rdma_source_connect;
    }

    if (cm_event->event != RDMA_CM_EVENT_ESTABLISHED) {
        perror("rdma_get_cm_event != EVENT_ESTABLISHED after rdma_connect");
        ERROR(errp, "connecting to destination!");
        rdma_ack_cm_event(cm_event);
        rdma_destroy_id(rdma->cm_id);
        rdma->cm_id = NULL;
        goto err_rdma_source_connect;
    }
    rdma->connected = true;

    memcpy(&cap, cm_event->param.conn.private_data, sizeof(cap));
    network_to_caps(&cap);

    rdma->keepalive_rkey = cap.keepalive_rkey;
    rdma->keepalive_addr = cap.keepalive_addr;

    DDPRINTF("Received keepalive params: key %x addr: %" PRIx64 "\n",
            cap.keepalive_rkey, cap.keepalive_addr);

    /*
     * Verify that the *requested* capabilities are supported by the destination
     * and disable them otherwise.
     */
    if (rdma->pin_all && !(cap.flags & RDMA_CAPABILITY_PIN_ALL)) {
        ERROR(errp, "Server cannot support pinning all memory. "
                        "Will register memory dynamically.");
        rdma->pin_all = false;
    }

    if (rdma->do_keepalive && !(cap.flags & RDMA_CAPABILITY_KEEPALIVE)) {
        ERROR(errp, "Server cannot support keepalives. "
                        "Will not check for them.");
        rdma->do_keepalive = false;
    }

    DPRINTF("Pin all memory: %s\n", rdma->pin_all ? "enabled" : "disabled");
    DPRINTF("Keepalives: %s\n", rdma->do_keepalive ? "enabled" : "disabled");

    rdma_ack_cm_event(cm_event);

    ret = qemu_rdma_post_recv_control(rdma, RDMA_WRID_READY);
    if (ret) {
        ERROR(errp, "posting second control recv!");
        goto err_rdma_source_connect;
    }

    rdma->control_ready_expected = 1;
    rdma->nb_sent = 0;
    return 0;

err_rdma_source_connect:
    qemu_rdma_cleanup(rdma, false);
    return -1;
}

static int qemu_rdma_dest_init(RDMAContext *rdma, Error **errp)
{
    int ret = -EINVAL, idx;
    struct rdma_cm_id *listen_id;
    char ip[40] = "unknown";
    struct rdma_addrinfo *res;
    char port_str[16];

    for (idx = 0; idx < RDMA_WRID_MAX; idx++) {
        rdma->wr_data[idx].control_len = 0;
        rdma->wr_data[idx].control_curr = NULL;
    }

    if (rdma->host == NULL) {
        ERROR(errp, "RDMA host is not set!");
        SET_ERROR(rdma, -EINVAL);
        return -1;
    }
    /* create CM channel */
    rdma->channel = rdma_create_event_channel();
    if (!rdma->channel) {
        ERROR(errp, "could not create rdma event channel");
        SET_ERROR(rdma, -EINVAL);
        return -1;
    }

    /* create CM id */
    ret = rdma_create_id(rdma->channel, &listen_id, NULL, RDMA_PS_TCP);
    if (ret) {
        ERROR(errp, "could not create cm_id!");
        goto err_dest_init_create_listen_id;
    }

    snprintf(port_str, 16, "%d", rdma->port);
    port_str[15] = '\0';

    if (rdma->host && strcmp("", rdma->host)) {
        struct rdma_addrinfo *e;

        ret = rdma_getaddrinfo(rdma->host, port_str, NULL, &res);
        if (ret < 0) {
            ERROR(errp, "could not rdma_getaddrinfo address %s", rdma->host);
            goto err_dest_init_bind_addr;
        }

        for (e = res; e != NULL; e = e->ai_next) {
            inet_ntop(e->ai_family,
                &((struct sockaddr_in *) e->ai_dst_addr)->sin_addr, ip, sizeof ip);
            DPRINTF("Trying %s => %s\n", rdma->host, ip);
            ret = rdma_bind_addr(listen_id, e->ai_dst_addr);
            if (!ret) {
                if (e->ai_family == AF_INET6) {
                    ret = qemu_rdma_broken_ipv6_kernel(errp, listen_id->verbs);
                    if (ret) {
                        continue;
                    }
                }
                    
                goto listen;
            }
        }

        ERROR(errp, "Error: could not rdma_bind_addr!");
        goto err_dest_init_bind_addr;
    } else {
        ERROR(errp, "migration host and port not specified!");
        ret = -EINVAL;
        goto err_dest_init_bind_addr;
    }
listen:

    rdma->listen_id = listen_id;
    qemu_rdma_dump_gid("dest_init", listen_id);
    return 0;

err_dest_init_bind_addr:
    rdma_destroy_id(listen_id);
err_dest_init_create_listen_id:
    rdma_destroy_event_channel(rdma->channel);
    rdma->channel = NULL;
    SET_ERROR(rdma, ret);
    return ret;

}

static void send_keepalive(void *opaque)
{
    RDMAContext *rdma = opaque;
    struct ibv_sge sge;
    struct ibv_send_wr send_wr = { 0 };
    struct ibv_send_wr *bad_wr;
    int ret;

    if (!rdma->migration_started) {
        goto reset;
    }

    rdma->next_keepalive++;
retry:

    sge.addr = (uint64_t) &rdma->next_keepalive;
    sge.length = sizeof(rdma->next_keepalive);
    sge.lkey = rdma->next_keepalive_mr->lkey;
    send_wr.wr_id = RDMA_WRID_RDMA_KEEPALIVE;
    send_wr.opcode = IBV_WR_RDMA_WRITE;
    send_wr.send_flags = 0;
    send_wr.sg_list = &sge;
    send_wr.num_sge = 1;
    send_wr.wr.rdma.remote_addr = rdma->keepalive_addr;
    send_wr.wr.rdma.rkey = rdma->keepalive_rkey;

    DDPRINTF("Posting keepalive: addr: %lx"
              " remote: %lx, bytes %" PRIu32 "\n",
              sge.addr, send_wr.wr.rdma.remote_addr, sge.length);

    ret = ibv_post_send(rdma->lc_remote.qp, &send_wr, &bad_wr);

    if (ret == ENOMEM) {
        DPRINTF("send queue is full. wait a little....\n");
        g_usleep(RDMA_KEEPALIVE_INTERVAL_MS * 1000);
        goto retry;
    } else if (ret > 0) {
        perror("rdma migration: post keepalive");
        SET_ERROR(rdma, -ret);
        return;
    }

reset:
    timer_mod(keepalive_timer, qemu_clock_get_ms(QEMU_CLOCK_REALTIME) +
                    RDMA_KEEPALIVE_INTERVAL_MS);
}

static void check_qp_state(void *opaque)
{
    RDMAContext *rdma = opaque;
    int first_missed = 0;

    if (!rdma->migration_started) {
        goto reset;
    }

    if (rdma->last_keepalive == rdma->keepalive) {
        rdma->nb_missed_keepalive++;
        if (rdma->nb_missed_keepalive == 1) {
            first_missed = RDMA_KEEPALIVE_FIRST_MISSED_OFFSET;
            DDPRINTF("Setting first missed additional delay\n");
        } else {
            DPRINTF("WARN: missed keepalive: %" PRIu64 "\n",
                        rdma->nb_missed_keepalive);
        }
    } else {
        rdma->keepalive_startup = true;
        rdma->nb_missed_keepalive = 0;
    }

    rdma->last_keepalive = rdma->keepalive;

    if (rdma->keepalive_startup) {
        if (rdma->nb_missed_keepalive > RDMA_MAX_LOST_KEEPALIVE) {
            struct ibv_qp_attr attr = {.qp_state = IBV_QPS_ERR };
            SET_ERROR(rdma, -ENETUNREACH);
            ERROR(NULL, "peer keepalive failed.");
             
            if (ibv_modify_qp(rdma->lc_remote.qp, &attr, IBV_QP_STATE)) {
                ERROR(NULL, "modify QP to RTR");
                return;
            }
            return;
        }
    } else if (rdma->nb_missed_keepalive < RDMA_MAX_STARTUP_MISSED_KEEPALIVE) {
        DDPRINTF("Keepalive startup waiting: %" PRIu64 "\n",
                rdma->nb_missed_keepalive);
    } else {
        DDPRINTF("Keepalive startup too long.\n");
        rdma->keepalive_startup = true;
    }

reset:
    timer_mod(connection_timer, qemu_clock_get_ms(QEMU_CLOCK_REALTIME) +
                    RDMA_KEEPALIVE_INTERVAL_MS + first_missed);
}

static void qemu_rdma_keepalive_start(void)
{
    DPRINTF("Starting up keepalives....\n");
    timer_mod(connection_timer, qemu_clock_get_ms(QEMU_CLOCK_REALTIME) + 
                    RDMA_CONNECTION_INTERVAL_MS);
    timer_mod(keepalive_timer, qemu_clock_get_ms(QEMU_CLOCK_REALTIME) +
                    RDMA_KEEPALIVE_INTERVAL_MS);
}

static void *qemu_rdma_data_init(const char *host_port, Error **errp)
{
    RDMAContext *rdma = NULL;
    InetSocketAddress *addr;

    if (host_port) {
        rdma = g_malloc0(sizeof(RDMAContext));
        memset(rdma, 0, sizeof(RDMAContext));
        rdma->chunk_remote.current_block_idx = -1;
        rdma->chunk_remote.current_chunk = -1;
        rdma->chunk_local_src.current_block_idx = -1;
        rdma->chunk_local_src.current_chunk = -1;
        rdma->chunk_local_dest.current_block_idx = -1;
        rdma->chunk_local_dest.current_chunk = -1;

        addr = inet_parse(host_port, NULL);
        if (addr != NULL) {
            rdma->port = atoi(addr->port);
            rdma->host = g_strdup(addr->host);
        } else {
            ERROR(errp, "bad RDMA migration address '%s'", host_port);
            g_free(rdma);
            return NULL;
        }
    }
       
    rdma->keepalive_startup = false;
    connection_timer = timer_new_ms(QEMU_CLOCK_REALTIME, check_qp_state, rdma);
    keepalive_timer = timer_new_ms(QEMU_CLOCK_REALTIME, send_keepalive, rdma);
    rdma->lc_dest.id_str = "local destination";
    rdma->lc_src.id_str = "local src";
    rdma->lc_remote.id_str = "remote";


    return rdma;
}

/*
 * QEMUFile interface to the control channel.
 * SEND messages for control only.
 * pc.ram is handled with regular RDMA messages.
 */
static int qemu_rdma_put_buffer(void *opaque, const uint8_t *buf,
                                int64_t pos, int size)
{
    QEMUFileRDMA *r = opaque;
    QEMUFile *f = r->file;
    RDMAContext *rdma = r->rdma;
    size_t remaining = size;
    uint8_t * data = (void *) buf;
    int ret;

    CHECK_ERROR_STATE();

    /*
     * Push out any writes that
     * we're queued up for pc.ram.
     */
    ret = qemu_rdma_write(f, rdma, &rdma->chunk_remote, NULL);
    if (ret < 0) {
        SET_ERROR(rdma, ret);
        return ret;
    }

    while (remaining) {
        RDMAControlHeader head;

        r->len = MIN(remaining, RDMA_SEND_INCREMENT);
        remaining -= r->len;

        head.len = r->len;
        head.type = RDMA_CONTROL_QEMU_FILE;

        ret = qemu_rdma_exchange_send(rdma, &head, data, NULL, NULL, NULL);

        if (ret < 0) {
            SET_ERROR(rdma, ret);
            return ret;
        }

        data += r->len;
    }

    return size;
}

static size_t qemu_rdma_fill(RDMAContext *rdma, uint8_t *buf,
                             int size, int idx)
{
    size_t len = 0;

    if (rdma->wr_data[idx].control_len) {
        DDDPRINTF("RDMA %" PRId64 " of %d bytes already in buffer\n",
                    rdma->wr_data[idx].control_len, size);

        len = MIN(size, rdma->wr_data[idx].control_len);
        memcpy(buf, rdma->wr_data[idx].control_curr, len);
        rdma->wr_data[idx].control_curr += len;
        rdma->wr_data[idx].control_len -= len;
    }

    return len;
}

/*
 * QEMUFile interface to the control channel.
 * RDMA links don't use bytestreams, so we have to
 * return bytes to QEMUFile opportunistically.
 */
static int qemu_rdma_get_buffer(void *opaque, uint8_t *buf,
                                int64_t pos, int size)
{
    QEMUFileRDMA *r = opaque;
    RDMAContext *rdma = r->rdma;
    RDMAControlHeader head;
    int ret = 0;

    CHECK_ERROR_STATE();

    /*
     * First, we hold on to the last SEND message we
     * were given and dish out the bytes until we run
     * out of bytes.
     */
    r->len = qemu_rdma_fill(r->rdma, buf, size, 0);
    if (r->len) {
        return r->len;
    }

    /*
     * Once we run out, we block and wait for another
     * SEND message to arrive.
     */
    ret = qemu_rdma_exchange_recv(rdma, &head, RDMA_CONTROL_QEMU_FILE);

    if (ret < 0) {
        SET_ERROR(rdma, ret);
        return ret;
    }

    /*
     * SEND was received with new bytes, now try again.
     */
    return qemu_rdma_fill(r->rdma, buf, size, 0);
}

/*
 * Block until all the outstanding chunks have been delivered by the hardware.
 */
static int qemu_rdma_drain_cq(QEMUFile *f, RDMAContext *rdma,
                              RDMACurrentChunk *src,
                              RDMACurrentChunk *dest)
{
    int ret;
    RDMALocalContext *lc = (dest && dest != src) ? 
            (rdma->source ? &rdma->lc_src : &rdma->lc_dest) : &rdma->lc_remote;

    if (qemu_rdma_write(f, rdma, src, dest) < 0) {
        return -EIO;
    }

    while (lc->nb_sent) {
        ret = qemu_rdma_block_for_wrid(rdma, lc,
                                       RDMA_WRID_RDMA_WRITE_REMOTE, NULL);
        if (ret < 0) {
            ERROR(NULL, "complete polling!");
            return -EIO;
        }
    }

    qemu_rdma_unregister_waiting(rdma);

    return 0;
}

static int qemu_rdma_close(void *opaque)
{
    DPRINTF("Shutting down connection.\n");
    QEMUFileRDMA *r = opaque;
    if (r->rdma) {
        qemu_rdma_cleanup(r->rdma, false);
        g_free(r->rdma);
    }
    g_free(r);
    return 0;
}

static int qemu_rdma_instruct_unregister(RDMAContext *rdma, QEMUFile *f,
                                         ram_addr_t block_offset,
                                         ram_addr_t offset, long size)
{
    int ret;
    uint64_t block, chunk;

    if (size < 0) {
        ret = qemu_rdma_drain_cq(f, rdma, &rdma->chunk_remote, NULL);
        if (ret < 0) {
            fprintf(stderr, "rdma: failed to synchronously drain"
                            " completion queue before unregistration.\n");
            return ret;
        }
    }

    ret = qemu_rdma_search_ram_block(rdma, block_offset, 
                                     offset, size, &block, &chunk);

    if (ret) {
        fprintf(stderr, "ram block search failed\n");
        return ret;
    }

    qemu_rdma_signal_unregister(rdma, block, chunk, 0);

    /*
     * Synchronous, gauranteed unregistration (should not occur during
     * fast-path). Otherwise, unregisters will process on the next call to
     * qemu_rdma_drain_cq()
     */
    if (size < 0) {
        qemu_rdma_unregister_waiting(rdma);
    }

    return 0;
}


static int qemu_rdma_poll_until_empty(RDMAContext *rdma, RDMALocalContext *lc)
{
    uint64_t wr_id, wr_id_in;
    int ret;

    /*
     * Drain the Completion Queue if possible, but do not block,
     * just poll.
     *
     * If nothing to poll, the end of the iteration will do this
     * again to make sure we don't overflow the request queue.
     */
    while (1) {
        ret = qemu_rdma_poll(rdma, lc, &wr_id_in, NULL);
        if (ret < 0) {
            ERROR(NULL, "empty polling error! %d", ret);
            return ret;
        }

        wr_id = wr_id_in & RDMA_WRID_TYPE_MASK;

        if (wr_id == RDMA_WRID_NONE) {
            break;
        }
    }

    return 0;
}

/*
 * Parameters:
 *    @offset_{source|dest} == 0 :
 *        This means that 'block_offset' is a full virtual address that does not
 *        belong to a RAMBlock of the virtual machine and instead
 *        represents a private malloc'd memory area that the caller wishes to
 *        transfer. Source and dest can be different (either real RAMBlocks or
 *        private).
 *
 *    @offset != 0 :
 *        Offset is an offset to be added to block_offset and used
 *        to also lookup the corresponding RAMBlock. Source and dest can be different 
 *        (either real RAMBlocks or private).
 *
 *    @size > 0 :
 *        Amount of memory to copy locally using RDMA.
 *
 *    @size == 0 :
 *        A 'hint' or 'advice' that means that we wish to speculatively
 *        and asynchronously unregister either the source or destination memory.
 *        In this case, there is no gaurantee that the unregister will actually happen, 
 *        for example, if the memory is being actively copied. Additionally, the memory
 *        may be re-registered at any future time if a copy within the same
 *        range was requested again, even if you attempted to unregister it here.
 *
 *    @size < 0 : TODO, not yet supported
 *        Unregister the memory NOW. This means that the caller does not
 *        expect there to be any future RDMA copies and we just want to clean
 *        things up. This is used in case the upper layer owns the memory and
 *        cannot wait for qemu_fclose() to occur.
 */
static int qemu_rdma_copy_page(QEMUFile *f, void *opaque,
                                  ram_addr_t block_offset_dest,
                                  ram_addr_t offset_dest,
                                  ram_addr_t block_offset_source,
                                  ram_addr_t offset_source,
                                  long size)
{
    QEMUFileRDMA *rfile = opaque;
    RDMAContext *rdma = rfile->rdma;
    int ret;
    RDMACurrentChunk *src = &rdma->chunk_local_src;
    RDMACurrentChunk *dest = &rdma->chunk_local_dest;

    CHECK_ERROR_STATE();

    qemu_fflush(f);

    if (size > 0) {
        /*
         * Add this page to the current 'chunk'. If the chunk
         * is full, or the page doen't belong to the current chunk,
         * an actual RDMA write will occur and a new chunk will be formed.
         */
        src->block_offset = block_offset_source;
        src->offset = offset_source;
        dest->block_offset = block_offset_dest;
        dest->offset = offset_dest;

        DDPRINTF("Copy page: %p src offset %" PRIu64
                " dest %p offset %" PRIu64 "\n",
                (void *) block_offset_source, offset_source,
                (void *) block_offset_dest, offset_dest);

        ret = qemu_rdma_flush_unmergable(rdma, src, dest, f, size);

        if (ret) {
            ERROR(NULL, "local copy flush");
            goto err;
        }

        if ((src->current_length >= RDMA_MERGE_MAX) || 
            (dest->current_length >= RDMA_MERGE_MAX)) {
            ret = qemu_rdma_write(f, rdma, src, dest);

            if (ret < 0) {
                goto err;
            }
        } else {
            ret = 0;
        }
    } else {
        ret = qemu_rdma_instruct_unregister(rdma, f, block_offset_source,
                                                  offset_source, size);
        if (ret) {
            goto err;
        }

        ret = qemu_rdma_instruct_unregister(rdma, f, block_offset_dest, 
                                                  offset_dest, size);

        if (ret) {
            goto err;
        }
    }

    ret = qemu_rdma_poll_until_empty(rdma, 
                rdma->source ? &rdma->lc_src : &rdma->lc_dest);

    if (ret) {
        goto err;
    }

    return RAM_COPY_CONTROL_DELAYED;
err:
    SET_ERROR(rdma, ret);
    return ret;
}

/*
 * Parameters:
 *    @offset == 0 :
 *        This means that 'block_offset' is a full virtual address that does not
 *        belong to a RAMBlock of the virtual machine and instead
 *        represents a private malloc'd memory area that the caller wishes to
 *        transfer.
 *
 *        This allows callers to initiate RDMA transfers of arbitrary memory
 *        areas and not just only by migration itself.
 *
 *        If this is true, then the virtual address specified by 'block_offset'
 *        below must have been pre-registered with us in advance by calling the
 *        new QEMUFileOps->add()/remove() functions on both sides of the
 *        connection.
 *
 *        Also note: add()/remove() must been called in the *same sequence* and
 *        against the *same size* private virtual memory on both sides of the
 *        connection for this to work, regardless whether or not transfer of
 *        this private memory was initiated by the migration code or a private
 *        caller.
 *
 *    @offset != 0 :
 *        Offset is an offset to be added to block_offset and used
 *        to also lookup the corresponding RAMBlock.
 *
 *    @size > 0 :
 *        Initiate an transfer this size.
 *
 *    @size == 0 :
 *        A 'hint' that means that we wish to speculatively
 *        and asynchronously unregister this memory. In this case, there is no
 *        guarantee that the unregister will actually happen, for example,
 *        if the memory is being actively transmitted. Additionally, the memory
 *        may be re-registered at any future time if a write within the same
 *        chunk was requested again, even if you attempted to unregister it
 *        here.
 *
 *    @size < 0 : TODO, not yet supported
 *        Unregister the memory NOW. This means that the caller does not
 *        expect there to be any future RDMA transfers and we just want to clean
 *        things up. This is used in case the upper layer owns the memory and
 *        cannot wait for qemu_fclose() to occur.
 *
 *    @bytes_sent : User-specificed pointer to indicate how many bytes were
 *                  sent. Usually, this will not be more than a few bytes of
 *                  the protocol because most transfers are sent asynchronously.
 */
static int qemu_rdma_save_page(QEMUFile *f, void *opaque,
                                  ram_addr_t block_offset,
                                  uint8_t *host_addr,
                                  ram_addr_t offset,
                                  long size, int *bytes_sent)
{
    QEMUFileRDMA *rfile = opaque;
    RDMAContext *rdma = rfile->rdma;
    RDMACurrentChunk *cc = &rdma->chunk_remote;
    int ret;

    CHECK_ERROR_STATE();

    qemu_fflush(f);

    if (size > 0) {
        /*
         * Add this page to the current 'chunk'. If the chunk
         * is full, or the page doen't belong to the current chunk,
         * an actual RDMA write will occur and a new chunk will be formed.
         */
        cc->block_offset = block_offset;
        cc->offset = offset;

        ret = qemu_rdma_flush_unmergable(rdma, cc, NULL, f, size);

        if (ret) {
            ERROR(NULL, "remote flush unmergable");
            goto err;
        }

        if (cc->current_length >= RDMA_MERGE_MAX) {
            ret = qemu_rdma_write(f, rdma, cc, NULL);

            if (ret < 0) {
                ERROR(NULL, "remote write! %d", ret);
                goto err;
            }
        } else {
            ret = 0;
        }

        /*
         * We always return 1 bytes because the RDMA
         * protocol is completely asynchronous. We do not yet know
         * whether an  identified chunk is zero or not because we're
         * waiting for other pages to potentially be merged with
         * the current chunk. So, we have to call qemu_update_position()
         * later on when the actual write occurs.
         */
        if (bytes_sent) {
            *bytes_sent = 1;
        }
    } else {
        ret = qemu_rdma_instruct_unregister(rdma, f, block_offset, offset, size);

        if (ret) {
            goto err;
        }
    }

    ret = qemu_rdma_poll_until_empty(rdma, &rdma->lc_remote);

    if (ret) {
        goto err;
    }

    return RAM_SAVE_CONTROL_DELAYED;
err:
    SET_ERROR(rdma, ret);
    return ret;
}

static int qemu_rdma_accept(RDMAContext *rdma)
{
    RDMACapabilities cap;
    struct rdma_conn_param conn_param = {
                                            .responder_resources = 2,
                                            .private_data = &cap,
                                            .private_data_len = sizeof(cap),
                                         };
    struct rdma_cm_event *cm_event;
    struct ibv_context *verbs;
    int ret = -EINVAL;
    int idx;

    ret = rdma_get_cm_event(rdma->channel, &cm_event);
    if (ret) {
        goto err_rdma_dest_wait;
    }

    if (cm_event->event != RDMA_CM_EVENT_CONNECT_REQUEST) {
        rdma_ack_cm_event(cm_event);
        goto err_rdma_dest_wait;
    }

    memcpy(&cap, cm_event->param.conn.private_data, sizeof(cap));

    network_to_caps(&cap);

    if (cap.version < 1 || cap.version > RDMA_CONTROL_VERSION_CURRENT) {
            fprintf(stderr, "Unknown source RDMA version: %d, bailing...\n",
                            cap.version);
            rdma_ack_cm_event(cm_event);
            goto err_rdma_dest_wait;
    }

    rdma->keepalive_rkey = cap.keepalive_rkey;
    rdma->keepalive_addr = cap.keepalive_addr;

    DDPRINTF("Received keepalive params: key %x addr: %" PRIx64 
            " local %" PRIx64 "\n",
            cap.keepalive_rkey, cap.keepalive_addr, (uint64_t) &rdma->keepalive);

    /*
     * Respond with only the capabilities this version of QEMU knows about.
     */
    cap.flags &= known_capabilities;

    /*
     * Enable the ones that we do know about.
     * Add other checks here as new ones are introduced.
     */
    rdma->pin_all = cap.flags & RDMA_CAPABILITY_PIN_ALL;
    rdma->do_keepalive = cap.flags & RDMA_CAPABILITY_KEEPALIVE;

    rdma->cm_id = cm_event->id;
    verbs = cm_event->id->verbs;

    rdma_ack_cm_event(cm_event);

    DPRINTF("Memory pin all: %s\n", rdma->pin_all ? "enabled" : "disabled");
    DPRINTF("Keepalives: %s\n", rdma->do_keepalive ? "enabled" : "disabled");

    DPRINTF("verbs context after listen: %p\n", verbs);

    if (!rdma->lc_remote.verbs) {
        rdma->lc_remote.verbs = verbs;
    } else if (rdma->lc_remote.verbs != verbs) {
        ERROR(NULL, "ibv context %p != %p!", rdma->lc_remote.verbs, verbs);
        goto err_rdma_dest_wait;
    }

    qemu_rdma_dump_id("dest_init", verbs);

    ret = qemu_rdma_alloc_pd_cq(rdma, &rdma->lc_remote);
    if (ret) {
        ERROR(NULL, "error allocating pd and cq!");
        goto err_rdma_dest_wait;
    }

    ret = qemu_rdma_alloc_keepalive(rdma);

    if (ret) {
        ERROR(NULL, "allocating keepalive structures");
        goto err_rdma_dest_wait;
    }

    cap.keepalive_rkey = rdma->keepalive_mr->rkey,
    cap.keepalive_addr = (uint64_t) &rdma->keepalive;

    DDPRINTF("Sending keepalive params: key %x addr: %" PRIx64 
            " remote: %" PRIx64 "\n",
            cap.keepalive_rkey, cap.keepalive_addr, rdma->keepalive_addr);
    caps_to_network(&cap);

    ret = qemu_rdma_alloc_qp(rdma);
    if (ret) {
        ERROR(NULL, "allocating qp!");
        goto err_rdma_dest_wait;
    }

    ret = qemu_rdma_init_ram_blocks(rdma);
    if (ret) {
        ERROR(NULL, "initializing ram blocks!");
        goto err_rdma_dest_wait;
    }

    for (idx = 0; idx < RDMA_WRID_MAX; idx++) {
        ret = qemu_rdma_reg_control(rdma, idx);
        if (ret) {
            ERROR(NULL, "registering %d control!", idx);
            goto err_rdma_dest_wait;
        }
    }

    qemu_set_fd_handler2(rdma->channel->fd, NULL, NULL, NULL, NULL);

    ret = rdma_accept(rdma->cm_id, &conn_param);
    if (ret) {
        ERROR(NULL, "rdma_accept returns %d!", ret);
        goto err_rdma_dest_wait;
    }

    ret = rdma_get_cm_event(rdma->channel, &cm_event);
    if (ret) {
        ERROR(NULL, "rdma_accept get_cm_event failed %d!", ret);
        goto err_rdma_dest_wait;
    }

    if (cm_event->event != RDMA_CM_EVENT_ESTABLISHED) {
        ERROR(NULL, "rdma_accept not event established!");
        rdma_ack_cm_event(cm_event);
        goto err_rdma_dest_wait;
    }

    rdma_ack_cm_event(cm_event);
    rdma->connected = true;

    ret = qemu_rdma_post_recv_control(rdma, RDMA_WRID_READY);
    if (ret) {
        ERROR(NULL, "posting second control recv!");
        goto err_rdma_dest_wait;
    }

    qemu_rdma_dump_gid("dest_connect", rdma->cm_id);

    return 0;

err_rdma_dest_wait:
    SET_ERROR(rdma, ret);
    qemu_rdma_cleanup(rdma, false);
    return ret;
}

/*
 * During each iteration of the migration, we listen for instructions
 * by the source VM to perform pinning operations before they
 * can perform RDMA operations.
 *
 * Keep doing this until the source tells us to stop.
 */
static int qemu_rdma_registration_handle(QEMUFile *f, void *opaque,
                                         uint64_t flags)
{
    RDMAControlHeader reg_resp = { .len = sizeof(RDMARegisterResult),
                               .type = RDMA_CONTROL_REGISTER_RESULT,
                               .repeat = 0,
                             };
    RDMAControlHeader unreg_resp = { .len = 0,
                               .type = RDMA_CONTROL_UNREGISTER_FINISHED,
                               .repeat = 0,
                             };
    RDMAControlHeader blocks = { .type = RDMA_CONTROL_RAM_BLOCKS_RESULT,
                                 .repeat = 1 };
    QEMUFileRDMA *rfile = opaque;
    RDMAContext *rdma = rfile->rdma;
    RDMALocalBlocks *local = &rdma->local_ram_blocks;
    RDMAControlHeader head;
    RDMARegister *reg, *registers;
    RDMACompress *comp;
    RDMARegisterResult *reg_result;
    RDMALocalBlock *block;
    static RDMARegisterResult results[RDMA_CONTROL_MAX_COMMANDS_PER_MESSAGE];
    void *host_addr;
    int ret = 0;
    int idx = 0;
    int count = 0;
    int i = 0;

    CHECK_ERROR_STATE();

    do {
        DDDPRINTF("Waiting for next request %" PRIu64 "...\n", flags);

        ret = qemu_rdma_exchange_recv(rdma, &head, RDMA_CONTROL_NONE);

        if (ret < 0) {
            break;
        }

        if (head.repeat > RDMA_CONTROL_MAX_COMMANDS_PER_MESSAGE) {
            fprintf(stderr, "rdma: Too many requests in this message (%d)."
                            "Bailing.\n", head.repeat);
            ret = -EIO;
            break;
        }

        switch (head.type) {
        case RDMA_CONTROL_COMPRESS:
            comp = (RDMACompress *) rdma->wr_data[idx].control_curr;
            network_to_compress(comp);

            DDPRINTF("Zapping zero chunk: %" PRId64
                    " bytes, index %d, offset %" PRId64 "\n",
                    comp->length, comp->block_idx, comp->offset);
            block = &(rdma->local_ram_blocks.block[comp->block_idx]);

            host_addr = block->local_host_addr +
                            (comp->offset - block->offset);

            ram_handle_compressed(host_addr, comp->value, comp->length);
            break;

        case RDMA_CONTROL_REGISTER_FINISHED:
            DDDPRINTF("Current registrations complete.\n");
            goto out;

        case RDMA_CONTROL_RAM_BLOCKS_REQUEST:
            DPRINTF("Initial setup info requested.\n");

            if (rdma->pin_all) {
                ret = qemu_rdma_reg_whole_ram_blocks(rdma);
                if (ret) {
                    ERROR(NULL, "dest registering ram blocks!");
                    goto out;
                }
            }

            /*
             * Dest uses this to prepare to transmit the RAMBlock descriptions
             * to the source VM after connection setup.
             * Both sides use the "remote" structure to communicate and update
             * their "local" descriptions with what was sent.
             */
            for (i = 0; i < local->nb_blocks; i++) {
                rdma->block[i].remote_host_addr =
                    (uint64_t)(local->block[i].local_host_addr);

                if (rdma->pin_all) {
                    rdma->block[i].remote_rkey = local->block[i].mr->rkey;
                }

                rdma->block[i].offset = local->block[i].offset;
                rdma->block[i].length = local->block[i].length;

                remote_block_to_network(&rdma->block[i]);
            }

            blocks.len = rdma->local_ram_blocks.nb_blocks
                                                * sizeof(RDMARemoteBlock);


            ret = qemu_rdma_post_send_control(rdma,
                                        (uint8_t *) rdma->block, &blocks);

            if (ret < 0) {
                ERROR(NULL, "sending remote info!");
                goto out;
            }

            break;
        case RDMA_CONTROL_REGISTER_REQUEST:
            DDPRINTF("There are %d registration requests\n", head.repeat);

            reg_resp.repeat = head.repeat;
            registers = (RDMARegister *) rdma->wr_data[idx].control_curr;

            for (count = 0; count < head.repeat; count++) {
                RDMACurrentChunk cc;

                reg = &registers[count];
                network_to_register(reg);

                reg_result = &results[count];

                DDPRINTF("Registration request (%d): index %d, current_addr %"
                         PRIu64 " chunks: %" PRIu64 "\n", count,
                         reg->current_block_idx, reg->key.current_addr, reg->chunks);

                cc.block = &(rdma->local_ram_blocks.block[reg->current_block_idx]);
                if (cc.block->is_ram_block) {
                    cc.addr = (cc.block->local_host_addr +
                                (reg->key.current_addr - cc.block->offset));
                    cc.chunk_idx = ram_chunk_index(block->local_host_addr, cc.addr);
                } else {
                    cc.chunk_idx = reg->key.chunk;
                    cc.addr = cc.block->local_host_addr +
                        (reg->key.chunk * (1UL << RDMA_REG_CHUNK_SHIFT));
                }
                cc.chunk_start = ram_chunk_start(cc.block, cc.chunk_idx);
                cc.chunk_end = ram_chunk_end(cc.block, cc.chunk_idx + reg->chunks);
                if (qemu_rdma_register_and_get_keys(rdma, &cc, &rdma->lc_remote,
                                            false, NULL, &reg_result->rkey)) {
                    fprintf(stderr, "cannot get rkey!\n");
                    ret = -EINVAL;
                    goto out;
                }

                reg_result->host_addr = (uint64_t) cc.block->local_host_addr;

                DDPRINTF("Registered rkey for this request: %x\n",
                                reg_result->rkey);

                result_to_network(reg_result);
            }

            ret = qemu_rdma_post_send_control(rdma,
                            (uint8_t *) results, &reg_resp);

            if (ret < 0) {
                fprintf(stderr, "Failed to send control buffer!\n");
                goto out;
            }
            break;
        case RDMA_CONTROL_UNREGISTER_REQUEST:
            DDPRINTF("There are %d unregistration requests\n", head.repeat);
            unreg_resp.repeat = head.repeat;
            registers = (RDMARegister *) rdma->wr_data[idx].control_curr;

            for (count = 0; count < head.repeat; count++) {
                reg = &registers[count];
                network_to_register(reg);

                DDPRINTF("Unregistration request (%d): "
                         " index %d, chunk %" PRIu64 "\n",
                         count, reg->current_block_idx, reg->key.chunk);

                block = &(rdma->local_ram_blocks.block[reg->current_block_idx]);

                ret = ibv_dereg_mr(block->pmr[reg->key.chunk]);
                block->pmr[reg->key.chunk] = NULL;

                if (ret != 0) {
                    perror("rdma unregistration chunk failed");
                    ret = -ret;
                    goto out;
                }

                rdma->total_registrations--;

                DDPRINTF("Unregistered chunk %" PRIu64 " successfully.\n",
                            reg->key.chunk);
            }

            ret = qemu_rdma_post_send_control(rdma, NULL, &unreg_resp);

            if (ret < 0) {
                fprintf(stderr, "Failed to send control buffer!\n");
                goto out;
            }
            break;
        case RDMA_CONTROL_REGISTER_RESULT:
            fprintf(stderr, "Invalid RESULT message at dest.\n");
            ret = -EIO;
            goto out;
        default:
            fprintf(stderr, "Unknown control message %s\n",
                                control_desc[head.type]);
            ret = -EIO;
            goto out;
        }
    } while (1);
out:
    if (ret < 0) {
        SET_ERROR(rdma, ret);
    }
    return ret;
}

static int qemu_rdma_registration_start(QEMUFile *f, void *opaque,
                                        uint64_t flags)
{
    QEMUFileRDMA *rfile = opaque;
    RDMAContext *rdma = rfile->rdma;

    CHECK_ERROR_STATE();

    DDDPRINTF("start section: %" PRIu64 "\n", flags);

    if (flags == RAM_CONTROL_FLUSH) {
        int ret;

        if (rdma->source) {
            ret = qemu_rdma_drain_cq(f, rdma, &rdma->chunk_local_src, 
                                              &rdma->chunk_local_dest);

            if (ret < 0) {
                return ret;
            }
        }

    } else {
        qemu_put_be64(f, RAM_SAVE_FLAG_HOOK);
    }

    qemu_fflush(f);

    return 0;
}

/*
 * Inform dest that dynamic registrations are done for now.
 * First, flush writes, if any.
 */
static int qemu_rdma_registration_stop(QEMUFile *f, void *opaque,
                                       uint64_t flags)
{
    Error *local_err = NULL, **errp = &local_err;
    QEMUFileRDMA *rfile = opaque;
    RDMAContext *rdma = rfile->rdma;
    RDMAControlHeader head = { .len = 0, .repeat = 1 };
    int ret = 0;

    CHECK_ERROR_STATE();

    qemu_fflush(f);
    ret = qemu_rdma_drain_cq(f, rdma, &rdma->chunk_remote, NULL);

    if (ret < 0) {
        goto err;
    }

    if (flags == RAM_CONTROL_SETUP) {
        RDMAControlHeader resp = {.type = RDMA_CONTROL_RAM_BLOCKS_RESULT };
        RDMALocalBlocks *local = &rdma->local_ram_blocks;
        int reg_result_idx, i, j, nb_remote_blocks;

        head.type = RDMA_CONTROL_RAM_BLOCKS_REQUEST;
        DPRINTF("Sending registration setup for ram blocks...\n");

        /*
         * Make sure that we parallelize the pinning on both sides.
         * For very large guests, doing this serially takes a really
         * long time, so we have to 'interleave' the pinning locally
         * with the control messages by performing the pinning on this
         * side before we receive the control response from the other
         * side that the pinning has completed.
         */
        ret = qemu_rdma_exchange_send(rdma, &head, NULL, &resp,
                    &reg_result_idx, rdma->pin_all ?
                    qemu_rdma_reg_whole_ram_blocks : NULL);
        if (ret < 0) {
            ERROR(errp, "receiving remote info!");
            return ret;
        }

        nb_remote_blocks = resp.len / sizeof(RDMARemoteBlock);

        /*
         * The protocol uses two different sets of rkeys (mutually exclusive):
         * 1. One key to represent the virtual address of the entire ram block.
         *    (pinning enabled - pin everything with one rkey.)
         * 2. One to represent individual chunks within a ram block.
         *    (pinning disabled - pin individual chunks.)
         *
         * Once the capability is successfully negotiated, the destination transmits
         * the keys to use (or sends them later) including the virtual addresses
         * and then propagates the remote ram block descriptions to their local copy.
         */

        if (local->nb_blocks != nb_remote_blocks) {
            ERROR(errp, "ram blocks mismatch #1! "
                        "Your QEMU command line parameters are probably "
                        "not identical on both the source and destination.");
            return -EINVAL;
        }

        qemu_rdma_move_header(rdma, reg_result_idx, &resp);
        memcpy(rdma->block,
            rdma->wr_data[reg_result_idx].control_curr, resp.len);
        for (i = 0; i < nb_remote_blocks; i++) {
            network_to_remote_block(&rdma->block[i]);

            /* search local ram blocks */
            for (j = 0; j < local->nb_blocks; j++) {
                if (rdma->block[i].offset != local->block[j].offset) {
                    continue;
                }

                if (rdma->block[i].length != local->block[j].length) {
                    ERROR(errp, "ram blocks mismatch #2! "
                        "Your QEMU command line parameters are probably "
                        "not identical on both the source and destination.");
                    return -EINVAL;
                }
                local->block[j].remote_host_addr =
                        rdma->block[i].remote_host_addr;
                local->block[j].remote_rkey = rdma->block[i].remote_rkey;
                break;
            }

            if (j >= local->nb_blocks) {
                ERROR(errp, "ram blocks mismatch #3! "
                        "Your QEMU command line parameters are probably "
                        "not identical on both the source and destination.");
                return -EINVAL;
            }
        }
    }

    DDDPRINTF("Sending registration finish %" PRIu64 "...\n", flags);

    head.type = RDMA_CONTROL_REGISTER_FINISHED;
    ret = qemu_rdma_exchange_send(rdma, &head, NULL, NULL, NULL, NULL);

    if (ret < 0) {
        goto err;
    }

    return 0;
err:
    SET_ERROR(rdma, ret);
    return ret;
}

static int qemu_rdma_get_fd(void *opaque)
{
    QEMUFileRDMA *rfile = opaque;
    RDMAContext *rdma = rfile->rdma;

    return rdma->lc_remote.comp_chan->fd;
}

static int qemu_rdma_delete_block(QEMUFile *f, void *opaque,
                                  ram_addr_t block_offset)
{
    QEMUFileRDMA *rfile = opaque;
    return __qemu_rdma_delete_block(rfile->rdma, block_offset);
}


static int qemu_rdma_add_block(QEMUFile *f, void *opaque, void *host_addr,
                         ram_addr_t block_offset, uint64_t length)
{
    QEMUFileRDMA *rfile = opaque;
    return __qemu_rdma_add_block(rfile->rdma, host_addr,
                                 block_offset, length);
}

const QEMUFileOps rdma_read_ops = {
    .get_buffer    = qemu_rdma_get_buffer,
    .get_fd        = qemu_rdma_get_fd,
    .close         = qemu_rdma_close,
    .hook_ram_load = qemu_rdma_registration_handle,
    .copy_page     = qemu_rdma_copy_page,
    .add           = qemu_rdma_add_block,
    .remove        = qemu_rdma_delete_block,
};

const QEMUFileOps rdma_write_ops = {
    .put_buffer         = qemu_rdma_put_buffer,
    .close              = qemu_rdma_close,
    .before_ram_iterate = qemu_rdma_registration_start,
    .after_ram_iterate  = qemu_rdma_registration_stop,
    .save_page          = qemu_rdma_save_page,
    .copy_page          = qemu_rdma_copy_page,
    .add                = qemu_rdma_add_block,
    .remove             = qemu_rdma_delete_block,
};

static void *qemu_fopen_rdma(RDMAContext *rdma, const char *mode)
{
    QEMUFileRDMA *r = g_malloc0(sizeof(QEMUFileRDMA));

    if (qemu_file_mode_is_not_valid(mode)) {
        return NULL;
    }

    r->rdma = rdma;

    if (mode[0] == 'w') {
        r->file = qemu_fopen_ops(r, &rdma_write_ops);
    } else {
        r->file = qemu_fopen_ops(r, &rdma_read_ops);
    }

    return r->file;
}

static int connect_local(RDMAContext *rdma,
                                   RDMALocalContext *src,
                                   RDMALocalContext *dest)
{
    int ret;
	struct ibv_qp_attr next = {
			.qp_state = IBV_QPS_RTR,
			.path_mtu = IBV_MTU_1024,
			.dest_qp_num = src->qp->qp_num,
			.rq_psn = src->psn,
			.max_dest_rd_atomic = 1,
			.min_rnr_timer = 12,
			.ah_attr = {
				.is_global = 0,
				.dlid = src->port.lid,
				.sl = 0,
				.src_path_bits = 0,
				.port_num = src->port_num,
			}
	};

	if(src->gid.global.interface_id) {
		next.ah_attr.is_global = 1;
		next.ah_attr.grh.hop_limit = 1;
		next.ah_attr.grh.dgid = src->gid;
		next.ah_attr.grh.sgid_index = 0;
	}

	ret = ibv_modify_qp(dest->qp, &next,
		IBV_QP_STATE |
		IBV_QP_AV |
		IBV_QP_PATH_MTU |
		IBV_QP_DEST_QPN |
		IBV_QP_RQ_PSN |
		IBV_QP_MAX_DEST_RD_ATOMIC |
		IBV_QP_MIN_RNR_TIMER);

    if (ret) {
        SET_ERROR(rdma, -ret);
		ERROR(NULL, "modify src verbs to ready");
		return rdma->error_state;
	}

	next.qp_state = IBV_QPS_RTS;
	next.timeout = 14;
	next.retry_cnt = 7;
	next.rnr_retry = 7;
	next.sq_psn = dest->psn;
	next.max_rd_atomic = 1; 

	ret = ibv_modify_qp(dest->qp, &next,
		IBV_QP_STATE |
		IBV_QP_TIMEOUT |
		IBV_QP_RETRY_CNT |
		IBV_QP_RNR_RETRY |
		IBV_QP_SQ_PSN |
		IBV_QP_MAX_QP_RD_ATOMIC);

    if (ret) {
        SET_ERROR(rdma, -ret);
		ERROR(NULL, "modify dest verbs to ready\n");
		return rdma->error_state;
	}

    return 0;
}

static int init_local(RDMAContext *rdma)
{
    DDPRINTF("Opening copy local source queue pair...\n");
    if (open_local(rdma, &rdma->lc_src)) {
        return 1;
    }

    DDPRINTF("Opening copy local destination queue pair...\n");
    if (open_local(rdma, &rdma->lc_dest)) {
        return 1;
    }

    DDPRINTF("Connecting local src queue pairs...\n");
    if (connect_local(rdma, &rdma->lc_src, &rdma->lc_dest)) {
        return 1;
    }

    DDPRINTF("Connecting local dest queue pairs...\n");
    if (connect_local(rdma, &rdma->lc_dest, &rdma->lc_src)) {
        return 1;
    }

    return 0;
}

static void rdma_accept_incoming_migration(void *opaque)
{
    RDMAContext *rdma = opaque;
    int ret;
    QEMUFile *f;
    Error *local_err = NULL, **errp = &local_err;

    DPRINTF("Accepting rdma connection...\n");
    ret = qemu_rdma_accept(rdma);

    if (ret) {
        ERROR(errp, "initialization failed!");
        return;
    }

    DPRINTF("Accepted migration\n");

    if (init_local(rdma)) {
        ERROR(errp, "could not initialize local rdma queue pairs!");
        goto err;
    }

    f = qemu_fopen_rdma(rdma, "rb");
    if (f == NULL) {
        ERROR(errp, "could not qemu_fopen_rdma!");
        goto err;
    }

    if (rdma->do_keepalive) {
        qemu_rdma_keepalive_start();
    }

    rdma->migration_started = 1;
    process_incoming_migration(f);
    return;
err:
    qemu_rdma_cleanup(rdma, false);
}

void rdma_start_incoming_migration(const char *host_port, Error **errp)
{
    int ret;
    RDMAContext *rdma;
    Error *local_err = NULL;

    DPRINTF("Starting RDMA-based incoming migration\n");
    rdma = qemu_rdma_data_init(host_port, &local_err);

    if (rdma == NULL) {
        goto err;
    }

    rdma->source = false;
    rdma->dest = true;

    ret = qemu_rdma_dest_init(rdma, &local_err);

    if (ret) {
        goto err;
    }

    DPRINTF("qemu_rdma_dest_init success\n");

    ret = rdma_listen(rdma->listen_id, 5);

    if (ret) {
        ERROR(errp, "listening on socket!");
        goto err;
    }

    DPRINTF("rdma_listen success\n");

    qemu_set_fd_handler2(rdma->channel->fd, NULL,
                         rdma_accept_incoming_migration, NULL,
                            (void *)(intptr_t) rdma);
    return;
err:
    error_propagate(errp, local_err);
    g_free(rdma);
}

void rdma_start_outgoing_migration(void *opaque,
                            const char *host_port, Error **errp)
{
    MigrationState *s = opaque;
    Error *local_err = NULL, **temp = &local_err;
    RDMAContext *rdma = qemu_rdma_data_init(host_port, &local_err);
    int ret = 0;

    if (rdma == NULL) {
        ERROR(temp, "Failed to initialize RDMA data structures! %d", ret);
        goto err;
    }

    rdma->source = true;
    rdma->dest = false;

    ret = qemu_rdma_source_init(rdma, &local_err, s);

    if (ret) {
        goto err;
    }

    DPRINTF("qemu_rdma_source_init success\n");
    ret = qemu_rdma_connect(rdma, &local_err);

    if (ret) {
        goto err;
    }

    if (init_local(rdma)) {
        ERROR(temp, "could not initialize local rdma queue pairs!");
        goto err;
    }

    DPRINTF("qemu_rdma_source_connect success\n");

    s->file = qemu_fopen_rdma(rdma, "wb");
    rdma->migration_started = 1;

    if (rdma->do_keepalive) {
        qemu_rdma_keepalive_start();
    }

    migrate_fd_connect(s);
    return;
err:
    error_propagate(errp, local_err);
    g_free(rdma);
    migrate_fd_error(s);
}
