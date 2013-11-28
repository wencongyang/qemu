/*
 *  Copyright (C) 2014 Michael R. Hines <mrhines@us.ibm.com>
 *
 *  Micro-Checkpointing (MC) support 
 *  (a.k.a. Fault Tolerance or Continuous Replication)
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
#include <libnl3/netlink/route/qdisc/plug.h>
#include <libnl3/netlink/route/class.h>
#include <libnl3/netlink/cli/utils.h>
#include <libnl3/netlink/cli/tc.h>
#include <libnl3/netlink/cli/qdisc.h>
#include <libnl3/netlink/cli/link.h>
#include "qemu-common.h"
#include "hw/virtio/virtio.h"
#include "hw/virtio/virtio-net.h"
#include "qemu/sockets.h"
#include "migration/migration.h"
#include "migration/qemu-file.h"
#include "qmp-commands.h"
#include "net/tap-linux.h"
#include <sys/ioctl.h>

#define DEBUG_MC
//#define DEBUG_MC_VERBOSE
//#define DEBUG_MC_REALLY_VERBOSE

#ifdef DEBUG_MC
#define DPRINTF(fmt, ...) \
    do { printf("mc: " fmt, ## __VA_ARGS__); } while (0)
#else
#define DPRINTF(fmt, ...) \
    do { } while (0)
#endif

#ifdef DEBUG_MC_VERBOSE
#define DDPRINTF(fmt, ...) \
    do { printf("mc: " fmt, ## __VA_ARGS__); } while (0)
#else
#define DDPRINTF(fmt, ...) \
    do { } while (0)
#endif

#ifdef DEBUG_MC_REALLY_VERBOSE
#define DDDPRINTF(fmt, ...) \
    do { printf("mc: " fmt, ## __VA_ARGS__); } while (0)
#else
#define DDDPRINTF(fmt, ...) \
    do { } while (0)
#endif

#define MBPS(bytes, time) time ? ((((double) bytes * 8)         \
        / ((double) time / 1000.0)) / 1000.0 / 1000.0) : -1.0

/*
 * Micro checkpoints (MC)s are typically only a few MB when idle.
 * However, they can easily be very large during heavy workloads.
 * In the *extreme* worst-case, QEMU might need double the amount of main memory
 * than that of what was originally allocated to the virtual machine.
 *
 * To support this variability during transient periods, a MC
 * consists of a linked list of slabs, each of identical size. A better name
 * would be welcome, as the name was only chosen because it resembles linux
 * memory allocation. Because MCs occur several times per second 
 * (a frequency of 10s of milliseconds), slabs allow MCs to grow and shrink 
 * without constantly re-allocating all memory in place during each checkpoint.
 *
 * During steady-state, the 'head' slab is permanently allocated and never goes
 * away, so when the VM is idle, there is no memory allocation at all.
 * This design supports the use of RDMA. Since RDMA requires memory pinning, we
 * must be able to hold on to a slab for a reasonable amount of time to get any
 * real use out of it.
 *
 * Regardless, the current strategy taken is:
 * 
 * 1. If the checkpoint size increases,
 *    then grow the number of slabs to support it.
 * 2. If the next checkpoint size is smaller than the last one,
      then that's a "strike".
 * 3. After N strikes, cut the size of the slab cache in half
 *    (to a minimum of 1 slab as described before).
 *
 * As of this writing, a typical average size of 
 * an Idle-VM checkpoint is under 5MB.
 */

#define MC_SLAB_BUFFER_SIZE     (5UL * 1024UL * 1024UL) /* empirical */
#define MC_DEV_NAME_MAX_SIZE    256

#define MC_DEFAULT_CHECKPOINT_FREQ_MS 100 /* too slow, but best for now */
#define CALC_MAX_STRIKES()                                           \
    do {  max_strikes = (max_strikes_delay_secs * 1000) / freq_ms; } \
    while (0)

/*
 * How many "seconds-worth" of checkpoints to wait before re-evaluating the size
 * of the slab cache?
 *
 * #strikes_until_shrink_cache = Function(#checkpoints/sec)
 *
 * Increasing the number of seconds, increases the number of strikes needed to
 * be reached until it is time to cut the cache in half.
 *
 * Below value is open for debate - we just want it to be small enough to ensure
 * that a large, idle cache doesn't stay too large for too long.
 */
#define MC_DEFAULT_SLAB_MAX_CHECK_DELAY_SECS 10

/* 
 * MC serializes the actual RAM page contents in such a way that the actual
 * pages are separated from the meta-data (all the QEMUFile stuff).
 *
 * This is done strictly for the purposes of being able to use RDMA
 * to replace memcpy() on the local machine.
 * 
 * This serialization requires recording the page descriptions and then
 * pushing them into slabs after the checkpoint has been captured
 * (minus the page data).
 *
 * The memory holding the page descriptions are allocated in unison with the
 * slabs themselves, and thus we need to know in advance the maximum number of
 * page descriptions that can fit into a slab before allocating the slab.
 * It should be safe to assume the *minimum* page size (not the maximum,
 * that would be dangerous) is 4096.
 *
 * We're not actually using this assumption for any memory management 
 * management, only as a hint to know how big of an array to allocate.
 *
 * The following adds a fixed-cost of about 40 KB to each slab.
 */
#define MC_MAX_SLAB_COPY_DESCRIPTORS (MC_SLAB_BUFFER_SIZE / 4096)

#define SLAB_RESET(s) do {                      \
                            s->size = 0;      \
                            s->read = 0;      \
                      } while(0)

uint64_t freq_ms = MC_DEFAULT_CHECKPOINT_FREQ_MS;
uint32_t max_strikes_delay_secs = MC_DEFAULT_SLAB_MAX_CHECK_DELAY_SECS;
uint32_t max_strikes = -1;

typedef struct QEMU_PACKED MCCopy {
    uint64_t ramblock_offset;
    uint64_t host_addr;
    uint64_t offset;
    uint64_t size;
} MCCopy;

typedef struct QEMU_PACKED MCCopyset {
    QTAILQ_ENTRY(MCCopyset) node;
    MCCopy copies[MC_MAX_SLAB_COPY_DESCRIPTORS];
    uint64_t nb_copies;
    int idx;
} MCCopyset;

typedef struct QEMU_PACKED MCSlab {
    QTAILQ_ENTRY(MCSlab) node;
    uint8_t buf[MC_SLAB_BUFFER_SIZE];
    uint64_t read;
    uint64_t size;
    int idx;
} MCSlab;

typedef struct MCParams {
    QTAILQ_HEAD(shead, MCSlab) slab_head;
    QTAILQ_HEAD(chead, MCCopyset) copy_head;
    MCSlab *curr_slab;
    MCSlab *mem_slab;
    MCCopyset *curr_copyset;
    MCCopy *copy;
    QEMUFile *file;
    QEMUFile *staging;
    uint64_t start_copyset;
    uint64_t slab_total;
    uint64_t total_copies;
    uint64_t nb_slabs;
    uint64_t used_slabs;
    uint32_t slab_strikes;
    uint32_t copy_strikes;
    int nb_copysets;
    uint64_t checkpoints;
} MCParams;

enum {
    MC_TRANSACTION_NACK = 300,
    MC_TRANSACTION_START,
    MC_TRANSACTION_COMMIT,
    MC_TRANSACTION_ABORT,
    MC_TRANSACTION_ACK,
    MC_TRANSACTION_END,
    MC_TRANSACTION_ANY,
};

static const char * mc_desc[] = {
    [MC_TRANSACTION_NACK] = "NACK",
    [MC_TRANSACTION_START] = "START",
    [MC_TRANSACTION_COMMIT] = "COMMIT",
    [MC_TRANSACTION_ABORT] = "ABORT",
    [MC_TRANSACTION_ACK] = "ACK",
    [MC_TRANSACTION_END] = "END",
    [MC_TRANSACTION_ANY] = "ANY",
};

static struct rtnl_qdisc        *qdisc      = NULL;
static struct nl_sock           *sock       = NULL;
static struct rtnl_tc           *tc         = NULL;
static struct nl_cache          *link_cache = NULL;
static struct rtnl_tc_ops       *ops        = NULL;
static struct nl_cli_tc_module  *tm         = NULL;
static int first_nic_chosen = 0;

/*
 * Assuming a guest can 'try' to fill a 1 Gbps pipe,
 * that works about to 125000000 bytes/sec.
 *
 * Netlink better not be pre-allocating megabytes in the
 * kernel qdisc, that would be crazy....
 */
#define START_BUFFER (1000*1000*1000 / 8)
static int buffer_size = START_BUFFER, new_buffer_size = START_BUFFER;
static const char * parent = "root";
static int buffering_enabled = 0;
static const char * BUFFER_NIC_PREFIX = "ifb";
static QEMUBH *checkpoint_bh = NULL;
static bool mc_requested = false;

int migrate_use_mc(void)
{
    MigrationState *s = migrate_get_current();
    return s->enabled_capabilities[MIGRATION_CAPABILITY_MC];
}

int migrate_use_mc_net(void)
{
    MigrationState *s = migrate_get_current();
    return !s->enabled_capabilities[MIGRATION_CAPABILITY_MC_NET_DISABLE];
}

int migrate_use_mc_rdma_copy(void)
{
    MigrationState *s = migrate_get_current();
    return s->enabled_capabilities[MIGRATION_CAPABILITY_MC_RDMA_COPY];
}

static int mc_deliver(int update)
{
    int err, flags = NLM_F_CREATE;

    if (!buffering_enabled)
        return -EINVAL;

    if (!update)
        flags |= NLM_F_EXCL;
 
    if ((err = rtnl_qdisc_add(sock, qdisc, flags)) < 0) {
        fprintf(stderr, "Unable control qdisc: %s! %p %p %d\n",
            nl_geterror(err), sock, qdisc, flags);
        return -EINVAL;
    }

    return 0;
}

static int mc_set_buffer_size(int size)
{
    int err;

    if (!buffering_enabled) {
        return 1;
    }

    buffer_size = size;
    new_buffer_size = size;

    if ((err = rtnl_qdisc_plug_set_limit((void *) qdisc, size)) < 0) {
       fprintf(stderr, "MC: Unable to change buffer size: %s\n",
			nl_geterror(err));
       return -EINVAL;
    }

    DPRINTF("Set buffer size to %d bytes\n", size);

    return mc_deliver(1);
}

/*
 * Micro-checkpointing may require buffering network packets.
 * Set that up for the first NIC only....
 */
static void init_mc_nic_buffering(NICState *nic, void *opaque)
{
    char * device = opaque;
    NetClientState * nc = &nic->ncs[0];
    const char * key = "ifname=";
    int keylen = strlen(key);
    char * name;
    int end = 0;
    bool use_fd = false;
   
    if (first_nic_chosen) {
         fprintf(stderr, "Micro-Checkpointing with multiple NICs not yet supported!\n");
         return;
    }

    if (!nc->peer) {
        fprintf(stderr, "Micro-Checkpoint nic %s does not have peer host device for buffering. VM will not be consistent.\n", nc->name);
        return;
    }

    name = nc->peer->info_str;

    DPRINTF("Checking contents of %s\n", name);

    if (strncmp(name, key, keylen)) {
        fprintf(stderr, "Micro-Checkpoint nic %s does not have 'ifname' "
                        "in its description (%s, %s). Trying workaround...\n",
                        nc->name, name, nc->peer->name);
        key = "fd=";
        keylen = strlen(key);
        if (strncmp(name, key, keylen)) {
            fprintf(stderr, "Still cannot find 'fd=' either. Failure.\n");
            return;
        }

        use_fd = true;
    }

    name += keylen;

    while (name[end++] != (use_fd ? '\0' : ','));

    strncpy(device, name, end - 1);
    memset(&device[end - 1], 0, MC_DEV_NAME_MAX_SIZE - (end - 1));

    if (use_fd) {
        struct ifreq r;
        DPRINTF("Want to retreive name from fd: %d\n", atoi(device));

        if (ioctl(atoi(device), TUNGETIFF, &r) == -1) {
            fprintf(stderr, "Failed to convert fd %s to name.\n", device);
            return;
        }

        DPRINTF("Got name %s!\n", r.ifr_name);
        strcpy(device, r.ifr_name);
    }

    first_nic_chosen = 1;
}

static int mc_suspend_buffering(void)
{
    int err;

    if (!buffering_enabled) {
        return -EINVAL;
    }

    if ((err = rtnl_qdisc_plug_release_indefinite((void *) qdisc)) < 0) {
        fprintf(stderr, "MC: Unable to release indefinite: %s\n",
            nl_geterror(err));
        return -EINVAL;
    }

    DPRINTF("Buffering suspended\n");

    return mc_deliver(1);
}

static int mc_disable_buffering(void)
{
    int err;

    if (!buffering_enabled) {
		goto out;
	}

    mc_suspend_buffering();

    if (qdisc && sock && (err = rtnl_qdisc_delete(sock, (void *) qdisc)) < 0) {
        fprintf(stderr, "Unable to release indefinite: %s\n", nl_geterror(err));
    }

out:
    buffering_enabled = 0;
    qdisc = NULL;
    sock = NULL;
    tc = NULL;
    link_cache = NULL;
    ops = NULL;
    tm = NULL;

    DPRINTF("Buffering disabled\n");

    return 0;
}

/*
 * Install a Qdisc plug for micro-checkpointing.
 * If it exists already (say, from a previous dead VM or debugging
 * session) then just open all the netlink data structures pointing
 * to the existing plug so that we can continue to manipulate it.
 */
int mc_enable_buffering(void)
{
    char dev[MC_DEV_NAME_MAX_SIZE], buffer_dev[MC_DEV_NAME_MAX_SIZE];
    int prefix_len = 0;
    int buffer_prefix_len = strlen(BUFFER_NIC_PREFIX);

    if (buffering_enabled) {
        fprintf(stderr, "Buffering already enable Skipping.\n");
        return 0;
    }

    first_nic_chosen = 0;

    qemu_foreach_nic(init_mc_nic_buffering, dev);

    if (!first_nic_chosen) {
        fprintf(stderr, "Enumeration of NICs complete, but failed.\n");
        goto failed;
    }

    while ((dev[prefix_len] < '0') || (dev[prefix_len] > '9'))
        prefix_len++;

    strcpy(buffer_dev, BUFFER_NIC_PREFIX);
    strncpy(buffer_dev + buffer_prefix_len,
                dev + prefix_len, strlen(dev) - prefix_len + 1);

    fprintf(stderr, "Initializing buffering for nic %s => %s\n", dev, buffer_dev);

    if (sock == NULL) {
        sock = (struct nl_sock *) nl_cli_alloc_socket();
        if (!sock) {
            fprintf(stderr, "MC: failed to allocate netlink socket\n");
            goto failed;
        }
		nl_cli_connect(sock, NETLINK_ROUTE);
    }

    if (qdisc == NULL) {
        qdisc = nl_cli_qdisc_alloc();
        if (!qdisc) {
            fprintf(stderr, "MC: failed to allocate netlink qdisc\n");
            goto failed;
        }
        tc = (struct rtnl_tc *) qdisc;
    }

    if (link_cache == NULL) {
		link_cache = nl_cli_link_alloc_cache(sock);
        if (!link_cache) {
            fprintf(stderr, "MC: failed to allocate netlink link_cache\n");
            goto failed;
        }
    }

    nl_cli_tc_parse_dev(tc, link_cache, (char *) buffer_dev);
    nl_cli_tc_parse_parent(tc, (char *) parent);

    if (!rtnl_tc_get_ifindex(tc)) {
        fprintf(stderr, "Qdisc device '%s' does not exist!\n", buffer_dev);
        goto failed;
    }

    if (!rtnl_tc_get_parent(tc)) {
        fprintf(stderr, "Qdisc parent '%s' is not valid!\n", parent);
        goto failed;
    }

    if (rtnl_tc_set_kind(tc, "plug") < 0) {
        fprintf(stderr, "Could not open qdisc plug!\n");
        goto failed;
    }

    if (!(ops = rtnl_tc_get_ops(tc))) {
        fprintf(stderr, "Could not open qdisc plug!\n");
        goto failed;
    }

    if (!(tm = nl_cli_tc_lookup(ops))) {
        fprintf(stderr, "Qdisc plug not supported!\n");
        goto failed;
    }
   
    buffering_enabled = 1;

    if (mc_deliver(0) < 0) {
		fprintf(stderr, "First time qdisc create failed\n");
		goto failed;
    }

    DPRINTF("Buffering enabled, size: %d MB.\n", buffer_size / 1024 / 1024);
  
    if (mc_set_buffer_size(buffer_size) < 0) {
		goto failed;
	}

    if (mc_suspend_buffering() < 0) {
		goto failed;
	}


    return 0;

failed:
    mc_disable_buffering();
    return -EINVAL;
}

int mc_start_buffer(void)
{
    int err;

    if (!buffering_enabled) {
        return -EINVAL;
    }

    if (new_buffer_size != buffer_size) {
        buffer_size = new_buffer_size;
        fprintf(stderr, "GDB setting new buffer size to %d\n", buffer_size);
        if (mc_set_buffer_size(buffer_size) < 0)
            return -EINVAL;
    }

    if ((err = rtnl_qdisc_plug_buffer((void *) qdisc)) < 0) {
        fprintf(stderr, "Unable to flush oldest checkpoint: %s\n", nl_geterror(err));
        return -EINVAL;
    }

    DDPRINTF("Inserted checkpoint barrier\n");

    return mc_deliver(1);
}

static int mc_flush_oldest_buffer(void)
{
    int err;

    if (!buffering_enabled)
        return -EINVAL;

    if ((err = rtnl_qdisc_plug_release_one((void *) qdisc)) < 0) {
        fprintf(stderr, "Unable to flush oldest checkpoint: %s\n", nl_geterror(err));
        return -EINVAL;
    }

    DDPRINTF("Flushed oldest checkpoint barrier\n");

    return mc_deliver(1);
}

/*
 * Get the next slab in the list. If there is none, then make one.
 */
static MCSlab *mc_slab_next(MCParams *mc, MCSlab *slab)
{
    if (!QTAILQ_NEXT(slab, node)) {
        int idx = mc->nb_slabs++;
        mc->used_slabs++;
        DDPRINTF("Extending slabs by one: %" PRIu64 " slabs total, "
                 "%" PRIu64 " MB\n", mc->nb_slabs,
                 mc->nb_slabs * sizeof(MCSlab) / 1024UL / 1024UL);
        mc->curr_slab = qemu_memalign(4096, sizeof(MCSlab));
        memset(mc->curr_slab, 0, sizeof(*(mc->curr_slab)));
        mc->curr_slab->idx = idx;
        QTAILQ_INSERT_TAIL(&mc->slab_head, mc->curr_slab, node);
        slab = mc->curr_slab;
        ram_control_add(mc->file, slab->buf, 
                (uint64_t) slab->buf, MC_SLAB_BUFFER_SIZE);
    } else {
        slab = QTAILQ_NEXT(slab, node);
        mc->used_slabs++;
    }

    mc->curr_slab = slab;
    SLAB_RESET(slab);

    if (slab->idx == mc->start_copyset) {
        DDPRINTF("Found copyset slab @ idx %d\n", slab->idx);
        mc->mem_slab = slab;
    }

    return slab;
}

static int mc_put_buffer(void *opaque, const uint8_t *buf,
                                  int64_t pos, int size)
{
    MCParams *mc = opaque;
    MCSlab *slab = mc->curr_slab;
    uint64_t len = size;

    assert(slab);

    while (len) {
        long put = MIN(MC_SLAB_BUFFER_SIZE - slab->size, len);

        if (put == 0) {
            DDPRINTF("Reached the end of slab %d Need a new one\n", slab->idx);
            goto zero;
        }

        if (mc->copy && migrate_use_mc_rdma_copy()) {
            int ret = ram_control_copy_page(mc->file, 
                                        (uint64_t) slab->buf,
                                        slab->size,
                                        (ram_addr_t) mc->copy->ramblock_offset,
                                        (ram_addr_t) mc->copy->offset,
                                        put);

            DDDPRINTF("Attempted offloaded memcpy.\n");

            if (ret != RAM_COPY_CONTROL_NOT_SUPP) {
                if (ret == RAM_COPY_CONTROL_DELAYED) {
                    DDDPRINTF("Offloaded memcpy successful.\n"); 
                    mc->copy->offset += put;
                    goto next;
                } else {
                    fprintf(stderr, "Offloaded memcpy failed: %d\n", ret);
                    return ret;
                }
            }
        }

        DDDPRINTF("Copying to %p from %p, size %" PRId64 "\n",
                 slab->buf + slab->size, buf, put);

        memcpy(slab->buf + slab->size, buf, put);
next:

        buf            += put;
        slab->size     += put;
        len            -= put;
        mc->slab_total += put;

        DDDPRINTF("put: %" PRIu64 " len: %" PRIu64
                  " total %" PRIu64 " size: %" PRIu64 
                  " slab %d\n",
                  put, len, mc->slab_total, slab->size,
                  slab->idx);
zero:
        if (len) {
            slab = mc_slab_next(mc, slab);
        }
    }

    return size;
}

/*
 * Stop the VM, generate the micro checkpoint,
 * but save the dirty memory into staging memory
 * (buffered_file will sit on it) until
 * we can re-activate the VM as soon as possible.
 */
static int capture_checkpoint(MCParams *mc, MigrationState *s)
{
    MCCopyset *copyset;
    int idx, ret = 0;
    uint64_t start, stop, copies = 0;
    int64_t start_time;

    mc->total_copies = 0;
    qemu_mutex_lock_iothread();
    vm_stop_force_state(RUN_STATE_CHECKPOINT_VM);
    start = qemu_clock_get_ms(QEMU_CLOCK_REALTIME);

    /*
     * If buffering is enabled, insert a Qdisc plug here
     * to hold packets for the *next* MC, (not this one,
     * the packets for this one have already been plugged
     * and will be released after the MC has been transmitted.
     */
    mc_start_buffer();

    qemu_savevm_state_begin(mc->staging, &s->params);
    ret = qemu_file_get_error(s->file);

    if (ret < 0) {
        migrate_set_state(s, MIG_STATE_MC, MIG_STATE_ERROR);
    }

    qemu_savevm_state_complete(mc->staging);

    ret = qemu_file_get_error(s->file);
    if (ret < 0) {
        migrate_set_state(s, MIG_STATE_MC, MIG_STATE_ERROR);
        goto out;
    }

    /*
     * The copied memory gets appended to the end of the snapshot, so let's
     * remember where its going to go first and start a new slab.
     */
    mc_slab_next(mc, mc->curr_slab);
    mc->start_copyset = mc->curr_slab->idx;

    start_time = qemu_clock_get_ms(QEMU_CLOCK_REALTIME);

    /*
     * Now perform the actual copy of memory into the tail end of the slab list. 
     */
    QTAILQ_FOREACH(copyset, &mc->copy_head, node) {
        if (!copyset->nb_copies) {
            break;
        }

        copies += copyset->nb_copies;

        DDDPRINTF("copyset %d copies: %" PRIu64 " total: %" PRIu64 "\n",
                copyset->idx, copyset->nb_copies, copies);

        for (idx = 0; idx < copyset->nb_copies; idx++) {
            uint8_t *addr;
            long size;
            mc->copy = &copyset->copies[idx];
            addr = (uint8_t *) (mc->copy->host_addr + mc->copy->offset);
            size = mc_put_buffer(mc, addr, mc->copy->offset, mc->copy->size);
            if (size != mc->copy->size) {
                fprintf(stderr, "Failure to initiate copyset %d index %d\n",
                        copyset->idx, idx);
                migrate_set_state(s, MIG_STATE_MC, MIG_STATE_ERROR);
                vm_start();
                goto out;
            }

            DDDPRINTF("Success copyset %d index %d\n", copyset->idx, idx);
        }

        copyset->nb_copies = 0;
    }

    s->ram_copy_time = (qemu_clock_get_ms(QEMU_CLOCK_REALTIME) - start_time);

    mc->copy = NULL;
    ram_control_before_iterate(mc->file, RAM_CONTROL_FLUSH); 
    assert(mc->total_copies == copies);

    stop = qemu_clock_get_ms(QEMU_CLOCK_REALTIME);

    /*
     * MC is safe in staging area. Let the VM go.
     */
    vm_start();
    qemu_fflush(mc->staging);

    s->downtime = stop - start;
out:
    qemu_mutex_unlock_iothread();
    return ret;
}

/*
 * Synchronously send a micro-checkpointing command
 */
static int mc_send(QEMUFile *f, uint64_t request)
{
    int ret = 0;

    qemu_put_be64(f, request);

    ret = qemu_file_get_error(f);
    if (ret) {
        fprintf(stderr, "transaction: send error while sending %" PRIu64 ", "
                "bailing: %s\n", request, strerror(-ret));
    } else {
        DDPRINTF("transaction: sent: %s (%" PRIu64 ")\n", 
            mc_desc[request], request);
    }

    qemu_fflush(f);

    return ret;
}

/*
 * Synchronously receive a micro-checkpointing command
 */
static int mc_recv(QEMUFile *f, uint64_t request, uint64_t *action)
{
    int ret = 0;
    uint64_t got;

    got = qemu_get_be64(f);

    ret = qemu_file_get_error(f);
    if (ret) {
        fprintf(stderr, "transaction: recv error while expecting %s (%"
                PRIu64 "), bailing: %s\n", mc_desc[request], 
                request, strerror(-ret));
    } else {
        if ((request != MC_TRANSACTION_ANY) && request != got) {
            fprintf(stderr, "transaction: was expecting %s (%" PRIu64 
                    ") but got %" PRIu64 " instead\n",
                    mc_desc[request], request, got);
            ret = -EINVAL;
        } else {
            DDPRINTF("transaction: recv: %s (%" PRIu64 ")\n", 
                     mc_desc[got], got);
            ret = 0;
            if (action) {
                *action = got;
            }
        }
    }

    return ret;
}

static int migrate_use_bitworkers(void)
{
    MigrationState *s = migrate_get_current();
    return s->enabled_capabilities[MIGRATION_CAPABILITY_BITWORKERS];
}

static MCSlab *mc_slab_start(MCParams *mc)
{
    if (mc->nb_slabs > 2) {
        if (mc->slab_strikes >= max_strikes) {
            uint64_t nb_slabs_to_free = MAX(1, (((mc->nb_slabs - 1) / 2)));

            DPRINTF("MC has reached max strikes. Will free %" 
                    PRIu64 " / %" PRIu64 " slabs max %d, "
                    "checkpoints %" PRIu64 "\n",
                    nb_slabs_to_free, mc->nb_slabs,
                    max_strikes, mc->checkpoints);

            mc->slab_strikes = 0;

            while (nb_slabs_to_free) {
                MCSlab *slab = QTAILQ_LAST(&mc->slab_head, shead);
                ram_control_remove(mc->file, (uint64_t) slab->buf);
                QTAILQ_REMOVE(&mc->slab_head, slab, node);
                g_free(slab);
                nb_slabs_to_free--;
                mc->nb_slabs--;
            }

            goto skip;
        } else if (((mc->slab_total <= 
                    ((mc->nb_slabs - 1) * MC_SLAB_BUFFER_SIZE)))) {
            mc->slab_strikes++;
            DDPRINTF("MC has strike %d slabs %" PRIu64 " max %d\n", 
                     mc->slab_strikes, mc->nb_slabs, max_strikes);
            goto skip;
        }
    }

    if (mc->slab_strikes) {
        DDPRINTF("MC used all slabs. Resetting strikes to zero.\n");
        mc->slab_strikes = 0;
    }
skip:

    mc->used_slabs = 1;
    mc->slab_total = 0;
    mc->curr_slab = QTAILQ_FIRST(&mc->slab_head);
    SLAB_RESET(mc->curr_slab);

    return mc->curr_slab;
}

static MCCopyset *mc_copy_start(MCParams *mc)
{
    if (mc->nb_copysets >= 2) {
        if (mc->copy_strikes >= max_strikes) {
            int nb_copies_to_free = MAX(1, (((mc->nb_copysets - 1) / 2)));

            DPRINTF("MC has reached max strikes. Will free %d / %d copies max %d\n",
                    nb_copies_to_free, mc->nb_copysets, max_strikes);

            mc->copy_strikes = 0;

            while (nb_copies_to_free) {
                MCCopyset * copyset = QTAILQ_LAST(&mc->copy_head, chead);
                QTAILQ_REMOVE(&mc->copy_head, copyset, node);
                g_free(copyset);
                nb_copies_to_free--;
                mc->nb_copysets--;
            }

            goto skip;
        } else if (((mc->total_copies <= 
                    ((mc->nb_copysets - 1) * MC_MAX_SLAB_COPY_DESCRIPTORS)))) {
            mc->copy_strikes++;
            DDPRINTF("MC has strike %d copies %d max %d\n", 
                     mc->copy_strikes, mc->nb_copysets, max_strikes);
            goto skip;
        }
    }

    if (mc->copy_strikes) {
        DDPRINTF("MC used all copies. Resetting strikes to zero.\n");
        mc->copy_strikes = 0;
    }
skip:

    mc->total_copies = 0;
    mc->curr_copyset = QTAILQ_FIRST(&mc->copy_head);
    mc->curr_copyset->nb_copies = 0;

    return mc->curr_copyset;
}

/*
 * Main MC loop. Stop the VM, dump the dirty memory
 * into buffered_file, restart the VM, transmit the MC,
 * and then sleep for some milliseconds before
 * starting the next MC.
 */
static void *mc_thread(void *opaque)
{
    MigrationState *s = opaque;
    MCParams mc = { .file = s->file };
    MCSlab * slab;
    int64_t initial_time = qemu_clock_get_ms(QEMU_CLOCK_REALTIME);
    int ret = 0, fd = qemu_get_fd(s->file), x;
    QEMUFile *mc_control, *mc_staging = NULL;
    uint64_t wait_time = 0;
   
    if (migrate_use_bitworkers()) {
        DPRINTF("Starting bitmap workers.\n");
        qemu_mutex_lock_iothread();
        migration_bitmap_worker_start(s);
        qemu_mutex_unlock_iothread();
    }

    if (!(mc_control = qemu_fopen_socket(fd, "rb"))) {
        fprintf(stderr, "Failed to setup read MC control\n");
        goto err;
    }

    if (!(mc_staging = qemu_fopen_mc(&mc, "wb"))) {
        fprintf(stderr, "Failed to setup MC staging area\n");
        goto err;
    }

    mc.staging = mc_staging;

    qemu_set_block(fd);
    socket_set_nodelay(fd);

    s->checkpoints = 0;

    while (s->state == MIG_STATE_MC) {
        int64_t current_time = qemu_clock_get_ms(QEMU_CLOCK_REALTIME);
        int64_t start_time, xmit_start, end_time;
        bool commit_sent = false;
        int nb_slab = 0;
        (void)nb_slab;
        
        slab = mc_slab_start(&mc);
        mc_copy_start(&mc);
        acct_clear();
        start_time = qemu_clock_get_ms(QEMU_CLOCK_REALTIME);

        if (capture_checkpoint(&mc, s) < 0)
                break;

        xmit_start = qemu_clock_get_ms(QEMU_CLOCK_REALTIME);

        if ((ret = mc_send(s->file, MC_TRANSACTION_START) < 0)) {
            fprintf(stderr, "transaction start failed\n");
            break;
        }
        
        DDPRINTF("Sending checkpoint size %" PRId64 
                 " copyset start: %" PRIu64 " nb slab %" PRIu64 
                 " used slabs %" PRIu64 "\n",
                 mc.slab_total, mc.start_copyset, mc.nb_slabs, mc.used_slabs);

        mc.curr_slab = QTAILQ_FIRST(&mc.slab_head);

        qemu_put_be64(s->file, mc.slab_total);
        qemu_put_be64(s->file, mc.start_copyset);
        qemu_put_be64(s->file, mc.used_slabs);

        qemu_fflush(s->file);
       
        DDPRINTF("Transaction commit\n");

        /*
         * The MC is safe, and VM is running again.
         * Start a transaction and send it.
         */
        ram_control_before_iterate(s->file, RAM_CONTROL_ROUND); 

        slab = QTAILQ_FIRST(&mc.slab_head);

        for (x = 0; x < mc.used_slabs; x++) {
            DDPRINTF("Attempting write to slab #%d: %p"
                    " total size: %" PRId64 " / %" PRIu64 "\n",
                    nb_slab++, slab->buf, slab->size, MC_SLAB_BUFFER_SIZE);

            ret = ram_control_save_page(s->file, (uint64_t) slab->buf,
                                        NULL, 0, slab->size, NULL);

            if (ret == RAM_SAVE_CONTROL_NOT_SUPP) {
                if (!commit_sent) {
                    if ((ret = mc_send(s->file, MC_TRANSACTION_COMMIT) < 0)) {
                        fprintf(stderr, "transaction commit failed\n");
                        break;
                    }
                    commit_sent = true;
                }

                qemu_put_be64(s->file, slab->size);
                qemu_put_buffer_async(s->file, slab->buf, slab->size);
            } else if ((ret < 0) && (ret != RAM_SAVE_CONTROL_DELAYED)) {
                fprintf(stderr, "failed 1, skipping send\n");
                goto err;
            }

            if (qemu_file_get_error(s->file)) {
                fprintf(stderr, "failed 2, skipping send\n");
                goto err;
            }
                
            DDPRINTF("Sent %" PRId64 " all %ld\n", slab->size, mc.slab_total);

            slab = QTAILQ_NEXT(slab, node);
        }

        if (!commit_sent) {
            ram_control_after_iterate(s->file, RAM_CONTROL_ROUND); 
            slab = QTAILQ_FIRST(&mc.slab_head);

            for (x = 0; x < mc.used_slabs; x++) {
                qemu_put_be64(s->file, slab->size);
                slab = QTAILQ_NEXT(slab, node);
            }
        }

        qemu_fflush(s->file);

        if (commit_sent) {
            DDPRINTF("Waiting for commit ACK\n");

            if ((ret = mc_recv(mc_control, MC_TRANSACTION_ACK, NULL)) < 0) {
                goto err;
            }
        }

        ret = qemu_file_get_error(s->file);
        if (ret) {
            fprintf(stderr, "Error sending checkpoint: %d\n", ret);
            goto err;
        }

        DDPRINTF("Memory transfer complete.\n");

        /*
         * The MC is safe on the other side now,
         * go along our merry way and release the network
         * packets from the buffer if enable
         */
        mc_flush_oldest_buffer();

        end_time = qemu_clock_get_ms(QEMU_CLOCK_REALTIME);
        s->total_time = end_time - start_time;
        s->xmit_time = end_time - xmit_start;
        s->bitmap_time = norm_mig_bitmap_time();
        s->log_dirty_time = norm_mig_log_dirty_time();
        s->mbps = MBPS(mc.slab_total, s->xmit_time);
        s->copy_mbps = MBPS(mc.slab_total, s->ram_copy_time);
        s->bytes_xfer = mc.slab_total;
        s->checkpoints = mc.checkpoints++;

        wait_time = (s->downtime <= freq_ms) ? (freq_ms - s->downtime) : 0;

        if (current_time >= initial_time + 1000) {
            DPRINTF("bytes %" PRIu64 " xmit_mbps %0.1f xmit_time %" PRId64
                    " downtime %" PRIu64 " sync_time %" PRId64
                    " logdirty_time %" PRId64 " ram_copy_time %" PRId64
                    " copy_mbps %0.1f wait time %" PRIu64
                    " checkpoints %" PRId64 "\n",
                    s->bytes_xfer,
                    s->mbps,
                    s->xmit_time,
                    s->downtime,
                    s->bitmap_time,
                    s->log_dirty_time,
                    s->ram_copy_time,
                    s->copy_mbps,
                    wait_time,
                    s->checkpoints);
            initial_time = current_time;
        }

        /*
         * Checkpoint frequency in microseconds.
         * 
         * Sometimes, when checkpoints are very large,
         * all of the wait time was dominated by the 
         * time taken to copy the checkpoint into the staging area,
         * in which case wait_time, will probably be zero and we
         * will end up diving right back into the next checkpoint
         * as soon as the previous transmission completed.
         */
        if (wait_time) {
            g_usleep(wait_time * 1000);
        }
    }

    goto out;

err:
    /*
     * TODO: Possible split-brain scenario:
     * Normally, this should never be reached unless there was a
     * connection error or network partition - in which case
     * only the management software can resume the VM safely 
     * when it knows the exact state of the MC destination.
     *
     * We need management to poll the source and destination to deterine
     * if the destination has already taken control. If not, then
     * we need to resume the source.
     *
     * If there was a connection error during checkpoint *transmission*
     * then the destination VM will likely have already resumed,
     * in which case we need to stop the current VM from running
     * and throw away any buffered packets.
     * 
     * Verify that "disable_buffering" below does not release any traffic.
     */
    migrate_set_state(s, MIG_STATE_MC, MIG_STATE_ERROR);
out:
    if (mc_staging) {
        qemu_fclose(mc_staging);
    }

    if (mc_control) {
        qemu_fclose(mc_control);
    }

    mc_disable_buffering();

    qemu_mutex_lock_iothread();

    if (migrate_use_bitworkers()) {
        DPRINTF("Stopping bitmap workers.\n");
        migration_bitmap_worker_stop(s);
    }

    if (s->state != MIG_STATE_ERROR) {
        migrate_set_state(s, MIG_STATE_MC, MIG_STATE_COMPLETED);
    }

    qemu_bh_schedule(s->cleanup_bh);
    qemu_mutex_unlock_iothread();

    return NULL;
}

/*
 * Get the next copyset in the list. If there is none, then make one.
 */
static MCCopyset *mc_copy_next(MCParams *mc, MCCopyset *copyset)
{
    if (!QTAILQ_NEXT(copyset, node)) {
        int idx = mc->nb_copysets++;
        DDPRINTF("Extending copysets by one: %d sets total, "
                 "%" PRIu64 " MB\n", mc->nb_copysets,
                 mc->nb_copysets * sizeof(MCCopyset) / 1024UL / 1024UL);
        mc->curr_copyset = g_malloc(sizeof(MCCopyset));
        mc->curr_copyset->idx = idx;
        QTAILQ_INSERT_TAIL(&mc->copy_head, mc->curr_copyset, node);
        copyset = mc->curr_copyset;
    } else {
        copyset = QTAILQ_NEXT(copyset, node);
    }

    mc->curr_copyset = copyset;
    copyset->nb_copies = 0;

    return copyset;
}

void mc_process_incoming_checkpoints_if_requested(QEMUFile *f)
{
    MCParams mc = { .file = f };
    MCSlab *slab;
    int fd = qemu_get_fd(f);
    QEMUFile *mc_control, *mc_staging;
    uint64_t checkpoint_size, action;
    uint64_t slabs;
    int got, x, ret, received = 0;
    bool checkpoint_received;

    CALC_MAX_STRIKES();

    if (!mc_requested) {
        DPRINTF("Source has not requested MC. Returning.\n");
        return;
    }
   
    if (!(mc_control = qemu_fopen_socket(fd, "wb"))) {
        fprintf(stderr, "Could not make incoming MC control channel\n");
        goto rollback;
    }

    if (!(mc_staging = qemu_fopen_mc(&mc, "rb"))) {
        fprintf(stderr, "Could not make outgoing MC staging area\n");
        goto rollback;
    }

    //qemu_set_block(fd);
    socket_set_nodelay(fd);

    while (true) {
        checkpoint_received = false;
        ret = mc_recv(f, MC_TRANSACTION_ANY, &action);
        if (ret < 0) {
            goto rollback;
        }

        switch(action) {
        case MC_TRANSACTION_START:
            checkpoint_size = qemu_get_be64(f);
            mc.start_copyset = qemu_get_be64(f);
            slabs = qemu_get_be64(f);

            DDPRINTF("Transaction start: size %" PRIu64 
                     " copyset start: %" PRIu64 " slabs %" PRIu64 "\n",
                     checkpoint_size, mc.start_copyset, slabs);

            assert(checkpoint_size);
            break;
        case MC_TRANSACTION_COMMIT: /* tcp */
            slab = mc_slab_start(&mc);
            received = 0;

            while (received < checkpoint_size) {
                int total = 0;
                slab->size = qemu_get_be64(f);

                DDPRINTF("Expecting size: %" PRIu64 "\n", slab->size);

                while (total != slab->size) {
                    got = qemu_get_buffer(f, slab->buf + total, slab->size - total);
                    if (got <= 0) {
                        fprintf(stderr, "Error pre-filling checkpoint: %d\n", got);
                        goto rollback;
                    }
                    DDPRINTF("Received %d slab %d / %ld received %d total %"
                             PRIu64 "\n", got, total, slab->size, 
                             received, checkpoint_size);
                    received += got;
                    total += got;
                }

                if (received != checkpoint_size) {
                    slab = mc_slab_next(&mc, slab);
                }
            }

            DDPRINTF("Acknowledging successful commit\n");

            if (mc_send(mc_control, MC_TRANSACTION_ACK) < 0) {
                goto rollback;
            }

            checkpoint_received = true;
            break;
        case RAM_SAVE_FLAG_HOOK: /* rdma */
            /*
             * Must be RDMA registration handling. Preallocate
             * the slabs (if not already done in a previous checkpoint)
             * before allowing RDMA to register them.
             */
            slab = mc_slab_start(&mc);

            DDPRINTF("Pre-populating slabs %" PRIu64 "...\n", slabs);

            for(x = 1; x < slabs; x++) {
                slab = mc_slab_next(&mc, slab);
            }

            ram_control_load_hook(f, action);

            DDPRINTF("Hook complete.\n");

            slab = QTAILQ_FIRST(&mc.slab_head);

            for(x = 0; x < slabs; x++) {
                slab->size = qemu_get_be64(f);
                slab = QTAILQ_NEXT(slab, node);
            }

            checkpoint_received = true;
            break;
        default:
            fprintf(stderr, "Unknown MC action: %" PRIu64 "\n", action);
            goto rollback;
        }

        if (checkpoint_received) {
            mc.curr_slab = QTAILQ_FIRST(&mc.slab_head);
            mc.slab_total = checkpoint_size;

            DDPRINTF("Committed Loading MC state \n");

            mc_copy_start(&mc);

            if (qemu_loadvm_state(mc_staging) < 0) {
                fprintf(stderr, "loadvm transaction failed\n");
                /*
                 * This is fatal. No rollback possible because we have potentially
                 * applied only a subset of the checkpoint to main memory, potentially
                 * leaving the VM in an inconsistent state.
                 */
                goto err;
            }

            mc.slab_total = checkpoint_size;

            DDPRINTF("Transaction complete.\n");
            mc.checkpoints++;
        }
    }

rollback:
    fprintf(stderr, "MC: checkpointing stopped. Recovering VM\n");
    goto out;
err:
    fprintf(stderr, "Micro Checkpointing Protocol Failed\n");
    exit(1); 
out:
    if (mc_staging) {
        qemu_fclose(mc_staging);
    }

    if (mc_control) {
        qemu_fclose(mc_control);
    }
}

static int mc_get_buffer_internal(void *opaque, uint8_t *buf, int64_t pos,
                                  int size, MCSlab **curr_slab, uint64_t end_idx)
{
    uint64_t len = size;
    uint8_t *data = (uint8_t *) buf;
    MCSlab *slab = *curr_slab;
    MCParams *mc = opaque;

    assert(slab);

    DDDPRINTF("got request for %d bytes %p %p. idx %d\n",
              size, slab, QTAILQ_FIRST(&mc->slab_head), slab->idx);

    while (len && slab) {
        uint64_t get = MIN(slab->size - slab->read, len);

        memcpy(data, slab->buf + slab->read, get);

        data           += get;
        slab->read     += get;
        len            -= get;
        mc->slab_total -= get;

        DDDPRINTF("got: %" PRIu64 " read: %" PRIu64 
                 " len %" PRIu64 " slab_total %" PRIu64 
                 " size %" PRIu64 " addr: %p slab %d"
                 " requested %d\n",
                 get, slab->read, len, mc->slab_total, 
                 slab->size, slab->buf, slab->idx, size);

        if (len) {
            if (slab->idx == end_idx) {
                break;
            }

            slab = QTAILQ_NEXT(slab, node);
        }
    }

    *curr_slab = slab;
    DDDPRINTF("Returning %" PRIu64 " / %d bytes\n", size - len, size);

    return size - len;
}
static int mc_get_buffer(void *opaque, uint8_t *buf, int64_t pos, int size)
{
    MCParams *mc = opaque;

    return mc_get_buffer_internal(mc, buf, pos, size, &mc->curr_slab,
                                  mc->start_copyset - 1);
}

static int mc_load_page(QEMUFile *f, void *opaque, void *host_addr, long size)
{
    MCParams *mc = opaque;

    DDDPRINTF("Loading page into %p of size %" PRIu64 "\n", host_addr, size);

    return mc_get_buffer_internal(mc, host_addr, 0, size, &mc->mem_slab,
                                  mc->nb_slabs - 1);
}

/*
 * Provide QEMUFile with an *local* RDMA-based way to do memcpy().
 * This lowers cache pollution and allows the CPU pipeline to
 * remain free for regular use by VMs (as well as by neighbors).
 *
 * In a future implementation, we may attempt to perform this
 * copy *without* stopping the source VM - if the data shows
 * that it can be done effectively.
 */
static int mc_save_page(QEMUFile *f, void *opaque,
                           ram_addr_t block_offset, 
                           uint8_t *host_addr,
                           ram_addr_t offset,
                           long size, int *bytes_sent)
{
    MCParams *mc = opaque;
    MCCopyset *copyset = mc->curr_copyset;
    MCCopy *c;

    if (copyset->nb_copies >= MC_MAX_SLAB_COPY_DESCRIPTORS) {
        copyset = mc_copy_next(mc, copyset);
    }

    c = &copyset->copies[copyset->nb_copies++];
    c->ramblock_offset = (uint64_t) block_offset;
    c->host_addr = (uint64_t) host_addr;
    c->offset = (uint64_t) offset;
    c->size = (uint64_t) size;
    mc->total_copies++;

    return RAM_SAVE_CONTROL_DELAYED;
}

static ssize_t mc_writev_buffer(void *opaque, struct iovec *iov,
                                int iovcnt, int64_t pos)
{
    ssize_t len = 0;
    unsigned int i;

    for (i = 0; i < iovcnt; i++) {
        DDDPRINTF("iov # %d, len: %" PRId64 "\n", i, iov[i].iov_len); 
        len += mc_put_buffer(opaque, iov[i].iov_base, 0, iov[i].iov_len); 
    }

    return len;
}

static int mc_get_fd(void *opaque)
{
    MCParams *mc = opaque;

    return qemu_get_fd(mc->file);
}

static int mc_close(void *opaque)
{
    MCParams *mc = opaque;
    MCSlab *slab, *next;

    QTAILQ_FOREACH_SAFE(slab, &mc->slab_head, node, next) {
        ram_control_remove(mc->file, (uint64_t) slab->buf);
        QTAILQ_REMOVE(&mc->slab_head, slab, node);
        g_free(slab);
    }

    mc->curr_slab = NULL;

    return 0;
}
	
static const QEMUFileOps mc_write_ops = {
    .writev_buffer = mc_writev_buffer,
    .put_buffer = mc_put_buffer,
    .get_fd = mc_get_fd,
    .close = mc_close,
    .save_page = mc_save_page,
};

static const QEMUFileOps mc_read_ops = {
    .get_buffer = mc_get_buffer,
    .get_fd = mc_get_fd,
    .close = mc_close,
    .load_page = mc_load_page,
};

QEMUFile *qemu_fopen_mc(void *opaque, const char *mode)
{
    MCParams *mc = opaque;
    MCSlab *slab;
    MCCopyset *copyset;

    if (qemu_file_mode_is_not_valid(mode)) {
        return NULL;
    }

    QTAILQ_INIT(&mc->slab_head);
    QTAILQ_INIT(&mc->copy_head);

    slab = qemu_memalign(8, sizeof(MCSlab));
    memset(slab, 0, sizeof(*slab));
    slab->idx = 0;
    QTAILQ_INSERT_HEAD(&mc->slab_head, slab, node);
    mc->slab_total = 0;
    mc->curr_slab = slab;
    mc->nb_slabs = 1;
    mc->slab_strikes = 0;

    ram_control_add(mc->file, slab->buf, (uint64_t) slab->buf, MC_SLAB_BUFFER_SIZE);

    copyset = g_malloc(sizeof(MCCopyset));
    copyset->idx = 0;
    QTAILQ_INSERT_HEAD(&mc->copy_head, copyset, node);
    mc->total_copies = 0;
    mc->curr_copyset = copyset;
    mc->nb_copysets = 1;
    mc->copy_strikes = 0;

    if (mode[0] == 'w') {
        return qemu_fopen_ops(mc, &mc_write_ops);
    }

    return qemu_fopen_ops(mc, &mc_read_ops);
}

static void mc_start_checkpointer(void *opaque) {
    MigrationState *s = opaque;

    if (checkpoint_bh) {
        qemu_bh_delete(checkpoint_bh);
        checkpoint_bh = NULL;
    }

    qemu_mutex_unlock_iothread();
    qemu_thread_join(s->thread);
    g_free(s->thread);
    qemu_mutex_lock_iothread();

    migrate_set_state(s, MIG_STATE_ACTIVE, MIG_STATE_MC);
    s->thread = g_malloc0(sizeof(*s->thread));
	qemu_thread_create(s->thread, mc_thread, s, QEMU_THREAD_JOINABLE);
}

void mc_init_checkpointer(MigrationState *s)
{
    CALC_MAX_STRIKES();
    checkpoint_bh = qemu_bh_new(mc_start_checkpointer, s);
    qemu_bh_schedule(checkpoint_bh);
}

void qmp_migrate_set_mc_delay(int64_t value, Error **errp)
{
    freq_ms = value;
    CALC_MAX_STRIKES();
    DPRINTF("Setting checkpoint frequency to %" PRId64 " ms and "
            "resetting strikes to %d based on a %d sec delay.\n",
            freq_ms, max_strikes, max_strikes_delay_secs);
}

int mc_info_load(QEMUFile *f, void *opaque, int version_id)
{
    bool mc_enabled = qemu_get_byte(f);

    if (mc_enabled && !mc_requested) {
        DPRINTF("MC is requested\n");
        mc_requested = true;
    }

    max_strikes = qemu_get_be32(f);

    return 0;
}

void mc_info_save(QEMUFile *f, void *opaque)
{
    qemu_put_byte(f, migrate_use_mc());
    qemu_put_be32(f, max_strikes);
}
