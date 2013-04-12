/*
 *  Copyright (C) 2013 Michael R. Hines <mrhines@us.ibm.com>
 *  Copyright (C) 2010 Jiuxing Liu <jl@us.ibm.com>
 *
 *  RDMA protocol and interfaces
 *
 *  This program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation; under version 2 of the License.
 *
 *  This program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with this program; if not, see <http://www.gnu.org/licenses/>.
 */
#include "qemu-common.h"
#include "migration/migration.h"
#include "migration/qemu-file.h"
#include "exec/cpu-common.h"
#include "qemu/main-loop.h"
#include "qemu/sockets.h"
#include <stdio.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <netdb.h>
#include <arpa/inet.h>
#include <string.h>
#include <rdma/rdma_cma.h>

//#define DEBUG_RDMA
//#define DEBUG_RDMA_VERBOSE

#ifdef DEBUG_RDMA
#define DPRINTF(fmt, ...) \
    do { printf("rdma: " fmt, ## __VA_ARGS__); } while (0)
#else
#define DPRINTF(fmt, ...) \
    do { } while (0)
#endif

#ifdef DEBUG_RDMA_VERBOSE
#define DDPRINTF(fmt, ...) \
    do { printf("rdma: " fmt, ## __VA_ARGS__); } while (0)
#else
#define DDPRINTF(fmt, ...) \
    do { } while (0)
#endif

#define RDMA_RESOLVE_TIMEOUT_MS 10000

#define RDMA_CHUNK_REGISTRATION

#define RDMA_LAZY_CLIENT_REGISTRATION

/* Do not merge data if larger than this. */
#define RDMA_MERGE_MAX (4 * 1024 * 1024)
#define RDMA_UNSIGNALED_SEND_MAX 64

#define RDMA_REG_CHUNK_SHIFT 20 /* 1 MB */
//#define RDMA_REG_CHUNK_SHIFT 21 /* 2 MB */
//#define RDMA_REG_CHUNK_SHIFT 22 /* 4 MB */
//#define RDMA_REG_CHUNK_SHIFT 23 /* 8 MB */
//#define RDMA_REG_CHUNK_SHIFT 24 /* 16 MB */
//#define RDMA_REG_CHUNK_SHIFT 25 /* 32 MB */
//#define RDMA_REG_CHUNK_SHIFT 26 /* 64 MB */
//#define RDMA_REG_CHUNK_SHIFT 27 /* 128 MB */
//#define RDMA_REG_CHUNK_SHIFT 28 /* 256 MB */

#define RDMA_REG_CHUNK_SIZE (1UL << (RDMA_REG_CHUNK_SHIFT))
#define RDMA_REG_CHUNK_INDEX(start_addr, host_addr) \
            (((unsigned long)(host_addr) >> RDMA_REG_CHUNK_SHIFT) - \
            ((unsigned long)(start_addr) >> RDMA_REG_CHUNK_SHIFT))
#define RDMA_REG_NUM_CHUNKS(rdma_ram_block) \
            (RDMA_REG_CHUNK_INDEX((rdma_ram_block)->local_host_addr,\
                (rdma_ram_block)->local_host_addr +\
                (rdma_ram_block)->length) + 1)
#define RDMA_REG_CHUNK_START(rdma_ram_block, i) ((uint8_t *)\
            ((((unsigned long)((rdma_ram_block)->local_host_addr) >> \
                RDMA_REG_CHUNK_SHIFT) + (i)) << \
                RDMA_REG_CHUNK_SHIFT))
#define RDMA_REG_CHUNK_END(rdma_ram_block, i) \
            (RDMA_REG_CHUNK_START(rdma_ram_block, i) + \
             RDMA_REG_CHUNK_SIZE)

/*
 * This is only for non-live state being migrated.
 * Instead of RDMA_WRITE messages, we use RDMA_SEND
 * messages for that state, which requires a different
 * delivery design than main memory.
 */
#define RDMA_SEND_INCREMENT 32768

#define RDMA_BLOCKING
/*
 * Completion queue can be filled by both read and write work requests,
 * so must reflect the sum of both possible queue sizes.
 */
#define RDMA_QP_SIZE 1000
#define RDMA_CQ_SIZE (RDMA_QP_SIZE * 3)

/*
 * Maximum size infiniband SEND message
 */
#define RDMA_CONTROL_MAX_BUFFER (512 * 1024)
#define RDMA_CONTROL_MAX_WR 2
#define RDMA_CONTROL_MAX_COMMANDS_PER_MESSAGE 4096

/*
 * Capabilities for negotiation.
 */
#define RDMA_CAPABILITY_CHUNK_REGISTER 0x01
#define RDMA_CAPABILITY_NEXT_FEATURE   0x02

/*
 * RDMA migration protocol:
 * 1. RDMA Writes (data messages, i.e. RAM)
 * 2. IB Send/Recv (control channel messages)
 */
enum {
    RDMA_WRID_NONE = 0,
    RDMA_WRID_RDMA_WRITE,
    RDMA_WRID_SEND_CONTROL = 1000,
    RDMA_WRID_RECV_CONTROL = 2000,
};

const char *wrid_desc[] = {
        [RDMA_WRID_NONE] = "NONE",
        [RDMA_WRID_RDMA_WRITE] = "WRITE RDMA",
        [RDMA_WRID_SEND_CONTROL] = "CONTROL SEND",
        [RDMA_WRID_RECV_CONTROL] = "CONTROL RECV",
};

/*
 * SEND/RECV IB Control Messages.
 */
enum {
    RDMA_CONTROL_NONE = 0,
    RDMA_CONTROL_READY,                    /* ready to receive */
    RDMA_CONTROL_QEMU_FILE,                /* QEMUFile-transmitted bytes */
    RDMA_CONTROL_RAM_BLOCKS,               /* RAMBlock synchronization */
    RDMA_CONTROL_REGISTER_REQUEST,         /* dynamic page registration */
    RDMA_CONTROL_REGISTER_RESULT,          /* key to use after registration */
    RDMA_CONTROL_REGISTER_FINISHED,        /* current iteration finished */
};

const char *control_desc[] = {
        [RDMA_CONTROL_NONE] = "NONE",
        [RDMA_CONTROL_READY] = "READY",
        [RDMA_CONTROL_QEMU_FILE] = "QEMU FILE",
        [RDMA_CONTROL_RAM_BLOCKS] = "REMOTE INFO",
        [RDMA_CONTROL_REGISTER_REQUEST] = "REGISTER REQUEST",
        [RDMA_CONTROL_REGISTER_RESULT] = "REGISTER RESULT",
        [RDMA_CONTROL_REGISTER_FINISHED] = "REGISTER FINISHED",
};

/*
 * Memory and MR structures used to represent an IB Send/Recv work request.
 * This is *not* used for RDMA, only IB Send/Recv.
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
typedef struct {
    uint32_t version;
    uint32_t flags;
} RDMACapabilities;

/*
 * Main data structure for RDMA state.
 * While there is only one copy of this structure being allocated right now,
 * this is the place where one would start if you wanted to consider
 * having more than one RDMA connection open at the same time.
 */
typedef struct RDMAContext {
    char *host;
    int port;

    /* This is used by the migration protocol to transmit
     * control messages (such as device state and registration commands)
     *
     * WR #0 is for control channel ready messages from the server.
     * WR #1 is for control channel data messages from the server.
     * WR #2 is for control channel send messages.
     *
     * We could use more WRs, but we have enough for now.
     */
    RDMAWorkRequestData wr_data[RDMA_CONTROL_MAX_WR + 1];

    /*
     * This is used by *_exchange_send() to figure out whether or not
     * the initial "READY" message has already been received or not.
     * This is because other functions may potentially poll() and detect
     * the READY message before send() does, in which case we need to
     * know if it completed.
     */
    int control_ready_expected;

    /* The rest is only for the initiator of the migration. */
    int client_init_done;

    /* number of outstanding unsignaled send */
    int num_unsignaled_send;

    /* number of outstanding signaled send */
    int num_signaled_send;

    /* store info about current buffer so that we can
       merge it with future sends */
    uint64_t current_offset;
    uint64_t current_length;
    /* index of ram block the current buffer belongs to */
    int current_index;
    /* index of the chunk in the current ram block */
    int current_chunk;

    bool chunk_register_destination;

    /*
     * infiniband-specific variables for opening the device
     * and maintaining connection state and so forth.
     *
     * cm_id also has ibv_context, rdma_event_channel, and ibv_qp in
     * cm_id->verbs, cm_id->channel, and cm_id->qp.
     */
    struct rdma_cm_id *cm_id;               /* connection manager ID */
    struct rdma_cm_id *listen_id;

    struct ibv_context *verbs;
    struct rdma_event_channel *channel;
    struct ibv_qp *qp;                      /* queue pair */
    struct ibv_comp_channel *comp_channel;  /* completion channel */
    struct ibv_pd *pd;                      /* protection domain */
    struct ibv_cq *cq;                      /* completion queue */
} RDMAContext;

/*
 * Interface to the rest of the migration call stack.
 */
typedef struct QEMUFileRDMA {
    RDMAContext *rdma;
    size_t len;
    void *file;
} QEMUFileRDMA;

/*
 * Representation of a RAMBlock from an RDMA perspective.
 * This an subsequent structures cannot be linked lists
 * because we're using a single IB message to transmit
 * the information. It's small anyway, so a list is overkill.
 */
typedef struct RDMALocalBlock {
    uint8_t  *local_host_addr; /* local virtual address */
    uint64_t remote_host_addr; /* remote virtual address */
    uint64_t offset;
    uint64_t length;
    struct   ibv_mr **pmr;     /* MRs for chunk-level registration */
    struct   ibv_mr *mr;       /* MR for non-chunk-level registration */
    uint32_t *remote_keys;     /* rkeys for chunk-level registration */
    uint32_t remote_rkey;      /* rkeys for non-chunk-level registration */
} RDMALocalBlock;

/*
 * Also represents a RAMblock, but only on the server.
 * This gets transmitted by the server during connection-time
 * to the client / primary VM and then is used to populate the
 * corresponding RDMALocalBlock with
 * the information needed to perform the actual RDMA.
 *
 */
typedef struct RDMARemoteBlock {
    uint64_t remote_host_addr;
    uint64_t offset;
    uint64_t length;
    uint32_t remote_rkey;
} RDMARemoteBlock;

/*
 * Virtual address of the above structures used for transmitting
 * the RAMBlock descriptions at connection-time.
 */
typedef struct RDMALocalBlocks {
    int num_blocks;
    RDMALocalBlock *block;
} RDMALocalBlocks;

/*
 * Same as above
 */
typedef struct RDMARemoteBlocks {
    int *num_blocks;
    RDMARemoteBlock *block;
    void *remote_area;
    int remote_size;
} RDMARemoteBlocks;

#define RDMA_CONTROL_VERSION_1      1
//#define RDMA_CONTROL_VERSION_2      2  /* next version */
#define RDMA_CONTROL_VERSION_MAX    1
#define RDMA_CONTROL_VERSION_MIN    1    /* change on next version */

#define RDMA_CONTROL_CURRENT_VERSION RDMA_CONTROL_VERSION_1

/*
 * Main structure for IB Send/Recv control messages.
 * This gets prepended at the beginning of every Send/Recv.
 */
typedef struct {
    uint32_t    len;     /* Total length of data portion */
    uint32_t    type;    /* which control command to perform */
    uint32_t    version; /* version */
    uint32_t    repeat;  /* number of commands in data portion of same type */
} RDMAControlHeader;

/*
 * Register a single Chunk.
 * Information sent by the primary VM to inform the server
 * to register an single chunk of memory before we can perform
 * the actual RDMA operation.
 */
typedef struct {
    uint32_t   len;              /* length of the chunk to be registered */
    uint32_t   current_index;    /* which ramblock the chunk belongs to */
    uint64_t   offset;           /* offset into the ramblock of the chunk */
} RDMARegister;

/*
 * The result of the server's memory registration produces an "rkey"
 * which the primary VM must reference in order to perform
 * the RDMA operation.
 */
typedef struct {
    uint32_t rkey;
} RDMARegisterResult;

#define RDMAControlHeaderSize sizeof(RDMAControlHeader)

RDMALocalBlocks local_ram_blocks;
RDMARemoteBlocks remote_ram_blocks;

/*
 * Memory regions need to be registered with the device and queue pairs setup
 * in advanced before the migration starts. This tells us where the RAM blocks
 * are so that we can register them individually.
 */
static void qemu_rdma_init_one_block(void *host_addr,
    ram_addr_t offset, ram_addr_t length, void *opaque)
{
    RDMALocalBlocks *rdma_local_ram_blocks = opaque;
    int num_blocks = rdma_local_ram_blocks->num_blocks;

    rdma_local_ram_blocks->block[num_blocks].local_host_addr = host_addr;
    rdma_local_ram_blocks->block[num_blocks].offset = (uint64_t)offset;
    rdma_local_ram_blocks->block[num_blocks].length = (uint64_t)length;
    rdma_local_ram_blocks->num_blocks++;
}

static void qemu_rdma_ram_block_counter(void *host_addr,
    ram_addr_t offset, ram_addr_t length, void *opaque)
{
    int *num_blocks = opaque;
    *num_blocks = *num_blocks + 1;
}

/*
 * Identify the RAMBlocks and their quantity. They will be references to
 * identify chunk boundaries inside each RAMBlock and also be referenced
 * during dynamic page registration.
 */
static int qemu_rdma_init_ram_blocks(RDMALocalBlocks *rdma_local_ram_blocks)
{
    int num_blocks = 0;

    qemu_ram_foreach_block(qemu_rdma_ram_block_counter, &num_blocks);

    memset(rdma_local_ram_blocks, 0, sizeof *rdma_local_ram_blocks);
    rdma_local_ram_blocks->block = g_malloc0(sizeof(RDMALocalBlock) *
                                    num_blocks);

    rdma_local_ram_blocks->num_blocks = 0;
    qemu_ram_foreach_block(qemu_rdma_init_one_block, rdma_local_ram_blocks);

    DPRINTF("Allocated %d local ram block structures\n",
                    rdma_local_ram_blocks->num_blocks);
    return 0;
}

/*
 * Put in the log file which RDMA device was opened and the details
 * associated with that device.
 */
static void qemu_rdma_dump_id(const char *who, struct ibv_context *verbs)
{
    printf("%s RDMA Device opened: kernel name %s "
           "uverbs device name %s, "
           "infiniband_verbs class device path %s,"
           " infiniband class device path %s\n",
                who,
                verbs->device->name,
                verbs->device->dev_name,
                verbs->device->dev_path,
                verbs->device->ibdev_path);
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
 * Figure out which RDMA device corresponds to the requested IP hostname
 * Also create the initial connection manager identifiers for opening
 * the connection.
 */
static int qemu_rdma_resolve_host(RDMAContext *rdma)
{
    int ret;
    struct addrinfo *res;
    char port_str[16];
    struct rdma_cm_event *cm_event;
    char ip[40] = "unknown";

    if (rdma->host == NULL || !strcmp(rdma->host, "")) {
        fprintf(stderr, "RDMA hostname has not been set\n");
        return -1;
    }

    /* create CM channel */
    rdma->channel = rdma_create_event_channel();
    if (!rdma->channel) {
        fprintf(stderr, "could not create CM channel\n");
        return -1;
    }

    /* create CM id */
    ret = rdma_create_id(rdma->channel, &rdma->cm_id, NULL, RDMA_PS_TCP);
    if (ret) {
        fprintf(stderr, "could not create channel id\n");
        goto err_resolve_create_id;
    }

    snprintf(port_str, 16, "%d", rdma->port);
    port_str[15] = '\0';

    ret = getaddrinfo(rdma->host, port_str, NULL, &res);
    if (ret < 0) {
        fprintf(stderr, "could not getaddrinfo destination address %s\n",
                        rdma->host);
        goto err_resolve_get_addr;
    }

    inet_ntop(AF_INET, &((struct sockaddr_in *) res->ai_addr)->sin_addr,
                                ip, sizeof ip);
    printf("%s => %s\n", rdma->host, ip);

    /* resolve the first address */
    ret = rdma_resolve_addr(rdma->cm_id, NULL, res->ai_addr,
            RDMA_RESOLVE_TIMEOUT_MS);
    if (ret) {
        fprintf(stderr, "could not resolve address %s\n", rdma->host);
        goto err_resolve_get_addr;
    }

    qemu_rdma_dump_gid("client_resolve_addr", rdma->cm_id);

    ret = rdma_get_cm_event(rdma->channel, &cm_event);
    if (ret) {
        fprintf(stderr, "could not perform event_addr_resolved\n");
        goto err_resolve_get_addr;
    }

    if (cm_event->event != RDMA_CM_EVENT_ADDR_RESOLVED) {
        fprintf(stderr, "result not equal to event_addr_resolved %s\n",
                rdma_event_str(cm_event->event));
        perror("rdma_resolve_addr");
        rdma_ack_cm_event(cm_event);
        goto err_resolve_get_addr;
    }
    rdma_ack_cm_event(cm_event);

    /* resolve route */
    ret = rdma_resolve_route(rdma->cm_id, RDMA_RESOLVE_TIMEOUT_MS);
    if (ret) {
        fprintf(stderr, "could not resolve rdma route\n");
        goto err_resolve_get_addr;
    }

    ret = rdma_get_cm_event(rdma->channel, &cm_event);
    if (ret) {
        fprintf(stderr, "could not perform event_route_resolved\n");
        goto err_resolve_get_addr;
    }
    if (cm_event->event != RDMA_CM_EVENT_ROUTE_RESOLVED) {
        fprintf(stderr, "result not equal to event_route_resolved: %s\n",
                        rdma_event_str(cm_event->event));
        rdma_ack_cm_event(cm_event);
        goto err_resolve_get_addr;
    }
    rdma_ack_cm_event(cm_event);
    rdma->verbs = rdma->cm_id->verbs;
    qemu_rdma_dump_id("client_resolve_host", rdma->cm_id->verbs);
    qemu_rdma_dump_gid("client_resolve_host", rdma->cm_id);
    return 0;

err_resolve_get_addr:
    rdma_destroy_id(rdma->cm_id);
err_resolve_create_id:
    rdma_destroy_event_channel(rdma->channel);
    rdma->channel = NULL;

    return -1;
}

/*
 * Create protection domain and completion queues
 */
static int qemu_rdma_alloc_pd_cq(RDMAContext *rdma)
{
    /* allocate pd */
    rdma->pd = ibv_alloc_pd(rdma->verbs);
    if (!rdma->pd) {
        return -1;
    }

#ifdef RDMA_BLOCKING
    /* create completion channel */
    rdma->comp_channel = ibv_create_comp_channel(rdma->verbs);
    if (!rdma->comp_channel) {
        goto err_alloc_pd_cq;
    }
#endif

    /* create cq */
    rdma->cq = ibv_create_cq(rdma->verbs, RDMA_CQ_SIZE,
            NULL, rdma->comp_channel, 0);
    if (!rdma->cq) {
        goto err_alloc_pd_cq;
    }

    return 0;

err_alloc_pd_cq:
    if (rdma->pd) {
        ibv_dealloc_pd(rdma->pd);
    }
    if (rdma->comp_channel) {
        ibv_destroy_comp_channel(rdma->comp_channel);
    }
    rdma->pd = NULL;
    rdma->comp_channel = NULL;
    return -1;

}

/*
 * Create queue pairs.
 */
static int qemu_rdma_alloc_qp(RDMAContext *rdma)
{
    struct ibv_qp_init_attr attr = { 0 };
    int ret;

    attr.cap.max_send_wr = RDMA_QP_SIZE;
    attr.cap.max_recv_wr = 3;
    attr.cap.max_send_sge = 1;
    attr.cap.max_recv_sge = 1;
    attr.send_cq = rdma->cq;
    attr.recv_cq = rdma->cq;
    attr.qp_type = IBV_QPT_RC;

    ret = rdma_create_qp(rdma->cm_id, rdma->pd, &attr);
    if (ret) {
        return -1;
    }

    rdma->qp = rdma->cm_id->qp;
    return 0;
}

static int qemu_rdma_get_fd(void *opaque)
{
    return -2;
}

/*
 * This is probably dead code, but its here anyway for testing.
 * Sometimes nice to know the performance tradeoffs of pinning.
 */
#if !defined(RDMA_LAZY_CLIENT_REGISTRATION)
static int qemu_rdma_reg_chunk_ram_blocks(RDMAContext *rdma,
        RDMALocalBlocks *rdma_local_ram_blocks)
{
    int i, j;
    for (i = 0; i < rdma_local_ram_blocks->num_blocks; i++) {
        RDMALocalBlock *block = &(rdma_local_ram_blocks->block[i]);
        int num_chunks = RDMA_REG_NUM_CHUNKS(block);
        /* allocate memory to store chunk MRs */
        rdma_local_ram_blocks->block[i].pmr = g_malloc0(
                                num_chunks * sizeof(struct ibv_mr *));

        if (!block->pmr) {
            goto err_reg_chunk_ram_blocks;
        }

        for (j = 0; j < num_chunks; j++) {
            uint8_t *start_addr = RDMA_REG_CHUNK_START(block, j);
            uint8_t *end_addr = RDMA_REG_CHUNK_END(block, j);
            if (start_addr < block->local_host_addr) {
                start_addr = block->local_host_addr;
            }
            if (end_addr > block->local_host_addr + block->length) {
                end_addr = block->local_host_addr + block->length;
            }
            block->pmr[j] = ibv_reg_mr(rdma->pd,
                                start_addr,
                                end_addr - start_addr,
                                IBV_ACCESS_REMOTE_READ
                                );
            if (!block->pmr[j]) {
                break;
            }
        }
        if (j < num_chunks) {
            for (j--; j >= 0; j--) {
                ibv_dereg_mr(block->pmr[j]);
            }
            block->pmr[i] = NULL;
            goto err_reg_chunk_ram_blocks;
        }
    }

    return 0;

err_reg_chunk_ram_blocks:
    for (i--; i >= 0; i--) {
        int num_chunks =
            RDMA_REG_NUM_CHUNKS(&(rdma_local_ram_blocks->block[i]));
        for (j = 0; j < num_chunks; j++) {
            ibv_dereg_mr(rdma_local_ram_blocks->block[i].pmr[j]);
        }
        free(rdma_local_ram_blocks->block[i].pmr);
        rdma_local_ram_blocks->block[i].pmr = NULL;
    }

    return -1;

}
#endif

/*
 * Also probably dead code, but for the same reason, its nice
 * to know the performance tradeoffs of dynamic registration
 * on both sides of the connection.
 */
static int qemu_rdma_reg_whole_ram_blocks(RDMAContext *rdma,
                                RDMALocalBlocks *rdma_local_ram_blocks)
{
    int i;
    for (i = 0; i < rdma_local_ram_blocks->num_blocks; i++) {
        rdma_local_ram_blocks->block[i].mr =
            ibv_reg_mr(rdma->pd,
                    rdma_local_ram_blocks->block[i].local_host_addr,
                    rdma_local_ram_blocks->block[i].length,
                    IBV_ACCESS_LOCAL_WRITE |
                    IBV_ACCESS_REMOTE_WRITE
                    );
        if (!rdma_local_ram_blocks->block[i].mr) {
            fprintf(stderr, "Failed to register local server ram block!\n");
            break;
        }
    }

    if (i >= rdma_local_ram_blocks->num_blocks) {
        return 0;
    }

    for (i--; i >= 0; i--) {
        ibv_dereg_mr(rdma_local_ram_blocks->block[i].mr);
    }

    return -1;

}

static int qemu_rdma_client_reg_ram_blocks(RDMAContext *rdma,
                                RDMALocalBlocks *rdma_local_ram_blocks)
{
#ifdef RDMA_CHUNK_REGISTRATION
#ifdef RDMA_LAZY_CLIENT_REGISTRATION
    return 0;
#else
    return qemu_rdma_reg_chunk_ram_blocks(rdma, rdma_local_ram_blocks);
#endif
#else
    return qemu_rdma_reg_whole_ram_blocks(rdma, rdma_local_ram_blocks);
#endif
}

static int qemu_rdma_server_reg_ram_blocks(RDMAContext *rdma,
                                RDMALocalBlocks *rdma_local_ram_blocks)
{
    return qemu_rdma_reg_whole_ram_blocks(rdma, rdma_local_ram_blocks);
}

/*
 * Shutdown and clean things up.
 */
static void qemu_rdma_dereg_ram_blocks(RDMALocalBlocks *rdma_local_ram_blocks)
{
    int i, j;
    for (i = 0; i < rdma_local_ram_blocks->num_blocks; i++) {
        int num_chunks;
        if (!rdma_local_ram_blocks->block[i].pmr) {
            continue;
        }
        num_chunks = RDMA_REG_NUM_CHUNKS(&(rdma_local_ram_blocks->block[i]));
        for (j = 0; j < num_chunks; j++) {
            if (!rdma_local_ram_blocks->block[i].pmr[j]) {
                continue;
            }
            ibv_dereg_mr(rdma_local_ram_blocks->block[i].pmr[j]);
        }
        free(rdma_local_ram_blocks->block[i].pmr);
        rdma_local_ram_blocks->block[i].pmr = NULL;
    }
    for (i = 0; i < rdma_local_ram_blocks->num_blocks; i++) {
        if (!rdma_local_ram_blocks->block[i].mr) {
            continue;
        }
        ibv_dereg_mr(rdma_local_ram_blocks->block[i].mr);
        rdma_local_ram_blocks->block[i].mr = NULL;
    }
}

/*
 * Server uses this to prepare to transmit the RAMBlock descriptions
 * to the primary VM after connection setup.
 * Both sides use the "remote" structure to communicate and update
 * their "local" descriptions with what was sent.
 */
static void qemu_rdma_copy_to_remote_ram_blocks(RDMAContext *rdma,
                                                RDMALocalBlocks *local,
                                                RDMARemoteBlocks *remote)
{
    int i;
    DPRINTF("Allocating %d remote ram block structures\n", local->num_blocks);
    *remote->num_blocks = local->num_blocks;

    for (i = 0; i < local->num_blocks; i++) {
            remote->block[i].remote_host_addr =
                (uint64_t)(local->block[i].local_host_addr);

            if (rdma->chunk_register_destination == false) {
                remote->block[i].remote_rkey = local->block[i].mr->rkey;
            }

            remote->block[i].offset = local->block[i].offset;
            remote->block[i].length = local->block[i].length;
    }
}

/*
 * Client then propogates the remote ram block descriptions to his local copy.
 * Really, only the virtual addresses are useful, but we propogate everything
 * anyway.
 *
 * If we're using dynamic registration on the server side (the default), then
 * the 'rkeys' are not useful because we will re-ask for them later during
 * runtime.
 */
static int qemu_rdma_process_remote_ram_blocks(RDMALocalBlocks *local,
                                               RDMARemoteBlocks *remote)
{
    int i, j;

    if (local->num_blocks != *remote->num_blocks) {
        fprintf(stderr, "local %d != remote %d\n",
            local->num_blocks, *remote->num_blocks);
        return -1;
    }

    for (i = 0; i < *remote->num_blocks; i++) {
        /* search local ram blocks */
        for (j = 0; j < local->num_blocks; j++) {
            if (remote->block[i].offset != local->block[j].offset) {
                continue;
            }
            if (remote->block[i].length != local->block[j].length) {
                return -1;
            }
            local->block[j].remote_host_addr =
                remote->block[i].remote_host_addr;
            local->block[j].remote_rkey = remote->block[i].remote_rkey;
            break;
        }
        if (j >= local->num_blocks) {
            return -1;
        }
    }

    return 0;
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
static int qemu_rdma_search_ram_block(uint64_t offset, uint64_t length,
        RDMALocalBlocks *blocks, int *block_index, int *chunk_index)
{
    int i;
    for (i = 0; i < blocks->num_blocks; i++) {
        if (offset < blocks->block[i].offset) {
            continue;
        }
        if (offset + length >
                blocks->block[i].offset + blocks->block[i].length) {
            continue;
        }
        *block_index = i;
        if (chunk_index) {
            uint8_t *host_addr = blocks->block[i].local_host_addr +
                (offset - blocks->block[i].offset);
            *chunk_index = RDMA_REG_CHUNK_INDEX(
                    blocks->block[i].local_host_addr, host_addr);
        }
        return 0;
    }
    return -1;
}

/*
 * Register a chunk with IB. If the chunk was already registered
 * previously, then skip.
 *
 * Also return the keys associated with the registration needed
 * to perform the actual RDMA operation.
 */
static int qemu_rdma_register_and_get_keys(RDMAContext *rdma,
        RDMALocalBlock *block, uint64_t host_addr,
        uint32_t *lkey, uint32_t *rkey)
{
    int chunk;
    if (block->mr) {
        if (lkey) {
            *lkey = block->mr->lkey;
        }
        if (rkey) {
            *rkey = block->mr->rkey;
        }
        return 0;
    }

    /* allocate memory to store chunk MRs */
    if (!block->pmr) {
        int num_chunks = RDMA_REG_NUM_CHUNKS(block);
        block->pmr = g_malloc0(num_chunks *
                sizeof(struct ibv_mr *));
        if (!block->pmr) {
            return -1;
        }
    }

    /*
     * If 'rkey', then we're the server performing a dynamic
     * registration, so grant access to the client.
     *
     * If 'lkey', then we're the primary VM performing a dynamic
     * registration, so grant access only to ourselves.
     */
    chunk = RDMA_REG_CHUNK_INDEX(block->local_host_addr, host_addr);
    if (!block->pmr[chunk]) {
        uint8_t *start_addr = RDMA_REG_CHUNK_START(block, chunk);
        uint8_t *end_addr = RDMA_REG_CHUNK_END(block, chunk);
        if (start_addr < block->local_host_addr) {
            start_addr = block->local_host_addr;
        }
        if (end_addr > block->local_host_addr + block->length) {
            end_addr = block->local_host_addr + block->length;
        }
        block->pmr[chunk] = ibv_reg_mr(rdma->pd,
                start_addr,
                end_addr - start_addr,
                (rkey ? (IBV_ACCESS_LOCAL_WRITE |
                            IBV_ACCESS_REMOTE_WRITE) : 0)
                | IBV_ACCESS_REMOTE_READ);
        if (!block->pmr[chunk]) {
            fprintf(stderr, "Failed to register chunk!\n");
            return -1;
        }
    }

    if (lkey) {
        *lkey = block->pmr[chunk]->lkey;
    }
    if (rkey) {
        *rkey = block->pmr[chunk]->rkey;
    }
    return 0;
}

/*
 * Register (at connection time) the memory used for control
 * channel messages.
 */
static int qemu_rdma_reg_control(RDMAContext *rdma, int idx)
{
    rdma->wr_data[idx].control_mr = ibv_reg_mr(rdma->pd,
            rdma->wr_data[idx].control, RDMA_CONTROL_MAX_BUFFER,
            IBV_ACCESS_LOCAL_WRITE |
            IBV_ACCESS_REMOTE_WRITE |
            IBV_ACCESS_REMOTE_READ);
    if (rdma->wr_data[idx].control_mr) {
        return 0;
    }
    return -1;
}

static int qemu_rdma_dereg_control(RDMAContext *rdma, int idx)
{
    return ibv_dereg_mr(rdma->wr_data[idx].control_mr);
}

#if defined(DEBUG_RDMA) || defined(DEBUG_RDMA_VERBOSE)
static const char *print_wrid(int wrid)
{
    if (wrid >= RDMA_WRID_RECV_CONTROL) {
        return wrid_desc[RDMA_WRID_RECV_CONTROL];
    }
    return wrid_desc[wrid];
}
#endif

/*
 * Consult the connection manager to see a work request
 * (of any kind) has completed.
 * Return the work request ID that completed.
 */
static int qemu_rdma_poll(RDMAContext *rdma)
{
    int ret;
    struct ibv_wc wc;

    ret = ibv_poll_cq(rdma->cq, 1, &wc);
    if (!ret) {
        return RDMA_WRID_NONE;
    }
    if (ret < 0) {
        fprintf(stderr, "ibv_poll_cq return %d!\n", ret);
        return ret;
    }
    if (wc.status != IBV_WC_SUCCESS) {
        fprintf(stderr, "ibv_poll_cq wc.status=%d %s!\n",
                        wc.status, ibv_wc_status_str(wc.status));
        fprintf(stderr, "ibv_poll_cq wrid=%s!\n", wrid_desc[wc.wr_id]);

        return -1;
    }

    if (rdma->control_ready_expected &&
        (wc.wr_id >= RDMA_WRID_RECV_CONTROL)) {
        DPRINTF("completion %s #%" PRId64 " received (%" PRId64 ")\n",
            wrid_desc[RDMA_WRID_RECV_CONTROL], wc.wr_id -
            RDMA_WRID_RECV_CONTROL, wc.wr_id);
        rdma->control_ready_expected = 0;
    }

    if (wc.wr_id == RDMA_WRID_RDMA_WRITE) {
        rdma->num_signaled_send--;
        DPRINTF("completions %s (%" PRId64 ") left %d\n",
            print_wrid(wc.wr_id), wc.wr_id, rdma->num_signaled_send);
    } else {
        DPRINTF("other completion %s (%" PRId64 ") received left %d\n",
            print_wrid(wc.wr_id), wc.wr_id, rdma->num_signaled_send);
    }

    return  (int)wc.wr_id;
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
#ifdef RDMA_BLOCKING
static int qemu_rdma_block_for_wrid(RDMAContext *rdma, int wrid)
{
    int num_cq_events = 0;
    int r = RDMA_WRID_NONE;
    struct ibv_cq *cq;
    void *cq_ctx;

    if (ibv_req_notify_cq(rdma->cq, 0)) {
        return -1;
    }
    /* poll cq first */
    while (r != wrid) {
        r = qemu_rdma_poll(rdma);
        if (r < 0) {
            return r;
        }
        if (r == RDMA_WRID_NONE) {
            break;
        }
        if (r != wrid) {
            DPRINTF("A Wanted wrid %s (%d) but got %s (%d)\n",
                print_wrid(wrid), wrid, print_wrid(r), r);
        }
    }
    if (r == wrid) {
        return 0;
    }

    while (1) {
        if (ibv_get_cq_event(rdma->comp_channel, &cq, &cq_ctx)) {
            goto err_block_for_wrid;
        }
        num_cq_events++;
        if (ibv_req_notify_cq(cq, 0)) {
            goto err_block_for_wrid;
        }
        /* poll cq */
        while (r != wrid) {
            r = qemu_rdma_poll(rdma);
            if (r < 0) {
                goto err_block_for_wrid;
            }
            if (r == RDMA_WRID_NONE) {
                break;
            }
            if (r != wrid) {
                DPRINTF("B Wanted wrid %s (%d) but got %s (%d)\n",
                    print_wrid(wrid), wrid, print_wrid(r), r);
            }
        }
        if (r == wrid) {
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
    return -1;
}
#else
static int qemu_rdma_poll_for_wrid(RDMAContext *rdma, int wrid)
{
    int r = RDMA_WRID_NONE;
    while (r != wrid) {
        r = qemu_rdma_poll(rdma);
        if (r < 0) {
            return r;
        }
    }
    return 0;
}
#endif


static int wait_for_wrid(RDMAContext *rdma, int wrid)
{
#ifdef RDMA_BLOCKING
    return qemu_rdma_block_for_wrid(rdma, wrid);
#else
    return qemu_rdma_poll_for_wrid(rdma, wrid);
#endif
}

static void control_to_network(RDMAControlHeader *control)
{
    control->version = htonl(control->version);
    control->type = htonl(control->type);
    control->len = htonl(control->len);
    control->repeat = htonl(control->repeat);
}

static void network_to_control(RDMAControlHeader *control)
{
    control->version = ntohl(control->version);
    control->type = ntohl(control->type);
    control->len = ntohl(control->len);
    control->repeat = ntohl(control->repeat);
}

/*
 * Post a SEND message work request for the control channel
 * containing some data and block until the post completes.
 */
static int qemu_rdma_post_send_control(RDMAContext *rdma, uint8_t *buf,
                                       RDMAControlHeader *head)
{
    int ret = 0;
    RDMAWorkRequestData *wr = &rdma->wr_data[RDMA_CONTROL_MAX_WR];
    struct ibv_send_wr *bad_wr;
    struct ibv_sge sge = {
                           .addr = (uint64_t)(wr->control),
                           .length = head->len + RDMAControlHeaderSize,
                           .lkey = wr->control_mr->lkey,
                         };
    struct ibv_send_wr send_wr = {
                                   .wr_id = RDMA_WRID_SEND_CONTROL,
                                   .opcode = IBV_WR_SEND,
                                   .send_flags = IBV_SEND_SIGNALED,
                                   .sg_list = &sge,
                                   .num_sge = 1,
                                };

    if (head->version < RDMA_CONTROL_VERSION_MIN ||
            head->version > RDMA_CONTROL_VERSION_MAX) {
        fprintf(stderr, "SEND: Invalid control message version: %d,"
                        " min: %d, max: %d\n",
                        head->version, RDMA_CONTROL_VERSION_MIN,
                        RDMA_CONTROL_VERSION_MAX);
        return -1;
    }

    DPRINTF("CONTROL: sending %s..\n", control_desc[head->type]);

    /*
     * We don't actually need to do a memcpy() in here if we used
     * the "sge" properly, but since we're only sending control messages
     * (not RAM in a performance-critical path), then its OK for now.
     *
     * The copy makes the RDMAControlHeader simpler to manipulate
     * for the time being.
     */
    memcpy(wr->control, head, RDMAControlHeaderSize);
    control_to_network((void *) wr->control);

    if (buf) {
        memcpy(wr->control + RDMAControlHeaderSize, buf, head->len);
    }


    if (ibv_post_send(rdma->qp, &send_wr, &bad_wr)) {
        return -1;
    }

    if (ret < 0) {
        fprintf(stderr, "Failed to use post IB SEND for control!\n");
        return ret;
    }

    ret = wait_for_wrid(rdma, RDMA_WRID_SEND_CONTROL);
    if (ret < 0) {
        fprintf(stderr, "rdma migration: polling control error!");
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


    if (ibv_post_recv(rdma->qp, &recv_wr, &bad_wr)) {
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
    int ret = wait_for_wrid(rdma, RDMA_WRID_RECV_CONTROL + idx);

    if (ret < 0) {
        fprintf(stderr, "rdma migration: polling control error!\n");
        return ret;
    }

    network_to_control((void *) rdma->wr_data[idx].control);
    memcpy(head, rdma->wr_data[idx].control, RDMAControlHeaderSize);

    if (head->version < RDMA_CONTROL_VERSION_MIN ||
            head->version > RDMA_CONTROL_VERSION_MAX) {
        fprintf(stderr, "RECV: Invalid control message version: %d,"
                        " min: %d, max: %d\n",
                        head->version, RDMA_CONTROL_VERSION_MIN,
                        RDMA_CONTROL_VERSION_MAX);
        return -1;
    }

    DPRINTF("CONTROL: %s received\n", control_desc[expecting]);

    if (expecting != RDMA_CONTROL_NONE && head->type != expecting) {
        fprintf(stderr, "Was expecting a %s (%d) control message"
                ", but got: %s (%d), length: %d\n",
                control_desc[expecting], expecting,
                control_desc[head->type], head->type, head->len);
        return -EIO;
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
        rdma->wr_data[idx].control + RDMAControlHeaderSize;
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
                                   int *resp_idx)
{
    int ret = 0;
    int idx = 0;

    /*
     * Wait until the server is ready before attempting to deliver the message
     * by waiting for a READY message.
     */
    if (rdma->control_ready_expected) {
        RDMAControlHeader resp;
        ret = qemu_rdma_exchange_get_response(rdma,
                                    &resp, RDMA_CONTROL_READY, idx);
        if (ret < 0) {
            return ret;
        }
    }

    /*
     * If the user is expecting a response, post a WR in anticipation of it.
     */
    if (resp) {
        ret = qemu_rdma_post_recv_control(rdma, idx + 1);
        if (ret) {
            fprintf(stderr, "rdma migration: error posting"
                    " extra control recv for anticipated result!");
            return ret;
        }
    }

    /*
     * Post a WR to replace the one we just consumed for the READY message.
     */
    ret = qemu_rdma_post_recv_control(rdma, idx);
    if (ret) {
        fprintf(stderr, "rdma migration: error posting first control recv!");
        return ret;
    }

    /*
     * Deliver the control message that was requested.
     */
    ret = qemu_rdma_post_send_control(rdma, data, head);

    if (ret < 0) {
        fprintf(stderr, "Failed to send control buffer!\n");
        return ret;
    }

    /*
     * If we're expecting a response, block and wait for it.
     */
    if (resp) {
        DPRINTF("Waiting for response %s\n", control_desc[resp->type]);
        ret = qemu_rdma_exchange_get_response(rdma, resp, resp->type, idx + 1);

        if (ret < 0) {
            return ret;
        }

        qemu_rdma_move_header(rdma, idx + 1, resp);
        *resp_idx = idx + 1;
        DPRINTF("Response %s received.\n", control_desc[resp->type]);
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
                                .version = RDMA_CONTROL_CURRENT_VERSION,
                                .repeat = 1,
                              };
    int ret;
    int idx = 0;

    /*
     * Inform the client that we're ready to receive a message.
     */
    ret = qemu_rdma_post_send_control(rdma, NULL, &ready);

    if (ret < 0) {
        fprintf(stderr, "Failed to send control buffer!\n");
        return ret;
    }

    /*
     * Block and wait for the message.
     */
    ret = qemu_rdma_exchange_get_response(rdma, head, expecting, idx);

    if (ret < 0) {
        return ret;
    }

    qemu_rdma_move_header(rdma, idx, head);

    /*
     * Post a new RECV work request to replace the one we just consumed.
     */
    ret = qemu_rdma_post_recv_control(rdma, idx);
    if (ret) {
        fprintf(stderr, "rdma migration: error posting second control recv!");
        return ret;
    }

    return 0;
}

/*
 * Write an actual chunk of memory using RDMA.
 *
 * If we're using dynamic registration on the server-side, we have to
 * send a registration command first.
 */
static int __qemu_rdma_write(QEMUFile *f, RDMAContext *rdma,
        int current_index,
        uint64_t offset, uint64_t length,
        uint64_t wr_id, enum ibv_send_flags flag)
{
    struct ibv_sge sge;
    struct ibv_send_wr send_wr = { 0 };
    struct ibv_send_wr *bad_wr;
    RDMALocalBlock *block = &(local_ram_blocks.block[current_index]);
    int chunk;
    RDMARegister reg;
    RDMARegisterResult *reg_result;
    int reg_result_idx;
    RDMAControlHeader resp = { .type = RDMA_CONTROL_REGISTER_RESULT };
    RDMAControlHeader head = { .len = sizeof(RDMARegister),
                               .type = RDMA_CONTROL_REGISTER_REQUEST,
                               .version = RDMA_CONTROL_CURRENT_VERSION,
                               .repeat = 1,
                             };
    int ret;

    sge.addr = (uint64_t)(block->local_host_addr + (offset - block->offset));
    sge.length = length;
    if (qemu_rdma_register_and_get_keys(rdma, block, sge.addr,
                                        &sge.lkey, NULL)) {
        fprintf(stderr, "cannot get lkey!\n");
        return -EINVAL;
    }

    send_wr.wr_id = wr_id;
    send_wr.opcode = IBV_WR_RDMA_WRITE;
    send_wr.send_flags = flag;
    send_wr.sg_list = &sge;
    send_wr.num_sge = 1;
    send_wr.wr.rdma.remote_addr = block->remote_host_addr +
        (offset - block->offset);

    if (rdma->chunk_register_destination) {
        chunk = RDMA_REG_CHUNK_INDEX(block->local_host_addr, sge.addr);
        if (!block->remote_keys[chunk]) {
            /*
             * Tell other side to register.
             */
            reg.len = sge.length;
            reg.current_index = current_index;
            reg.offset = offset;

            DPRINTF("Sending registration request chunk %d for %d "
                    "bytes, index: %d, offset: %" PRId64 "...\n",
                    chunk, sge.length, current_index, offset);
            ret = qemu_rdma_exchange_send(rdma, &head, (uint8_t *) &reg,
                                    &resp, &reg_result_idx);
            if (ret < 0) {
                return ret;
            }

            reg_result = (RDMARegisterResult *)
                    rdma->wr_data[reg_result_idx].control_curr;

            DPRINTF("Received registration result:"
                    " my key: %x their key %x, chunk %d\n",
                    block->remote_keys[chunk], reg_result->rkey, chunk);

            block->remote_keys[chunk] = reg_result->rkey;
        }

        send_wr.wr.rdma.rkey = block->remote_keys[chunk];
    } else {
        send_wr.wr.rdma.rkey = block->remote_rkey;
    }

    return ibv_post_send(rdma->qp, &send_wr, &bad_wr);
}

/*
 * Push out any unwritten RDMA operations.
 *
 * We support sending out multiple chunks at the same time.
 * Not all of them need to get signaled in the completion queue.
 */
static int qemu_rdma_write_flush(QEMUFile *f, RDMAContext *rdma)
{
    int ret;
    enum ibv_send_flags flags = 0;

    if (!rdma->current_length) {
        return 0;
    }
    if (rdma->num_unsignaled_send >=
            RDMA_UNSIGNALED_SEND_MAX) {
        flags = IBV_SEND_SIGNALED;
    }

    while (1) {
        ret = __qemu_rdma_write(f, rdma,
                rdma->current_index,
                rdma->current_offset,
                rdma->current_length,
                RDMA_WRID_RDMA_WRITE, flags);
        if (ret) {
            if (ret == ENOMEM) {
                DPRINTF("send queue is full. wait a little....\n");
                ret = wait_for_wrid(rdma, RDMA_WRID_RDMA_WRITE);
                if (ret < 0) {
                    fprintf(stderr, "rdma migration: failed to make "
                                    "room in full send queue! %d\n", ret);
                    return -EIO;
                }
            } else {
                 fprintf(stderr, "rdma migration: write flush error! %d\n",
                                                                ret);
                 perror("write flush error");
                 return -EIO;
            }
        } else {
                break;
        }
    }

    if (rdma->num_unsignaled_send >=
            RDMA_UNSIGNALED_SEND_MAX) {
        rdma->num_unsignaled_send = 0;
        rdma->num_signaled_send++;
        DPRINTF("signaled total: %d\n", rdma->num_signaled_send);
    } else {
        rdma->num_unsignaled_send++;
    }

    rdma->current_length = 0;
    rdma->current_offset = 0;

    return 0;
}

static inline int qemu_rdma_in_current_block(RDMAContext *rdma,
                uint64_t offset, uint64_t len)
{
    RDMALocalBlock *block =
        &(local_ram_blocks.block[rdma->current_index]);
    if (rdma->current_index < 0) {
        return 0;
    }
    if (offset < block->offset) {
        return 0;
    }
    if (offset + len > block->offset + block->length) {
        return 0;
    }
    return 1;
}

static inline int qemu_rdma_in_current_chunk(RDMAContext *rdma,
                uint64_t offset, uint64_t len)
{
    RDMALocalBlock *block = &(local_ram_blocks.block[rdma->current_index]);
    uint8_t *chunk_start, *chunk_end, *host_addr;
    if (rdma->current_chunk < 0) {
        return 0;
    }
    host_addr = block->local_host_addr + (offset - block->offset);
    chunk_start = RDMA_REG_CHUNK_START(block, rdma->current_chunk);
    if (chunk_start < block->local_host_addr) {
        chunk_start = block->local_host_addr;
    }
    if (host_addr < chunk_start) {
        return 0;
    }
    chunk_end = RDMA_REG_CHUNK_END(block, rdma->current_chunk);
    if (chunk_end > chunk_start + block->length) {
        chunk_end = chunk_start + block->length;
    }
    if (host_addr + len > chunk_end) {
        return 0;
    }
    return 1;
}

static inline int qemu_rdma_buffer_mergable(RDMAContext *rdma,
                    uint64_t offset, uint64_t len)
{
    if (rdma->current_length == 0) {
        return 0;
    }
    if (offset != rdma->current_offset + rdma->current_length) {
        return 0;
    }
    if (!qemu_rdma_in_current_block(rdma, offset, len)) {
        return 0;
    }
#ifdef RDMA_CHUNK_REGISTRATION
    if (!qemu_rdma_in_current_chunk(rdma, offset, len)) {
        return 0;
    }
#endif
    return 1;
}

/*
 * We're not actually writing here, but doing three things:
 *
 * 1. Identify the chunk the buffer belongs to.
 * 2. If the chunk is full or the buffer doesn't belong to the current
 *    chunk, then start a new chunk and flush() the old chunk.
 * 3. To keep the hardware busy, we also group chunks into batches
 *    and only require that a batch gets acknowledged in the completion
 *    qeueue instead of each individual chunk.
 */
static int qemu_rdma_write(QEMUFile *f, RDMAContext *rdma,
                           uint64_t offset, uint64_t len)
{
    int index = rdma->current_index;
    int chunk_index = rdma->current_chunk;
    int ret;

    /* If we cannot merge it, we flush the current buffer first. */
    if (!qemu_rdma_buffer_mergable(rdma, offset, len)) {
        ret = qemu_rdma_write_flush(f, rdma);
        if (ret) {
            return ret;
        }
        rdma->current_length = 0;
        rdma->current_offset = offset;

        ret = qemu_rdma_search_ram_block(offset, len,
                    &local_ram_blocks, &index, &chunk_index);
        if (ret) {
            fprintf(stderr, "ram block search failed\n");
            return ret;
        }
        rdma->current_index = index;
        rdma->current_chunk = chunk_index;
    }

    /* merge it */
    rdma->current_length += len;

    /* flush it if buffer is too large */
    if (rdma->current_length >= RDMA_MERGE_MAX) {
        return qemu_rdma_write_flush(f, rdma);
    }

    return 0;
}

static void qemu_rdma_cleanup(RDMAContext *rdma)
{
    struct rdma_cm_event *cm_event;
    int ret, idx;

    if (rdma->cm_id) {
        DPRINTF("Disconnecting...\n");
        ret = rdma_disconnect(rdma->cm_id);
        if (!ret) {
            ret = rdma_get_cm_event(rdma->channel, &cm_event);
            if (!ret) {
                rdma_ack_cm_event(cm_event);
            }
        }
        DPRINTF("Disconnected.\n");
    }

    if (remote_ram_blocks.remote_area) {
        g_free(remote_ram_blocks.remote_area);
    }

    for (idx = 0; idx < (RDMA_CONTROL_MAX_WR + 1); idx++) {
        if (rdma->wr_data[idx].control_mr) {
            qemu_rdma_dereg_control(rdma, idx);
        }
        rdma->wr_data[idx].control_mr = NULL;
    }

    qemu_rdma_dereg_ram_blocks(&local_ram_blocks);

    if (local_ram_blocks.block) {
        if (rdma->chunk_register_destination) {
            for (idx = 0; idx < local_ram_blocks.num_blocks; idx++) {
                RDMALocalBlock *block = &(local_ram_blocks.block[idx]);
                if (block->remote_keys) {
                    g_free(block->remote_keys);
                }
            }
        }
        g_free(local_ram_blocks.block);
    }

    if (rdma->qp) {
        ibv_destroy_qp(rdma->qp);
    }
    if (rdma->cq) {
        ibv_destroy_cq(rdma->cq);
    }
    if (rdma->comp_channel) {
        ibv_destroy_comp_channel(rdma->comp_channel);
    }
    if (rdma->pd) {
        ibv_dealloc_pd(rdma->pd);
    }
    if (rdma->listen_id) {
        rdma_destroy_id(rdma->listen_id);
    }
    if (rdma->cm_id) {
        rdma_destroy_id(rdma->cm_id);
        rdma->cm_id = 0;
    }
    if (rdma->channel) {
        rdma_destroy_event_channel(rdma->channel);
    }
}

static void qemu_rdma_remote_ram_blocks_init(void)
{
    int remote_size = (sizeof(RDMARemoteBlock) *
                        local_ram_blocks.num_blocks)
                        +   sizeof(*remote_ram_blocks.num_blocks);

    DPRINTF("Preparing %d bytes for remote info\n", remote_size);

    remote_ram_blocks.remote_area = g_malloc0(remote_size);
    remote_ram_blocks.remote_size = remote_size;
    remote_ram_blocks.num_blocks = remote_ram_blocks.remote_area;
    remote_ram_blocks.block = (void *) (remote_ram_blocks.num_blocks + 1);
}

static int qemu_rdma_client_init(RDMAContext *rdma, Error **errp,
                                 bool chunk_register_destination)
{
    int ret, idx;

    if (rdma->client_init_done) {
        return 0;
    }

    rdma->chunk_register_destination = chunk_register_destination;

    ret = qemu_rdma_resolve_host(rdma);
    if (ret) {
        fprintf(stderr, "rdma migration: error resolving host!");
        goto err_rdma_client_init;
    }

    ret = qemu_rdma_alloc_pd_cq(rdma);
    if (ret) {
        fprintf(stderr, "rdma migration: error allocating pd and cq!");
        goto err_rdma_client_init;
    }

    ret = qemu_rdma_alloc_qp(rdma);
    if (ret) {
        fprintf(stderr, "rdma migration: error allocating qp!");
        goto err_rdma_client_init;
    }

    ret = qemu_rdma_init_ram_blocks(&local_ram_blocks);
    if (ret) {
        fprintf(stderr, "rdma migration: error initializing ram blocks!");
        goto err_rdma_client_init;
    }

    ret = qemu_rdma_client_reg_ram_blocks(rdma, &local_ram_blocks);
    if (ret) {
        fprintf(stderr, "rdma migration: error client registering ram blocks!");
        goto err_rdma_client_init;
    }

    for (idx = 0; idx < (RDMA_CONTROL_MAX_WR + 1); idx++) {
        ret = qemu_rdma_reg_control(rdma, idx);
        if (ret) {
            fprintf(stderr, "rdma migration: error registering %d control!",
                                                            idx);
            goto err_rdma_client_init;
        }
    }

    qemu_rdma_remote_ram_blocks_init();

    rdma->client_init_done = 1;
    return 0;

err_rdma_client_init:
    qemu_rdma_cleanup(rdma);
    return -1;
}

static void caps_to_network(RDMACapabilities *cap)
{
    cap->version = htonl(cap->version);
    cap->flags = htonl(cap->flags);
}

static void network_to_caps(RDMACapabilities *cap)
{
    cap->version = ntohl(cap->version);
    cap->flags = ntohl(cap->flags);
}

static int qemu_rdma_connect(RDMAContext *rdma, Error **errp)
{
    RDMAControlHeader head;
    RDMACapabilities cap = {
                                .version = RDMA_CONTROL_CURRENT_VERSION,
                                .flags = 0,
                           };
    struct rdma_conn_param conn_param = { .initiator_depth = 2,
                                          .retry_count = 5,
                                          .private_data = &cap,
                                          .private_data_len = sizeof(cap),
                                        };
    struct rdma_cm_event *cm_event;
    int ret;
    int idx = 0;
    int x;

    if (rdma->chunk_register_destination) {
        printf("Server dynamic registration requested.\n");
        cap.flags |= RDMA_CAPABILITY_CHUNK_REGISTER;
    }

    caps_to_network(&cap);

    ret = rdma_connect(rdma->cm_id, &conn_param);
    if (ret) {
        perror("rdma_connect");
        fprintf(stderr, "rdma migration: error connecting!");
        rdma_destroy_id(rdma->cm_id);
        rdma->cm_id = 0;
        goto err_rdma_client_connect;
    }

    ret = rdma_get_cm_event(rdma->channel, &cm_event);
    if (ret) {
        perror("rdma_get_cm_event after rdma_connect");
        fprintf(stderr, "rdma migration: error connecting!");
        rdma_ack_cm_event(cm_event);
        rdma_destroy_id(rdma->cm_id);
        rdma->cm_id = 0;
        goto err_rdma_client_connect;
    }

    if (cm_event->event != RDMA_CM_EVENT_ESTABLISHED) {
        perror("rdma_get_cm_event != EVENT_ESTABLISHED after rdma_connect");
        fprintf(stderr, "rdma migration: error connecting!");
        rdma_ack_cm_event(cm_event);
        rdma_destroy_id(rdma->cm_id);
        rdma->cm_id = 0;
        goto err_rdma_client_connect;
    }

    memcpy(&cap, cm_event->param.conn.private_data, sizeof(cap));
    network_to_caps(&cap);

    /*
     * Verify that the destination can support the capabilities we requested.
     */
    if (!(cap.flags & RDMA_CAPABILITY_CHUNK_REGISTER) &&
        rdma->chunk_register_destination) {
        printf("Server cannot support dynamic registration. Will disable\n");
        rdma->chunk_register_destination = false;
    }

    printf("Chunk registration %s\n",
        rdma->chunk_register_destination ? "enabled" : "disabled");

    rdma_ack_cm_event(cm_event);

    ret = qemu_rdma_post_recv_control(rdma, idx + 1);
    if (ret) {
        fprintf(stderr, "rdma migration: error posting second control recv!");
        goto err_rdma_client_connect;
    }

    ret = qemu_rdma_post_recv_control(rdma, idx);
    if (ret) {
        fprintf(stderr, "rdma migration: error posting second control recv!");
        goto err_rdma_client_connect;
    }

    ret = qemu_rdma_exchange_get_response(rdma,
                                &head, RDMA_CONTROL_RAM_BLOCKS, idx + 1);

    if (ret < 0) {
        fprintf(stderr, "rdma migration: error sending remote info!");
        goto err_rdma_client_connect;
    }

    qemu_rdma_move_header(rdma, idx + 1, &head);
    memcpy(remote_ram_blocks.remote_area, rdma->wr_data[idx + 1].control_curr,
                    remote_ram_blocks.remote_size);

    ret = qemu_rdma_process_remote_ram_blocks(
                            &local_ram_blocks, &remote_ram_blocks);
    if (ret) {
        fprintf(stderr, "rdma migration: error processing"
                        " remote ram blocks!\n");
        goto err_rdma_client_connect;
    }

    if (rdma->chunk_register_destination) {
        for (x = 0; x < local_ram_blocks.num_blocks; x++) {
            RDMALocalBlock *block = &(local_ram_blocks.block[x]);
            int num_chunks = RDMA_REG_NUM_CHUNKS(block);
            /* allocate memory to store remote rkeys */
            block->remote_keys = g_malloc0(num_chunks * sizeof(uint32_t));
        }
    }
    rdma->control_ready_expected = 1;
    rdma->num_signaled_send = 0;
    return 0;

err_rdma_client_connect:
    qemu_rdma_cleanup(rdma);
    return -1;
}

static int qemu_rdma_server_init(RDMAContext *rdma, Error **errp)
{
    int ret, idx;
    struct sockaddr_in sin;
    struct rdma_cm_id *listen_id;
    char ip[40] = "unknown";

    for (idx = 0; idx < RDMA_CONTROL_MAX_WR; idx++) {
        rdma->wr_data[idx].control_len = 0;
        rdma->wr_data[idx].control_curr = NULL;
    }

    if (rdma->host == NULL) {
        fprintf(stderr, "Error: RDMA host is not set!");
        return -1;
    }
    /* create CM channel */
    rdma->channel = rdma_create_event_channel();
    if (!rdma->channel) {
        fprintf(stderr, "Error: could not create rdma event channel");
        return -1;
    }

    /* create CM id */
    ret = rdma_create_id(rdma->channel, &listen_id, NULL, RDMA_PS_TCP);
    if (ret) {
        fprintf(stderr, "Error: could not create cm_id!");
        goto err_server_init_create_listen_id;
    }

    memset(&sin, 0, sizeof(sin));
    sin.sin_family = AF_INET;
    sin.sin_port = htons(rdma->port);

    if (rdma->host && strcmp("", rdma->host)) {
        struct hostent *server_addr;
        server_addr = gethostbyname(rdma->host);
        if (!server_addr) {
            fprintf(stderr, "Error: migration could not gethostbyname!");
            goto err_server_init_bind_addr;
        }
        memcpy(&sin.sin_addr.s_addr, server_addr->h_addr,
                server_addr->h_length);
        inet_ntop(AF_INET, server_addr->h_addr, ip, sizeof ip);
    } else {
        sin.sin_addr.s_addr = INADDR_ANY;
    }

    DPRINTF("%s => %s\n", rdma->host, ip);

    ret = rdma_bind_addr(listen_id, (struct sockaddr *)&sin);
    if (ret) {
        fprintf(stderr, "Error: could not rdma_bind_addr!");
        goto err_server_init_bind_addr;
    }

    rdma->listen_id = listen_id;
    if (listen_id->verbs) {
        rdma->verbs = listen_id->verbs;
    }
    qemu_rdma_dump_id("server_init", rdma->verbs);
    qemu_rdma_dump_gid("server_init", listen_id);
    return 0;

err_server_init_bind_addr:
    rdma_destroy_id(listen_id);
err_server_init_create_listen_id:
    rdma_destroy_event_channel(rdma->channel);
    rdma->channel = NULL;
    return -1;

}

static int qemu_rdma_server_prepare(RDMAContext *rdma, Error **errp)
{
    int ret;
    int idx;

    if (!rdma->verbs) {
        fprintf(stderr, "rdma migration: no verbs context!");
        return 0;
    }

    ret = qemu_rdma_alloc_pd_cq(rdma);
    if (ret) {
        fprintf(stderr, "rdma migration: error allocating pd and cq!");
        goto err_rdma_server_prepare;
    }

    ret = qemu_rdma_init_ram_blocks(&local_ram_blocks);
    if (ret) {
        fprintf(stderr, "rdma migration: error initializing ram blocks!");
        goto err_rdma_server_prepare;
    }

    qemu_rdma_remote_ram_blocks_init();

    /* Extra one for the send buffer */
    for (idx = 0; idx < (RDMA_CONTROL_MAX_WR + 1); idx++) {
        ret = qemu_rdma_reg_control(rdma, idx);
        if (ret) {
            fprintf(stderr, "rdma migration: error registering %d control!",
                        idx);
            goto err_rdma_server_prepare;
        }
    }

    ret = rdma_listen(rdma->listen_id, 5);
    if (ret) {
        fprintf(stderr, "rdma migration: error listening on socket!");
        goto err_rdma_server_prepare;
    }

    return 0;

err_rdma_server_prepare:
    qemu_rdma_cleanup(rdma);
    return -1;
}

static void *qemu_rdma_data_init(const char *host_port, Error **errp)
{
    RDMAContext *rdma = NULL;
    InetSocketAddress *addr;

    if (host_port) {
        rdma = g_malloc0(sizeof(RDMAContext));
        memset(rdma, 0, sizeof(RDMAContext));
        rdma->current_index = -1;
        rdma->current_chunk = -1;

        addr = inet_parse(host_port, errp);
        if (addr != NULL) {
            rdma->port = atoi(addr->port);
            rdma->host = g_strdup(addr->host);
            printf("rdma host: %s\n", rdma->host);
            printf("rdma port: %d\n", rdma->port);
        } else {
            error_setg(errp, "bad RDMA migration address '%s'", host_port);
            g_free(rdma);
            return NULL;
        }
    }

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

    /*
     * Push out any writes that
     * we're queued up for pc.ram.
     */
    if (qemu_rdma_write_flush(f, rdma) < 0) {
        return -EIO;
    }

    while (remaining) {
        RDMAControlHeader head;

        r->len = MIN(remaining, RDMA_SEND_INCREMENT);
        remaining -= r->len;

        head.len = r->len;
        head.type = RDMA_CONTROL_QEMU_FILE;
        head.version = RDMA_CONTROL_CURRENT_VERSION;

        ret = qemu_rdma_exchange_send(rdma, &head, data, NULL, NULL);

        if (ret < 0) {
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
        DPRINTF("RDMA %" PRId64 " of %d bytes already in buffer\n",
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
static int qemu_rdma_drain_cq(QEMUFile *f, RDMAContext *rdma)
{
    int ret;

    if (qemu_rdma_write_flush(f, rdma) < 0) {
        return -EIO;
    }

    while (rdma->num_signaled_send) {
        ret = wait_for_wrid(rdma, RDMA_WRID_RDMA_WRITE);
        if (ret < 0) {
            fprintf(stderr, "rdma migration: complete polling error!\n");
            return -EIO;
        }
    }

    return 0;
}

static int qemu_rdma_close(void *opaque)
{
    QEMUFileRDMA *r = opaque;
    if (r->rdma) {
        qemu_rdma_cleanup(r->rdma);
        g_free(r->rdma);
    }
    g_free(r);
    return 0;
}

static size_t qemu_rdma_save_page(QEMUFile *f, void *opaque,
                           ram_addr_t block_offset, ram_addr_t offset,
                           size_t size, uint8_t *va)
{
    ram_addr_t current_addr = block_offset + offset;
    QEMUFileRDMA *rfile = opaque;
    RDMAContext *rdma;
    int ret;

    if (rfile) {
        rdma = rfile->rdma;
    } else {
        return -ENOTSUP;
    }

    qemu_ftell(f);

    /*
     * Don't pin zero pages on the destination. Just return.
     */
    if (rdma->chunk_register_destination &&
            (buffer_find_nonzero_offset(va, size) == size)) {
        return size;
    }

    /*
     * Add this page to the current 'chunk'. If the chunk
     * is full, or the page doen't belong to the current chunk,
     * an actual RDMA write will occur and a new chunk will be formed.
     */
    ret = qemu_rdma_write(f, rdma, current_addr, size);
    if (ret < 0) {
        fprintf(stderr, "rdma migration: write error! %d\n", ret);
        return ret;
    }

    /*
     * Drain the Completion Queue if possible, but do not block,
     * just poll.
     *
     * If nothing to poll, the end of the iteration will do this
     * again to make sure we don't overflow the request queue.
     */
    while (1) {
        int ret = qemu_rdma_poll(rdma);
        if (ret == RDMA_WRID_NONE) {
            break;
        }
        if (ret < 0) {
            fprintf(stderr, "rdma migration: polling error! %d\n", ret);
            return ret;
        }
    }

    return size;
}

static int qemu_rdma_accept(RDMAContext *rdma)
{
    RDMAControlHeader head = { .len = remote_ram_blocks.remote_size,
                               .type = RDMA_CONTROL_RAM_BLOCKS,
                               .version = RDMA_CONTROL_CURRENT_VERSION,
                               .repeat = 1,
                             };
    RDMACapabilities cap;
    uint32_t requested_flags;
    struct rdma_conn_param conn_param = {
                                            .responder_resources = 2,
                                            .private_data = &cap,
                                            .private_data_len = sizeof(cap),
                                         };
    struct rdma_cm_event *cm_event;
    struct ibv_context *verbs;
    int ret;

    ret = rdma_get_cm_event(rdma->channel, &cm_event);
    if (ret) {
        goto err_rdma_server_wait;
    }

    if (cm_event->event != RDMA_CM_EVENT_CONNECT_REQUEST) {
        rdma_ack_cm_event(cm_event);
        goto err_rdma_server_wait;
    }

    memcpy(&cap, cm_event->param.conn.private_data, sizeof(cap));

    network_to_caps(&cap);

    if (cap.version < RDMA_CONTROL_VERSION_MIN ||
            cap.version > RDMA_CONTROL_VERSION_MAX) {
            fprintf(stderr, "Unknown client RDMA version: %d, bailing...\n",
                            cap.version);
            rdma_ack_cm_event(cm_event);
            goto err_rdma_server_wait;
    }

    if (cap.version == RDMA_CONTROL_VERSION_1) {
        if (cap.flags & RDMA_CAPABILITY_CHUNK_REGISTER) {
            rdma->chunk_register_destination = true;
        } else if (cap.flags & RDMA_CAPABILITY_NEXT_FEATURE) {
            /* handle new capability */
        }
    } else {
        fprintf(stderr, "Unknown client RDMA version: %d, bailing...\n",
                        cap.version);
        rdma_ack_cm_event(cm_event);
        goto err_rdma_server_wait;
    }

    rdma->cm_id = cm_event->id;
    verbs = cm_event->id->verbs;

    rdma_ack_cm_event(cm_event);

    /*
     * Respond to client with the capabilities we agreed to support.
     */
    requested_flags = cap.flags;
    cap.flags = 0;

    if (rdma->chunk_register_destination &&
        (requested_flags & RDMA_CAPABILITY_CHUNK_REGISTER)) {
        cap.flags |= RDMA_CAPABILITY_CHUNK_REGISTER;
    }

    printf("Chunk registration %s\n",
        rdma->chunk_register_destination ? "enabled" : "disabled");

    caps_to_network(&cap);

    DPRINTF("verbs context after listen: %p\n", verbs);

    if (!rdma->verbs) {
        rdma->verbs = verbs;
        ret = qemu_rdma_server_prepare(rdma, NULL);
        if (ret) {
            fprintf(stderr, "rdma migration: error preparing server!\n");
            goto err_rdma_server_wait;
        }
    } else if (rdma->verbs != verbs) {
            fprintf(stderr, "ibv context not matching %p, %p!\n",
                    rdma->verbs, verbs);
            goto err_rdma_server_wait;
    }

    /* xxx destroy listen_id ??? */

    qemu_set_fd_handler2(rdma->channel->fd, NULL, NULL, NULL, NULL);

    ret = qemu_rdma_alloc_qp(rdma);
    if (ret) {
        fprintf(stderr, "rdma migration: error allocating qp!");
        goto err_rdma_server_wait;
    }

    ret = rdma_accept(rdma->cm_id, &conn_param);
    if (ret) {
        fprintf(stderr, "rdma_accept returns %d!\n", ret);
        goto err_rdma_server_wait;
    }

    ret = rdma_get_cm_event(rdma->channel, &cm_event);
    if (ret) {
        fprintf(stderr, "rdma_accept get_cm_event failed %d!\n", ret);
        goto err_rdma_server_wait;
    }

    if (cm_event->event != RDMA_CM_EVENT_ESTABLISHED) {
        fprintf(stderr, "rdma_accept not event established!\n");
        rdma_ack_cm_event(cm_event);
        goto err_rdma_server_wait;
    }

    rdma_ack_cm_event(cm_event);

    ret = qemu_rdma_post_recv_control(rdma, 0);
    if (ret) {
        fprintf(stderr, "rdma migration: error posting second control recv!");
        goto err_rdma_server_wait;
    }

    if (rdma->chunk_register_destination == false) {
        ret = qemu_rdma_server_reg_ram_blocks(rdma, &local_ram_blocks);
        if (ret) {
            fprintf(stderr, "rdma migration: error server "
                            "registering ram blocks!");
            goto err_rdma_server_wait;
        }
    }

    qemu_rdma_copy_to_remote_ram_blocks(rdma,
            &local_ram_blocks, &remote_ram_blocks);

    ret = qemu_rdma_post_send_control(rdma,
            (uint8_t *) remote_ram_blocks.remote_area, &head);

    if (ret < 0) {
        fprintf(stderr, "rdma migration: error sending remote info!");
        goto err_rdma_server_wait;
    }

    qemu_rdma_dump_gid("server_connect", rdma->cm_id);

    return 0;

err_rdma_server_wait:
    qemu_rdma_cleanup(rdma);
    return ret;
}

/*
 * During each iteration of the migration, we listen for instructions
 * by the primary VM to perform dynamic page registrations before they
 * can perform RDMA operations.
 *
 * We respond with the 'rkey'.
 *
 * Keep doing this until the primary tells us to stop.
 */
static int qemu_rdma_registration_handle(QEMUFile *f, void *opaque,
                                         uint32_t flags)
{
    RDMAControlHeader resp = { .len = sizeof(RDMARegisterResult),
                               .type = RDMA_CONTROL_REGISTER_RESULT,
                               .version = RDMA_CONTROL_CURRENT_VERSION,
                               .repeat = 0,
                             };
    QEMUFileRDMA *rfile = opaque;
    RDMAContext *rdma = rfile->rdma;
    RDMAControlHeader head;
    RDMARegister *reg, *registers;
    RDMARegisterResult *reg_result;
    static RDMARegisterResult results[RDMA_CONTROL_MAX_COMMANDS_PER_MESSAGE];
    RDMALocalBlock *block;
    uint64_t host_addr;
    int ret = 0;
    int idx = 0;
    int count = 0;

    DPRINTF("Waiting for next registration %d...\n", flags);

    do {
        ret = qemu_rdma_exchange_recv(rdma, &head, RDMA_CONTROL_NONE);

        if (ret < 0) {
            break;
        }

        switch (head.type) {
        case RDMA_CONTROL_REGISTER_FINISHED:
                DPRINTF("Current registrations complete.\n");
                goto out;
        case RDMA_CONTROL_REGISTER_REQUEST:
                if (head.repeat > RDMA_CONTROL_MAX_COMMANDS_PER_MESSAGE) {
                    printf("Too many registration requests (%d). Bailing.\n",
                        head.repeat);
                    ret = -EIO;
                    goto out;
                }

                DPRINTF("There are %d registration requests\n", head.repeat);

                resp.repeat = head.repeat;
                registers = (RDMARegister *) rdma->wr_data[idx].control_curr;

                for (count = 0; count < head.repeat; count++) {
                    reg = &registers[count];
                    reg_result = &results[count];

                    DPRINTF("Registration request (%d): %d"
                        " bytes, index %d, offset %" PRId64 "\n",
                        count, reg->len, reg->current_index, reg->offset);

                    block = &(local_ram_blocks.block[reg->current_index]);
                    host_addr = (uint64_t)(block->local_host_addr +
                                (reg->offset - block->offset));
                    if (qemu_rdma_register_and_get_keys(rdma, block,
                                host_addr, NULL, &reg_result->rkey)) {
                        fprintf(stderr, "cannot get rkey!\n");
                        ret = -EINVAL;
                        goto out;
                    }

                    DPRINTF("Registered rkey for this request: %x\n",
                                    reg_result->rkey);
                }

                ret = qemu_rdma_post_send_control(rdma,
                                (uint8_t *) results, &resp);

                if (ret < 0) {
                    fprintf(stderr, "Failed to send control buffer!\n");
                    goto out;
                }
                break;
        case RDMA_CONTROL_REGISTER_RESULT:
                fprintf(stderr, "Invalid RESULT message at server.\n");
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
    return ret;
}

static int qemu_rdma_registration_start(QEMUFile *f, void *opaque,
                                        uint32_t flags)
{
    DPRINTF("start section: %d\n", flags);
    qemu_put_be64(f, RAM_SAVE_FLAG_HOOK);
    return 0;
}

/*
 * Inform server that dynamic registrations are done for now.
 * First, flush writes, if any.
 */
static int qemu_rdma_registration_stop(QEMUFile *f, void *opaque,
                                       uint32_t flags)
{
    QEMUFileRDMA *rfile = opaque;
    RDMAContext *rdma = rfile->rdma;
    RDMAControlHeader head = { .len = 0,
                               .type = RDMA_CONTROL_REGISTER_FINISHED,
                               .version = RDMA_CONTROL_CURRENT_VERSION,
                               .repeat = 1,
                             };
    int ret = qemu_rdma_drain_cq(f, rdma);

    if (ret >= 0) {
        DPRINTF("Sending registration finish %d...\n", flags);

        ret = qemu_rdma_exchange_send(rdma, &head, NULL, NULL, NULL);
    }

    return ret;
}

const QEMUFileOps rdma_read_ops = {
    .get_buffer    = qemu_rdma_get_buffer,
    .close         = qemu_rdma_close,
    .get_fd        = qemu_rdma_get_fd,
    .hook_ram_load = qemu_rdma_registration_handle,
};

const QEMUFileOps rdma_write_ops = {
    .put_buffer           = qemu_rdma_put_buffer,
    .close                = qemu_rdma_close,
    .get_fd               = qemu_rdma_get_fd,
    .before_ram_iterate   = qemu_rdma_registration_start,
    .after_ram_iterate    = qemu_rdma_registration_stop,
    .save_page            = qemu_rdma_save_page,
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

static void rdma_accept_incoming_migration(void *opaque)
{
    RDMAContext *rdma = opaque;
    int ret;
    QEMUFile *f;

    DPRINTF("Accepting rdma connection...\n");
    ret = qemu_rdma_accept(rdma);
    if (ret) {
        fprintf(stderr, "RDMA Migration initialization failed!\n");
        goto err;
    }

    DPRINTF("Accepted migration\n");

    f = qemu_fopen_rdma(rdma, "rb");
    if (f == NULL) {
        fprintf(stderr, "could not qemu_fopen_rdma!\n");
        goto err;
    }

    process_incoming_migration(f);
    return;

err:
    qemu_rdma_cleanup(rdma);
}

void rdma_start_incoming_migration(const char *host_port, Error **errp)
{
    int ret;
    RDMAContext *rdma;

    DPRINTF("Starting RDMA-based incoming migration\n");
    rdma = qemu_rdma_data_init(host_port, errp);
    if (rdma == NULL) {
        return;
    }

    ret = qemu_rdma_server_init(rdma, NULL);

    if (!ret) {
        DPRINTF("qemu_rdma_server_init success\n");
        ret = qemu_rdma_server_prepare(rdma, NULL);

        if (!ret) {
            DPRINTF("qemu_rdma_server_prepare success\n");

            qemu_set_fd_handler2(rdma->channel->fd, NULL,
                                 rdma_accept_incoming_migration, NULL,
                                    (void *)(intptr_t) rdma);
            return;
        }
    }

    g_free(rdma);
}

void rdma_start_outgoing_migration(void *opaque,
                            const char *host_port, Error **errp)
{
    MigrationState *s = opaque;
    RDMAContext *rdma = NULL;
    int ret;

    rdma = qemu_rdma_data_init(host_port, errp);
    if (rdma == NULL) {
        goto err;
    }

    ret = qemu_rdma_client_init(rdma, NULL,
        s->enabled_capabilities[MIGRATION_CAPABILITY_CHUNK_REGISTER_DESTINATION]);

    if (!ret) {
        DPRINTF("qemu_rdma_client_init success\n");
        ret = qemu_rdma_connect(rdma, NULL);

        if (!ret) {
            DPRINTF("qemu_rdma_client_connect success\n");
            s->file = qemu_fopen_rdma(rdma, "wb");
            migrate_fd_connect(s);
            return;
        }
    }
err:
    g_free(rdma);
    migrate_fd_error(s);
    error_setg(errp, "Error connecting using rdma! %d\n", ret);
}

