/*
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at:
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 *------------------------------------------------------------------
 */

#include <stdint.h>          
#include <net/if.h>
#include <sys/types.h>
#include <fcntl.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/uio.h>
#include <sys/mman.h>
#include <sys/prctl.h>
#include <inttypes.h>
#include <string.h>
#include <stdio.h>
#include <netdb.h>
#include <linux/ip.h>
#include <linux/icmp.h>
#include <arpa/inet.h>
#include <stdlib.h>
#include <netinet/if_ether.h>
#include <net/if_arp.h>
#include <asm/byteorder.h>
#include <byteswap.h>
#include <string.h>
#include <errno.h>
#include <sys/stat.h>
#include <sys/eventfd.h>
#include <sys/timerfd.h>

/* memif protocol msg, ring and descriptor definitions */
#include <memif.h>
/* memif api */
#include <libmemif.h>
/* socket messaging functions */
#include <socket.h>
/* private structs and functions */
#include <memif_private.h>

/*
 * WIP
 */ 
typedef struct 
{
    int fd;
    uint16_t use_count;
    uint8_t *filename;
} memif_socket_t;

/*
 * WIP
 */ 
/* probably function like memif_cleanup () will need to be called
    close timerfd, free struct libmemif_main and its nested structures */
typedef struct
{
    memif_control_fd_update_t *control_fd_update;
    int timerfd;
    struct itimerspec arm, disarm;

    /* TODO: update to arrays support multiple connections */
    memif_socket_t ms;
    memif_connection_t *conn;
} libmemif_main_t;

libmemif_main_t libmemif_main;

static void
print_conn (memif_connection_t *c)
{
    printf ("MEMIF CONNECTION:\n");
    printf ("\tconn %p\n", c);
    printf ("\tfd %d\n", c->fd);
    printf ("\tregion %p\n", c->regions);
    printf ("\ttx %p\n", c->tx_queues);
    printf ("\trx %p\n", c->rx_queues);
}

int memif_init (memif_control_fd_update_t *on_control_fd_update)
{
    libmemif_main_t *lm = &libmemif_main;
    lm->control_fd_update = on_control_fd_update;
    memset (&lm->ms, 0, sizeof (memif_socket_t));
    lm->conn = NULL;

    lm->timerfd = timerfd_create (CLOCK_REALTIME, TFD_NONBLOCK);
    if (lm->timerfd < 0)
    {
        DBG ("timerfd: %s", strerror (errno));
        return -1;
    }

    lm->arm.it_value.tv_sec = 2;
    lm->arm.it_value.tv_nsec = 0;
    lm->arm.it_interval.tv_sec = 2;
    lm->arm.it_interval.tv_nsec = 0;
    memset (&lm->disarm, 0, sizeof (lm->disarm));

    /* TODO: check return */
    lm->control_fd_update (lm->timerfd, MEMIF_FD_EVENT_READ);

    return 0;
}

static inline memif_ring_t *
memif_get_ring (memif_connection_t *conn, memif_ring_type_t type, uint16_t rn)
{
    if (conn->regions == NULL)
        return NULL;
    /* TODO: support multiple regions */
    void *p = conn->regions->shm;
    int ring_size =
        sizeof (memif_ring_t) +
        sizeof (memif_desc_t) * (1 << conn->args.log2_ring_size);
    p += (rn + type * conn->args.num_s2m_rings) * ring_size;

    return (memif_ring_t *) p;
}

int
memif_create (memif_conn_handle_t *c, memif_conn_args_t *args,
              memif_connection_update_t *on_connect,
              memif_connection_update_t *on_disconnect,
              void *private_ctx)
{
    memif_connection_t *conn = (memif_connection_t *) *c;
    if (conn != NULL)
    {
        DBG ("This handle already points to existing memif.");
        return -1;
    }
    conn = (memif_connection_t *) malloc (sizeof (memif_connection_t));
    if (conn == NULL)
    {
        DBG_UNIX ("out of memory!");
        return -1;
    }
    libmemif_main_t *lm = &libmemif_main;

    int err;
    int sockfd = -1;
    conn->args.interface_id = args->interface_id;
    /* lib or app? */
    if (args->log2_ring_size == 0)
        args->log2_ring_size = MEMIF_DEFAULT_LOG2_RING_SIZE;
    if (!args->buffer_size == 0)
        args->buffer_size = MEMIF_DEFAULT_BUFFER_SIZE;

    conn->args.num_s2m_rings = args->num_s2m_rings;
    conn->args.num_m2s_rings = args->num_m2s_rings;
    conn->args.buffer_size = args->buffer_size;
    conn->args.log2_ring_size = args->log2_ring_size;
    conn->args.is_master = args->is_master;
    conn->msg_queue = NULL;
    conn->regions = NULL;
    conn->tx_queues = NULL;
    conn->rx_queues = NULL;
    conn->fd = -1;
    conn->on_connect = on_connect;
    conn->on_disconnect = on_disconnect;
    conn->private_ctx = private_ctx;

    uint8_t l = strlen ((char *) args->interface_name);
    strncpy ((char *) conn->args.interface_name, (char *) args->interface_name, l);
    
    l = strlen ((char *) args->instance_name);
    strncpy ((char *) conn->args.instance_name, (char *) args->instance_name, l);

    if (args->socket_filename)
    {
        conn->args.socket_filename = (uint8_t *) malloc (
                    strlen ((char *) args->socket_filename));
        if (conn->args.socket_filename == NULL)
            {
                DBG_UNIX ("out of memory!");
                err = errno;
                goto error;
            }
        strncpy ((char *) conn->args.socket_filename, (char *) args->socket_filename,
                    strlen ((char *) args->socket_filename));
    }
    else
    {
        uint16_t sdl = strlen (MEMIF_DEFAULT_SOCKET_DIR);
        uint16_t sfl = strlen (MEMIF_DEFAULT_SOCKET_FILENAME);
        conn->args.socket_filename = (uint8_t *) malloc (sdl + sfl + 1);
        if (conn->args.socket_filename == NULL)
            {
                DBG_UNIX ("out of memory!");
                err = errno;
                goto error;
            }
        strncpy ((char *) conn->args.socket_filename,
                    MEMIF_DEFAULT_SOCKET_DIR, sdl);
        conn->args.socket_filename[sdl] = '/';
        strncpy ((char *) (conn->args.socket_filename + 1 +sdl),
                    MEMIF_DEFAULT_SOCKET_FILENAME, sfl);
    }

    if (args->secret)
    {
        l = strlen ((char *) args->secret);
        strncpy ((char *) conn->args.secret, (char *) args->secret, l);
    }

    if (conn->args.is_master)
    {
        /* master */
        struct sockaddr_un un;
        struct stat file_stat;
        int on = 1;

        memif_socket_t ms = lm->ms;

        DBG ("memif master mode draft...");

        if (lm->ms.fd == 0)
        {
            if (stat ((char *) conn->args.socket_filename, &file_stat) == 0)
            {
                if (S_ISSOCK (file_stat.st_mode))
                    {
                        unlink ((char *) conn->args.socket_filename);
                    }
                else
                    {
                        err = 0;
                        DBG ("file with specified socket filename exists but is not socket");
                        goto error;
                    }
            }

            sockfd = socket (AF_UNIX, SOCK_SEQPACKET, 0);
            if (sockfd < 0)
            {
                err = errno;
                goto error;
            }
            un.sun_family = AF_UNIX;
            strncpy ((char *) un.sun_path, (char *) conn->args.socket_filename,
                        sizeof (un.sun_path) - 1);

            if (setsockopt (sockfd, SOL_SOCKET, SO_PASSCRED, &on, sizeof (on)) < 0)
            {
                err = errno;
                goto error;
            }
            if (bind (sockfd, (struct sockaddr *) &un, sizeof (un)) == -1)
            {
            err = errno;
            goto error;
            }
            if (listen (sockfd, 1) == -1)
            {
                err = errno;
                goto error;
            }
            if (stat ((char *) conn->args.socket_filename, &file_stat) == -1)
            {
                err = errno;
                goto error;
            }
            lm->ms.fd = sockfd;
            lm->ms.use_count++;
            lm->ms.filename = malloc (strlen ((char *) conn->args.socket_filename));
            strncpy ((char *) lm->ms.filename, (char *) conn->args.socket_filename,
                            strlen ((char *) conn->args.socket_filename));
        }

        DBG ("fd %d, uc %u, filename %s", lm->ms.fd, lm->ms.use_count, lm->ms.filename);
        conn->fd = lm->ms.fd;
        conn->read_fn = memif_conn_fd_accept_ready;
    }
    else
    {
        if (timerfd_settime (lm->timerfd, 0, &lm->arm, NULL) < 0)
        {
            DBG ("timerfd: %s", strerror (errno));
            goto error;
        }
    }

    *c = lm->conn = conn;

    return 0;

error:
    if (sockfd > 0)
        close (sockfd);
    sockfd = -1;
    if (conn->args.socket_filename)
        free (conn->args.socket_filename);
    free (conn);
    *c = conn = NULL;
    error_return_unix ("%s", strerror (err));
}

/* TODO: support multiple interfaces */
int
memif_control_fd_handler (int fd, uint8_t events)
{
    int i;
    memif_connection_t *conn;
    libmemif_main_t *lm = &libmemif_main;
    if (fd == lm->timerfd)
    {
        uint64_t b;
        ssize_t size;
        size = read (fd, &b, sizeof (b));
            conn = lm->conn;
            if (conn->fd < 0)
            {
                DBG ("try connect");
                struct sockaddr_un sun;
                int sockfd = socket (AF_UNIX, SOCK_SEQPACKET, 0);
                if (sockfd < 0)
                {
                    DBG ("%s", strerror (errno));
                }

                sun.sun_family = AF_UNIX;
                strncpy (sun.sun_path, conn->args.socket_filename,
                            sizeof (sun.sun_path) -1);

                if (connect (sockfd, (struct sockaddr *) &sun,
                        sizeof (struct sockaddr_un)) == 0)
                {
                    conn->fd = sockfd;
                    conn->read_fn = memif_conn_fd_read_ready;
                    conn->write_fn = memif_conn_fd_write_ready;
                    conn->error_fn = memif_conn_fd_error;

                    lm->control_fd_update (
                            sockfd, MEMIF_FD_EVENT_READ | MEMIF_FD_EVENT_WRITE);

                    /* TODO: with multiple connections support,
                        only disarm if there is no disconnected slave */
                    if (timerfd_settime (lm->timerfd, 0, &lm->disarm, NULL) < 0)
                    {
                        DBG ("timerfd: %s", strerror (errno));
                    }                    
                }
                else
                {
                    DBG ("%s", strerror (errno));
                    if (sockfd > 0)
                        close (sockfd);
                    sockfd = -1;
                }
            }
    }
    else
    {
            conn = lm->conn;
            if (conn->fd == fd)
            {
                if (events & MEMIF_FD_EVENT_READ)
                    conn->read_fn (conn);
                if (events & MEMIF_FD_EVENT_WRITE)
                    conn->write_fn (conn);
                if (events & MEMIF_FD_EVENT_ERROR)
                    conn->error_fn (conn);
            }
    }
    return 0;
}

static void
memif_msg_queue_free (memif_msg_queue_elt_t **e)
{
    if (*e == NULL)
        return;
    memif_msg_queue_free (&(*e)->next);
    free (*e);
    *e = NULL;
    return;
}

/* send disconnect msg and close interface */
int
memif_disconnect_internal (memif_connection_t *c)
{
    if (c == NULL)
    {
        DBG ("no connection");
        return -1;
    }

    libmemif_main_t *lm = &libmemif_main;

    memif_msg_send_disconnect (c, c->remote_disconnect_string, 1);

    if (c->fd > 0)
    {
        lm->control_fd_update (c->fd, MEMIF_FD_EVENT_DEL);
        close (c->fd);
    }
    c->fd = -1;

    /* TODO: support multiple rings */
    if (c->tx_queues != NULL)
    {
        free (c->tx_queues);
        c->tx_queues = NULL;
    }
    if (c->rx_queues != NULL)
    {
        free (c->rx_queues);
        c->rx_queues = NULL;
    }

    /* TODO: support multiple regions */
    if (c->regions != NULL)
    {
        if (munmap (c->regions->shm, c->regions->region_size) < 0)
            DBG ("munmap: %s", strerror (errno));
        if (c->regions->fd > 0)
            close (c->regions->fd);
        c->regions->fd = -1;
        free (c->regions);
        c->regions = NULL;
    }

    memif_msg_queue_free (&c->msg_queue);

    /* TODO: use timerfd_gettime to check if timer is armed
        only arm if timer is disarmed */
    if (timerfd_settime (lm->timerfd, 0, &lm->arm, NULL) < 0)
    {
        DBG ("timerfd: %s", strerror (errno));
    }

    c->on_disconnect ((void *) c, c->private_ctx);

    return 0;
}

int
memif_delete (memif_conn_handle_t *conn)
{
    memif_connection_t *c = (memif_connection_t *) *conn;
    libmemif_main_t *lm = &libmemif_main;

    /* only fail if there is no connection to remove */
    if (memif_disconnect_internal (c) < 0)
        return -1;

    /* TODO: only disarm if this is the only disconnected slave */
    if (timerfd_settime (lm->timerfd, 0, &lm->disarm, NULL) < 0)
    {
        DBG ("timerfd: %s", strerror (errno));
    }

    if (c->args.socket_filename)
        free (c->args.socket_filename);
    c->args.socket_filename = NULL;

    free (c);
    c = NULL;

    *conn = c;
    return 0;
}


int
memif_connect1 (memif_connection_t *c)
{
    memif_region_t *mr = c->regions;
    /* TODO: support multiple regions */
    if (mr != NULL)
    {
        if (!mr->shm)
        {
            if (mr->fd < 0)
                error_return ("no memory region fd");

            if ((mr->shm = mmap (NULL, mr->region_size, PROT_READ | PROT_WRITE,
                     MAP_SHARED, mr->fd, 0)) == MAP_FAILED)
            error_return_unix ("mmap");
        }
    }

    /* TODO: support multiple queues */
    int i = 0;
    memif_queue_t *mq = c->tx_queues;
    if (mq != NULL)
    {
        mq->ring = c->regions->shm + mq->offset;
        if (mq->ring->cookie != MEMIF_COOKIE)
              error_return ("wrong cookie on tx ring %u", i);
        i++;
    }
    i = 0;
    mq = c->rx_queues;
    if (mq != NULL)
    {
        mq->ring = c->regions->shm + mq->offset;
        if (mq->ring->cookie != MEMIF_COOKIE)
            error_return ("wrong cookie on tx ring %u", i);
        i++;
    }
    return 0;
}

int
memif_init_regions_and_queues (memif_connection_t *conn)
{
    memif_ring_t *ring = NULL;
    uint64_t buffer_offset;
    memif_region_t *r;
    int i,j;

    conn->regions = (memif_region_t *) malloc (sizeof (memif_region_t));
    r = conn->regions;

    buffer_offset = (conn->args.num_s2m_rings + conn->args.num_m2s_rings) *
        (sizeof (memif_ring_t) +
        sizeof (memif_desc_t) * (1 << conn->args.log2_ring_size));

    r->region_size = buffer_offset +
        conn->args.buffer_size * (1 << conn->args.log2_ring_size) *
        (conn->args.num_s2m_rings + conn->args.num_m2s_rings);
    
    if ((r->fd = memfd_create ("memif region 0", MFD_ALLOW_SEALING)) == -1)
        error_return_unix ("memfd_create: %s", strerror (errno));

    if ((fcntl (r->fd, F_ADD_SEALS, F_SEAL_SHRINK)) == -1)
        error_return_unix ("fcntl: %s", strerror (errno));

    if ((ftruncate (r->fd, r->region_size)) == -1)
        error_return_unix ("ftruncate: %s", strerror (errno));

    if ((r->shm = mmap (NULL, r->region_size, PROT_READ | PROT_WRITE,
                        MAP_SHARED, r->fd, 0)) == MAP_FAILED)
        error_return_unix ("mmap: %s", strerror (errno));

    for (i = 0; i < conn->args.num_s2m_rings; i++)
    {
        ring = memif_get_ring (conn, MEMIF_RING_S2M, i);
        ring->head = ring->tail = 0;
        ring->cookie = MEMIF_COOKIE;
        for (j = 0; j < (1 << conn->args.log2_ring_size); j++)
        {
            uint16_t slot = i * (1 << conn->args.log2_ring_size) + j;
            ring->desc[j].region = 0;
            ring->desc[j].offset = buffer_offset +
                    (uint32_t) (slot * conn->args.buffer_size);
            ring->desc[j].buffer_length = conn->args.buffer_size;
        }
    }
    for (i = 0; i < conn->args.num_m2s_rings; i++)
    {
        ring = memif_get_ring (conn, MEMIF_RING_M2S, i);
        ring->head = ring->tail = 0;
        ring->cookie = MEMIF_COOKIE;
        for (j = 0; j < (1 << conn->args.log2_ring_size); j++)
        {
            uint16_t slot = (i + conn->args.num_s2m_rings) * (1 << conn->args.log2_ring_size) + j;
            ring->desc[j].region = 0;
            ring->desc[j].offset = buffer_offset +
                    (uint32_t) (slot * conn->args.buffer_size);
            ring->desc[j].buffer_length = conn->args.buffer_size;
        }
    }
    memif_queue_t *mq;
    int x;
    for (x = 0; x < conn->args.num_s2m_rings; x++)
    {
        mq = (memif_queue_t *) malloc (sizeof (memif_queue_t));
        if ((mq->int_fd = eventfd (0, EFD_NONBLOCK)) < 0)
            error_return_unix ("eventfd: %s", strerror (errno));
        mq->ring = memif_get_ring (conn, MEMIF_RING_S2M, x);
        mq->log2_ring_size = conn->args.log2_ring_size;
        mq->region = 0;
        mq->offset = (void *) mq->ring - (void *) conn->regions->shm;
        conn->tx_queues = mq;
    }

    for (x = 0; x < conn->args.num_m2s_rings; x++)
    {
        mq = (memif_queue_t *) malloc (sizeof (memif_queue_t));
        if ((mq->int_fd = eventfd (0, EFD_NONBLOCK)) < 0)
            error_return_unix ("eventfd: %s", strerror (errno));
        mq->ring = memif_get_ring (conn, MEMIF_RING_M2S, x);
        mq->log2_ring_size = conn->args.log2_ring_size;
        mq->region = 0;
        mq->offset = (void *) mq->ring - (void *) conn->regions->shm;
        mq->last_head = 0; 
        conn->rx_queues = mq;
    }

    return 0;
}
