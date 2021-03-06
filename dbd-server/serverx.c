#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <netdb.h>
#include <fcntl.h>
#include <sys/epoll.h>
#include <errno.h>
#include <sys/ioctl.h>
#include <corosync/cpg.h>
#include <corosync/cfg.h>

#include "../include/list.h"
#include "../include/defs.h"
#include "../include/cs.h"

#define _FILE_OFFSET_BITS 64
#define _LARGEFILE64_SOURCE
#define __USE_LARGEFILE64

#define MAXEVENTS 64


static void readit(int f, void *buf, size_t len)
{
    int res;
    char *b = buf;
    while (len > 0) {
        res = read(f, b, len);
        if(res < 0){
            if(errno == EAGAIN){
                perror("readit again");
                continue;
            }else{
                perror("readit err");
                break;
            }

        }
        len -= res;
        b += res;
    }
}

static void writeit(int f, void *buf, size_t len)
{
    int res;
    char *b = buf;
    while (len > 0) {
        res = write(f, b, len);
        if(res < 0){
            if(errno == EAGAIN){
                perror("writeit again");
                continue;
            }else{
                perror("writeit err");
                break;
            }

        }
        len -= res;
        b += res;
    }
}

struct dbd_msg *recv_msg(int fd)
{
    struct dbd_msg *msg = malloc(sizeof(struct dbd_msg));

    int size = sizeof(struct dbd_msghdr);
    int _s = 0;
    int body_size;
    readit(fd, (char*)msg, size);

    body_size = msg->head.size;
    msg->body = malloc(body_size);
    readit(fd, (char*)msg->body, body_size);
    return msg;
}

void clean_msg(struct dbd_msg *msg)
{
    free(msg->body);
    free(msg);
}

void send_msg(int fd, void *buf, int size, int type)
{
    int s = sizeof(struct dbd_msghdr) + size;
    struct dbd_msghdr *hdr = malloc(s);
    hdr->type = type;
    hdr->size = size;
    memcpy((char*)hdr + sizeof(struct dbd_msghdr), buf, size);
    writeit(fd, hdr, s);
}

static int make_socket_non_blocking (int sfd)
{
    int flags, s;

    flags = fcntl (sfd, F_GETFL, 0);
    if (flags == -1) {
        perror ("fcntl");
        return -1;
    }

    flags |= O_NONBLOCK;
    s = fcntl (sfd, F_SETFL, flags);
    if (s == -1) {
        perror ("fcntl");
        return -1;
    }

    return 0;
}

static int create_and_bind (char *port)
{
    struct addrinfo hints;
    struct addrinfo *result, *rp;
    int s, sfd;

    memset (&hints, 0, sizeof (struct addrinfo));
    hints.ai_family = AF_UNSPEC;     /* Return IPv4 and IPv6 choices */
    hints.ai_socktype = SOCK_STREAM; /* We want a TCP socket */
    hints.ai_flags = AI_PASSIVE;     /* All interfaces */

    s = getaddrinfo (NULL, port, &hints, &result);
    if (s != 0) {
        fprintf (stderr, "getaddrinfo: %s\n", gai_strerror (s));
        return -1;
    }

    for (rp = result; rp != NULL; rp = rp->ai_next) {
        sfd = socket (rp->ai_family, rp->ai_socktype, rp->ai_protocol);
        if (sfd == -1)
            continue;

        s = bind (sfd, rp->ai_addr, rp->ai_addrlen);
        if (s == 0) {
            break;
        }

        close (sfd);
    }

    if (rp == NULL) {
        fprintf (stderr, "Could not bind\n");
        return -1;
    }

    freeaddrinfo (result);

    return sfd;
}

int main (int argc, char *argv[])
{
    int sfd, s;
    int efd;
    struct epoll_event event;
    struct epoll_event *events;

    char work_path[50];



    if (argc != 3) {
        fprintf (stderr, "Usage: %s [port] [file]\n", argv[0]);
        exit (EXIT_FAILURE);
    }
    strcpy(work_path, argv[2]);
    FILE *fp = fopen(work_path, "rb+");

    sfd = create_and_bind (argv[1]);
    if (sfd == -1)
        abort ();

    s = make_socket_non_blocking (sfd);
    if (s == -1)
        abort ();

    s = listen (sfd, SOMAXCONN);
    if (s == -1) {
        perror ("listen");
        abort ();
    }

    efd = epoll_create1 (0);
    if (efd == -1) {
        perror ("epoll_create");
        abort ();
    }

    event.data.fd = sfd;
    event.events = EPOLLIN | EPOLLET;
    s = epoll_ctl (efd, EPOLL_CTL_ADD, sfd, &event);
    if (s == -1) {
        perror ("epoll_ctl");
        abort ();
    }

    events = calloc (MAXEVENTS, sizeof event);

    while (1) {
        int n, i;

        n = epoll_wait (efd, events, MAXEVENTS, -1);
        for (i = 0; i < n; i++) {
            if ((events[i].events & EPOLLERR) ||
                (events[i].events & EPOLLHUP) ||
                (!(events[i].events & EPOLLIN))) {
                /* An error has occured on this fd, or the socket is not
                   ready for reading (why were we notified then?) */
                fprintf (stderr, "epoll error\n");
                close (events[i].data.fd);
                continue;
            }

            else if (sfd == events[i].data.fd) {
                /* We have a notification on the listening socket, which
                   means one or more incoming connections. */
                while (1) {
                    struct sockaddr in_addr;
                    socklen_t in_len;
                    int infd;
                    char hbuf[NI_MAXHOST], sbuf[NI_MAXSERV];

                    in_len = sizeof in_addr;
                    infd = accept (sfd, &in_addr, &in_len);
                    if (infd == -1) {
                        if ((errno == EAGAIN) ||
                            (errno == EWOULDBLOCK)) {
                            /* We have processed all incoming
                               connections. */
                            break;
                        } else {
                            perror ("accept");
                            break;
                        }
                    }

                    s = getnameinfo (&in_addr, in_len,
                                     hbuf, sizeof hbuf,
                                     sbuf, sizeof sbuf,
                                     NI_NUMERICHOST | NI_NUMERICSERV);
                    if (s == 0) {
                        printf("Accepted connection on descriptor %d "
                               "(host=%s, port=%s)\n", infd, hbuf, sbuf);
                    }

                    s = make_socket_non_blocking (infd);
                    if (s == -1)
                        abort ();

                    event.data.fd = infd;
                    event.events = EPOLLIN | EPOLLET;
                    s = epoll_ctl (efd, EPOLL_CTL_ADD, infd, &event);
                    if (s == -1) {
                        perror ("epoll_ctl");
                        abort ();
                    }
                }
                continue;
            } else {
                int done = 0;

                int nread;
                ioctl(events[i].data.fd, FIONREAD, &nread);
                if (nread == 0) {
                    /* End of file. The remote has closed the
                       connection. */
                    done = 1;
                    goto done;
                }
                while (1) {

                    struct dbd_msg *msg = recv_msg(events[i].data.fd);

                    struct io_request *rqst = (struct io_request*)msg->body;
                    printf("type: %d, unit_id: %d, size: %d, off: %d\n", rqst->type, rqst->unit_id, rqst->size, rqst->offset);
                    switch(rqst->type) {
                    case DBD_MSG_READ: {
                        int s = sizeof(struct io_response) + rqst->size;
                        int ret;
                        struct io_response *rsp = malloc(s);
                        memcpy(rsp->handle, rqst->handle, sizeof(rqst->handle));
                        rsp->inner_offset = rqst->inner_offset;
                        rsp->size = rqst->size;
                        uint64_t addr = rqst->unit_id * UNIT_SIZE + rqst->offset;
                        ret = fseek(fp, addr, SEEK_SET);
                        fread((char*)rsp + sizeof(struct io_response), rqst->size, 1, fp);

                        send_msg(events[i].data.fd, rsp, s, DBD_MSG_RESPONSE);
                        free(rsp);
                        break;
                    }
                    case DBD_MSG_WRITE: {
                        int s = sizeof(struct io_response);
                        struct io_response *rsp = malloc(s);
                        memcpy(rsp->handle, rqst->handle, sizeof(rqst->handle));
                        rsp->inner_offset = rqst->inner_offset;
                        rsp->size = rqst->size;

                        uint64_t addr = rqst->unit_id * UNIT_SIZE + rqst->offset;
                        fseek(fp, addr, SEEK_SET);
                        fwrite((char*)rqst + sizeof(struct io_request), rqst->size, 1, fp);

                        send_msg(events[i].data.fd, rsp, s, DBD_MSG_RESPONSE);
                        free(rsp);
                        break;
                    }
                    case DBD_MSG_CREATEOS: {
                        break;
                    }
                    default:
                        break;
                    }
                    clean_msg(msg);
                    ioctl(events[i].data.fd, FIONREAD, &nread);
                    if (nread == 0) {
                        break;
                    }
                }
done:
                if (done) {
                    printf ("Closed connection on descriptor %d\n",
                            events[i].data.fd);
                    close (events[i].data.fd);
                }
            }
        }
    }

    free (events);

    close (sfd);
    fclose(fp);
    return EXIT_SUCCESS;
}
