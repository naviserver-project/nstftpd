/*
 * The contents of this file are subject to the Mozilla Public License
 * Version 1.1 (the "License"); you may not use this file except in
 * compliance with the License. You may obtain a copy of the License at
 * http://www.mozilla.org/.
 *
 * Software distributed under the License is distributed on an "AS IS"
 * basis, WITHOUT WARRANTY OF ANY KIND, either express or implied. See
 * the License for the specific language governing rights and limitations
 * under the License.
 *
 * Copyright (C) 2001-2003 Vlad Seryakov
 * All rights reserved.
 *
 * Alternatively, the contents of this file may be used under the terms
 * of the GNU General Public License (the "GPL"), in which case the
 * provisions of GPL are applicable instead of those above.  If you wish
 * to allow use of your version of this file only under the terms of the
 * GPL and not to allow others to use your version of this file under the
 * License, indicate your decision by deleting the provisions above and
 * replace them with the notice and other provisions required by the GPL.
 * If you do not delete the provisions above, a recipient may use your
 * version of this file under either the License or the GPL.
 */

/*
 * nstftpd.c -- TFTP  server
 *
 * Authors
 *
 *     Vlad Seryakov vlad@crystalballinc.com
 */

#include "ns.h"
#include <stdlib.h>
#include <stdio.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <sys/time.h>
#include <netdb.h>
#include <errno.h>
#include <signal.h>
#include <sys/socket.h>
#include <string.h>
#include <unistd.h>

#define MAX_BLKSIZE 1536

typedef struct {
    char *server;
    char *rootpath;
    bool drivermode;
    int umask;
    short blksize;
    short timeout;
    int retries;
    int debug;
    int sock;
    unsigned short port;
    const char *address;
    const char *proc;
} TFTPServer;

typedef struct
{
    TFTPServer *server;
    char *file;
    char mode[16];
    int fd;
    int op;
    int sock;
    unsigned short block;
    short blksize;
    short timeout;
    struct stat fstat;
    struct NS_SOCKADDR_STORAGE sa;
    Ns_DString ds;
    int pktsize;
    union {
        char data[MAX_BLKSIZE];
        union {
            struct {
                unsigned short opcode;
                unsigned short block;
                char data[MAX_BLKSIZE-4];
            } pkt;
            struct {
                unsigned short opcode;
                union {
                    unsigned short block;
                    char data[512];
                } op;
            } ack;
            struct {
                unsigned short opcode;
                unsigned short errcode;
                unsigned char msg[508];
            } error;
        } var;
    } content;
    struct {
        unsigned short opcode;
        unsigned short block;
        char data[MAX_BLKSIZE];
    } reply;
} TFTPRequest;

static Ns_DriverListenProc Listen;
static Ns_DriverAcceptProc Accept;
static Ns_DriverRecvProc Recv;
static Ns_DriverSendProc Send;
static Ns_DriverSendFileProc SendFile;
static Ns_DriverKeepProc Keep;
static Ns_DriverCloseProc Close;
static Ns_DriverRequestProc Request;

static Ns_SockProc TFTPSockProc;
static TFTPRequest *TFTPNew(TFTPServer *server);
static void TFTPProcessRequest(TFTPRequest *arg);
static void TFTPFree(TFTPRequest *req);
static void TFTPThread(void *arg);
static int TFTPRecv(TFTPRequest *req);
static ssize_t TFTPSend(TFTPRequest *req, char *buf, size_t len);
static ssize_t TFTPSendACK(TFTPRequest *req, char *buf, size_t len);
static ssize_t TFTPSendError(TFTPRequest *req, int errcode, const char *msg, int err);
static int TFTPCmd(ClientData arg, Tcl_Interp *interp,int objc,Tcl_Obj *const objv[]);

static Ns_TclTraceProc TFTPInterpInit;

NS_EXPORT int Ns_ModuleVersion = 1;
NS_EXPORT Ns_ModuleInitProc Ns_ModuleInit;

NS_EXPORT Ns_ReturnCode Ns_ModuleInit(const char *server, const char *module)
{
    const char *path;
    Tcl_DString ds;
    TFTPServer *srvPtr;
    Ns_DriverInitData init;

    memset(&init, 0, sizeof(init));
    srvPtr = ns_calloc(1, sizeof(TFTPServer));
    path = Ns_ConfigGetPath(server,module,NULL);
    Ns_ConfigGetBool(path, "drivermode", &srvPtr->drivermode);
    srvPtr->debug = Ns_ConfigIntRange(path, "debug", 0, 0, 10);
    srvPtr->umask = Ns_ConfigIntRange(path, "umask", 0644, 0, INT_MAX);
    srvPtr->retries = Ns_ConfigIntRange(path, "retries", 1, 1, 10);
    srvPtr->blksize = (short)Ns_ConfigIntRange(path, "blksize", 512, 512, MAX_BLKSIZE);
    srvPtr->timeout = (short)Ns_ConfigIntRange(path, "timeout", 5, 1, 1000);
    srvPtr->rootpath = ns_strcopy(Ns_ConfigGetValue(path, "rootpath"));
    srvPtr->proc = Ns_ConfigGetValue(path, "proc");
    srvPtr->port = (unsigned short)Ns_ConfigIntRange(path, "port", 69, 1, 99999);
    srvPtr->address = Ns_ConfigGetValue(path, "address");

    if (srvPtr->drivermode) {
        init.version = NS_DRIVER_VERSION_2;
        init.name = "nssyslogd";
        init.listenProc = Listen;
        init.acceptProc = Accept;
        init.recvProc = Recv;
        init.sendProc = Send;
        init.sendFileProc = SendFile;
        init.keepProc = Keep;
        init.requestProc = Request;
        init.closeProc = Close;
        init.opts = NS_DRIVER_ASYNC|NS_DRIVER_NOPARSE;
        init.arg = srvPtr;
        init.path = path;

        if (Ns_DriverInit(server, module, &init) != NS_OK) {
            Ns_Log(Error, "nstftpd: driver init failed.");
            ns_free(srvPtr);
            return NS_ERROR;
        }
    } else {
      if ((srvPtr->sock = Ns_SockListenUdp(srvPtr->address, srvPtr->port, NS_FALSE)) == -1) {
            Ns_Log(Error,"nstftp: %s:%d: couldn't create socket: %s", srvPtr->address, srvPtr->port, strerror(errno));
            ns_free(srvPtr);
            return NS_ERROR;
        }
        Ns_SockCallback(srvPtr->sock, TFTPSockProc, srvPtr, NS_SOCK_READ|NS_SOCK_EXIT|NS_SOCK_EXCEPTION);
    }
    srvPtr->server = ns_strdup(server);
    Tcl_DStringInit(&ds);
    if (srvPtr->rootpath == NULL) {
        srvPtr->rootpath = ns_strcopy(Ns_PagePath(&ds, server, "", NULL));
    }
    Ns_TclRegisterTrace(server, TFTPInterpInit, srvPtr, NS_TCL_TRACE_CREATE);
    Tcl_DStringFree(&ds);

    Ns_Log(Notice, "%s: root=%s, port=%d", module, srvPtr->rootpath, srvPtr->port);
    return NS_OK;
}

/*
 *----------------------------------------------------------------------
 *
 * Listen --
 *
 *      Open a listening socket in non-blocking mode.
 *
 * Results:
 *      The open socket or NS_INVALID_SOCKET on error.
 *
 * Side effects:
 *      None
 *
 *----------------------------------------------------------------------
 */

static NS_SOCKET
Listen(Ns_Driver *UNUSED(driver), const char *address, unsigned short port, int UNUSED(backlog), bool reuseport)
{
    NS_SOCKET sock;
    //TFTPServer *srvPtr = (TFTPServer*)driver->arg;

    sock = Ns_SockListenUdp((char*)address, port, reuseport);
    if (sock != NS_INVALID_SOCKET) {
        (void) Ns_SockSetNonBlocking(sock);
    }
    return sock;
}


/*
 *----------------------------------------------------------------------
 *
 * Accept --
 *
 *      Accept a new socket in non-blocking mode.
 *
 * Results:
 *      NS_DRIVER_ACCEPT_DATA  - socket accepted, data present
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */

static NS_DRIVER_ACCEPT_STATUS
Accept(Ns_Sock *sock, NS_SOCKET listensock, struct sockaddr *UNUSED(saPtr), socklen_t *UNUSED(socklenPtr))
{
    sock->sock = listensock;
    return NS_DRIVER_ACCEPT_DATA;
}


/*
 *----------------------------------------------------------------------
 *
 * Recv --
 *
 *      Receive data into given buffers.
 *
 * Results:
 *      Total number of bytes received or -1 on error or timeout.
 *
 * Side effects:
 *      None
 *
 *----------------------------------------------------------------------
 */

static ssize_t Recv(Ns_Sock *sock, struct iovec *bufs, int UNUSED(nbufs), Ns_Time *UNUSED(timeoutPtr), unsigned int UNUSED(flags))
{
    socklen_t salen = sizeof(sock->sa);

    ssize_t len = recvfrom(sock->sock, bufs->iov_base, bufs->iov_len - 1, 0, (struct sockaddr*)&sock->sa, &salen);
    if (len == -1) {
        char saString[NS_IPADDR_SIZE];

        Ns_Log(Error,"DriverRecv: %s: FD %d: recv from %s: %s",
               sock->driver->moduleName,
               sock->sock,
               ns_inet_ntop((struct sockaddr*)&sock->sa, saString, sizeof(saString)),
               strerror(errno));
    }
    return len;
}


/*
 *----------------------------------------------------------------------
 *
 * Send --
 *
 *      Send data from given buffers.
 *
 * Results:
 *      Total number of bytes sent or -1 on error or timeout.
 *
 * Side effects:
 *      None
 *
 *----------------------------------------------------------------------
 */

static ssize_t
Send(Ns_Sock *sock, const struct iovec *bufs, int UNUSED(nbufs),
#if NS_MAJOR_VERSION < 5
     const Ns_Time *UNUSED(timeoutPtr),
#endif
     unsigned int UNUSED(flags))
{
    ssize_t len = sendto(sock->sock, bufs->iov_base, bufs->iov_len, 0, (struct sockaddr*)&sock->sa, sizeof(struct sockaddr_in));
    if (len == -1) {
        char saString[NS_IPADDR_SIZE];

        Ns_Log(Error,"DriverSend: %s: FD %d: sendto %ld bytes to %s: %s",
               sock->driver->moduleName,
               sock->sock,
               len,
               ns_inet_ntop((struct sockaddr*)&sock->sa, saString, sizeof(saString)),
               strerror(errno));
    }
    return len;
}


/*
 *----------------------------------------------------------------------
 *
 * SendFile --
 *
 *      Send given file buffers directly to socket.
 *
 * Results:
 *      Total number of bytes sent or -1 on error or timeout.
 *
 * Side effects:
 *      May block once for driver sendwait timeout seconds if first
 *      attempt would block.
 *      May block 1 or more times due to disk IO.
 *
 *----------------------------------------------------------------------
 */

static ssize_t SendFile(Ns_Sock *UNUSED(sock), Ns_FileVec *UNUSED(bufs), int UNUSED(nbufs),
#if NS_MAJOR_VERSION < 5
                        Ns_Time *UNUSED(timeoutPtr),
#endif
                        unsigned int UNUSED(flags))
{
    return -1;
}


/*
 *----------------------------------------------------------------------
 *
 * Keep --
 *
 *      No keepalives
 *
 * Results:
 *      NS_FALSE, always.
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */

static bool Keep(Ns_Sock *UNUSED(sock))
{
    return NS_FALSE;
}

/*
 *----------------------------------------------------------------------
 *
 * Request --
 *
 *	Request callback for processing syslog connections
 *
 * Results:
 *	NS_TRUE
 *
 * Side effects:
 *      None
 *
 *----------------------------------------------------------------------
 */

static int Request(void *arg, Ns_Conn *conn)
{
    TFTPServer  *server = arg;
    Ns_Sock     *sock = Ns_ConnSockPtr(conn);
    Ns_DString  *ds = Ns_ConnSockContent(conn);
    TFTPRequest *req = TFTPNew(server);

    req->sa = sock->sa;
    if (ds != NULL) {
        /* Data was read in the driver's proc during accept */
        memcpy(req->content.data, ds->string, ds->length);
        req->pktsize = ds->length;
        TFTPProcessRequest(req);
        /* For access log file */
        if (req->file) {
            ns_free((char *)conn->request.line);
            snprintf(req->content.data, sizeof(req->content.data), "%s %s TFTP/2.0", req->op == 1 ? "GET" : "PUT", req->file);
            conn->request.line = ns_strdup(req->content.data);
            Ns_ConnSetContentSent(conn, (size_t)req->fstat.st_size);
        }
    } else {
        char saString[NS_IPADDR_SIZE];

        Ns_Log(Error, "TFTP: FD %d: %s: invalid connection",
               req->sock,
               ns_inet_ntop((struct sockaddr*)&req->sa, saString, sizeof(saString)));
    }
    TFTPFree(req);

    return NS_FILTER_BREAK;
}


/*
 *----------------------------------------------------------------------
 *
 * Close --
 *
 *      Close the connection socket.
 *
 * Results:
 *      None.
 *
 * Side effects:
 *      Does not close UDP socket
 *
 *----------------------------------------------------------------------
 */

static void Close(Ns_Sock *sock)
{
    sock->sock = -1;
}

static Ns_ReturnCode
TFTPInterpInit(Tcl_Interp *interp, const void *arg)
{
    Tcl_CreateObjCommand(interp, "ns_tftp", TFTPCmd, (ClientData)arg, NULL);
    return NS_OK;
}

static bool
TFTPSockProc(NS_SOCKET sock, void *arg, unsigned int when)
{
    TFTPServer *server = (TFTPServer*)arg;
    TFTPRequest *req;

    switch(when) {
    case NS_SOCK_READ:
         req = TFTPNew(server);
         req->sock = sock;
         if (TFTPRecv(req) > 0) {
             Ns_ThreadCreate(TFTPThread, (void *)req, 0, 0);
         } else {
             req->sock = -1;
             TFTPFree(req);
         }
         return NS_TRUE;
    }
    ns_sockclose(sock);
    return NS_FALSE;
}

static void
TFTPThread(void *arg)
{
    TFTPRequest *req = (TFTPRequest*)arg;

    Ns_ThreadSetName("tftp:%d:%s", htons(req->content.var.pkt.opcode), ns_inet_ntoa((struct sockaddr *)&req->sa));
    TFTPProcessRequest(req);
    TFTPFree(req);
}

static void
TFTPProcessRequest(TFTPRequest* req)
{
    TFTPServer *server = req->server;
    int         rc;
    ssize_t     nread;
    char       *str, *ptr;
    struct sockaddr_in sa;

    if (server->debug > 1) {
        Ns_Log(Notice, "TFTP: FD %d: %s: connected, %d bytes",
               req->sock,
               ns_inet_ntoa((struct sockaddr *)&req->sa),
               req->pktsize);
    }
    req->op = htons(req->content.var.pkt.opcode);
    /* Check request type */
    switch (req->op) {
     case 1:
         break;
     case 2:
         break;
     case 5:
         Ns_Log(Notice, "TFTP: FD %d: %s: error msg, %d: %s",
                req->sock,
                ns_inet_ntoa((struct sockaddr *)&req->sa),
                htons(req->content.var.error.errcode),
                req->content.var.error.msg);
         req->sock = -1;
         goto done;
         break;
     default:
         Ns_Log(Notice, "TFTP: FD %d: %s: invalid request, opcode=%d, %d bytes",
                req->sock,
                ns_inet_ntoa((struct sockaddr *)&req->sa),
                htons(req->content.var.pkt.opcode),
                req->pktsize);
         req->sock = -1;
         goto done;
    }
    req->sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    sa.sin_family = AF_INET;
    sa.sin_addr.s_addr = INADDR_ANY;
    sa.sin_port = 0;
    if (bind(req->sock, (struct sockaddr*)&sa, sizeof(sa)) == -1) {
        Ns_Log(Notice, "TFTP: FD %d: %s: bind error: %s",
               req->sock,
               ns_inet_ntoa((struct sockaddr *)&req->sa),
               strerror(errno));
        goto done;
    }
    connect(req->sock, (struct sockaddr*)&req->sa, sizeof(struct sockaddr_in));

    ptr = req->content.data + 2;
    req->file = ptr;
    ptr += strlen(ptr) + 1;
    strncpy(req->mode, ptr, sizeof(req->mode) - 1);
    ptr += strlen(ptr) + 1;

    /*
     * Normalize file name
     */

    snprintf(req->reply.data, sizeof(req->reply.data), "%s/%s", server->rootpath, req->file);
    req->file = ns_strdup(Ns_NormalizePath(&req->ds, req->reply.data));
    if (strncmp(req->file, server->rootpath, strlen(server->rootpath))) {
        TFTPSendError(req, 2, "Invalid Path", ENOENT);
        goto done;
    }

    /*
     * Invoke Tcl proc, it can return full pathname of the file to be returned
     */

    if (server->proc) {
        Tcl_Interp *interp = Ns_TclAllocateInterp(server->server);
        if (interp) {
            rc = Tcl_VarEval(interp, server->proc, " ",
                             (req->op == 1 ? "r" : "w"),
                             " {", req->file, "} ",
                             ns_inet_ntoa((struct sockaddr *)&req->sa),
                             0);
            if (rc != NS_OK) {
                TFTPSendError(req, 2, (char*)Tcl_GetStringResult(interp), EACCES);
                goto done;
            }
            str = (char*)Tcl_GetStringResult(interp);
            /* New file name was given */
            if (str && *str) {
                ns_free(req->file);
                req->file = ns_strdup(str);
            }
            Ns_TclDeAllocateInterp(interp);
        }
    }

    /*
     * Open the file, after this point we just return the contents
     */

    req->fd = open(req->file, req->op == 1 ? O_RDONLY : O_CREAT|O_RDWR|O_TRUNC, server->umask);
    if (req->fd <= 0) {
        TFTPSendError(req, 1, "Invalid File", errno);
        goto done;
    }
    fstat(req->fd, &req->fstat);
    if (server->debug > 2) {
        Ns_Log(Notice,"TFTP: FD %d: %s: file %s, size %llu",
               req->sock,
               ns_inet_ntoa((struct sockaddr *)&req->sa),
               req->file,
               req->fstat.st_size);
    }

    /*
     * Parse TFTP parameters
     */

    if (*ptr) {
        memset(&req->content.var.pkt, 0, sizeof(req->content.var.pkt));
        req->content.var.pkt.opcode = htons(6);
        str = req->content.var.ack.op.data;
        while (*ptr) {
            if (!strcasecmp(ptr, "blksize")) {
                strcpy(str, ptr);
                str += strlen(str) + 1;
                ptr += strlen(ptr) + 1;
                req->blksize = (short)strtol(ptr, NULL, 10);
                if (req->blksize < 512) {
                    req->blksize = 512;
                } else
                if (req->blksize > server->blksize) {
                    req->blksize = server->blksize;
                }
                if (req->blksize > MAX_BLKSIZE) {
                    req->blksize = MAX_BLKSIZE;
                }
                sprintf(str, "%u", req->blksize);
                str += strlen(str) + 1;
            } else
            if (!strcasecmp(ptr, "tsize")) {
                strcpy(str, ptr);
                str += strlen(str) + 1;
                ptr += strlen(ptr) + 1;
                sprintf(str, "%llu", req->fstat.st_size);
                str += strlen(str) + 1;
            } else
            if (!strcasecmp(ptr, "timeout")) {
                strcpy(str, ptr);
                str += strlen(str) + 1;
                ptr += strlen(ptr) + 1;
                req->timeout = (short)strtol(ptr, NULL, 10);
                if (req->timeout <= 0) {
                    req->timeout = server->timeout;
                }
                sprintf(str, "%u", req->timeout);
                str += strlen(str) + 1;
            } else {
                ptr += strlen(ptr) + 1;
            }
            ptr += strlen(ptr) + 1;
        }
        if (TFTPSendACK(req, (char*)&req->content.var.pkt, (size_t)(str - (char*)&req->content.var.pkt)) == -1) {
            goto done;
        }
        if (server->debug > 3) {
            Ns_Log(Notice, "TFTP: FD %d: %s: parameters: timeout=%d blksize=%d",
                   req->sock,
                   ns_inet_ntoa((struct sockaddr *)&req->sa),
                   req->timeout,
                   req->blksize);
        }
    }
    if (req->op == 2) {
        // Send ACK on WRQ request
        req->reply.opcode = htons(4);
        req->reply.block = htons(0);
        if (TFTPSend(req, (char*)&req->reply, 4) == -1) {
            goto done;
        }
    }
    switch (req->op) {
     case 1:
         while (req->fd > 0) {
             req->reply.opcode = htons(3);
             req->reply.block = htons(++req->block);
             nread = read(req->fd, req->reply.data, (size_t)req->blksize);
             if (nread < 0) {
                 Ns_Log(Error, "TFTP: FD %d: %s: read: %s",
                        req->sock,
                        ns_inet_ntoa((struct sockaddr *)&req->sa),
                        strerror(errno));
                 break;
             }
             // Last block read
             if (nread < req->blksize) {
                 ns_sockclose(req->fd);
                 req->fd = -1;
             }
             // Send ACK and wait for the next block
             if (TFTPSendACK(req, (char*)&req->reply, (size_t)(nread + 4)) == -1) {
                 break;
             }
         }
         break;

     case 2:
         while (req->fd > 0) {
             if (Ns_SockWait(req->sock, NS_SOCK_READ, req->timeout) != NS_OK) {
                 Ns_Log(Error, "TFTP: FD %d: %s: timeout %d secs",
                        req->sock,
                        ns_inet_ntoa((struct sockaddr *)&req->sa),
                        req->timeout);
                 goto done;
             }
             if (TFTPRecv(req) <= 0) {
                 goto done;
             }
             if (htons(req->content.var.pkt.opcode) == 3) {
                 if (htons(req->content.var.pkt.block) == req->block + 1) {
                     if (req->pktsize > 4) {
                         if (write(req->fd, req->content.var.pkt.data, (size_t)(req->pktsize - 4)) == -1) {
                             TFTPSendError(req, 3, "Unable to write", errno);
                             goto done;
                         }
                         if (req->block == USHRT_MAX) {
                             TFTPSendError(req, 3, "File Too Large", EFBIG);
                             goto done;
                         }
                         req->block++;
                     }
                     if (req->pktsize - 4 < req->blksize) {
                         ns_sockclose(req->fd);
                         req->fd = -1;
                     }
                 }
                 // Send ACK with new block number
                 req->reply.opcode = htons(4);
                 req->reply.block = htons(req->block);
                 if (TFTPSend(req, (char*)&req->reply, 4) == -1) {
                     goto done;
                 }
             }
         }
         break;
    }
done:
    if (server->debug > 1) {
        Ns_Log(Notice, "TFTP: FD %d: %s: disconnected",
               req->sock,
               ns_inet_ntoa((struct sockaddr *)&req->sa));
    }
}

static TFTPRequest *
TFTPNew(TFTPServer *server)
{
    TFTPRequest *req = ns_calloc(1, sizeof(TFTPRequest));

    req->server = server;
    req->blksize = server->blksize;
    req->timeout = server->timeout;
    Ns_DStringInit(&req->ds);
    return req;
}

static void
TFTPFree(TFTPRequest *req)
{
    if (req != NULL) {
        if (req->sock > 0) {
            ns_sockclose(req->sock);
        }
        if (req->fd > 0) {
            ns_sockclose(req->fd);
        }
        Ns_DStringFree(&req->ds);
        ns_free(req->file);
        ns_free(req);
    }
}

static int
TFTPRecv(TFTPRequest *req)
{
    socklen_t salen = sizeof(struct sockaddr_in);

    req->pktsize = (short)recvfrom(req->sock, req->content.data, sizeof(req->content.data), 0, (struct sockaddr*)&req->sa, &salen);
    if (req->pktsize <= 0) {
        Ns_Log(Error,"TFTP: FD %d: %s: recvfrom: %s",
               req->sock,
               ns_inet_ntoa((struct sockaddr *)&req->sa),
               strerror(errno));
        return -1;
    }
    if (req->server->debug > 5) {
        Ns_Log(Notice,"TFTP: FD %d: %s: recv: block %d/%d, op %d, %d bytes",
               req->sock,
               ns_inet_ntoa((struct sockaddr *)&req->sa),
               htons(req->content.var.pkt.block),
               req->block,
               htons(req->content.var.pkt.opcode),
               req->pktsize);
    }
    return req->pktsize;
}

static ssize_t
TFTPSend(TFTPRequest *req, char *buf, size_t len)
{
    ssize_t nsent;

    nsent = sendto(req->sock, buf, len, 0, (struct sockaddr*)&req->sa, sizeof(struct sockaddr_in));
    if (nsent <= 0) {
        Ns_Log(Error, "TFTP: FD %d: %s: sendto: len=%lu: %s",
               req->sock,
               ns_inet_ntoa((struct sockaddr *)&req->sa),
               len,
               strerror(errno));
        return -1;
    }
    if (req->server->debug > 5) {
        Ns_Log(Notice,"TFTP: FD %d: %s: send: block %d/%d, %ld bytes",
               req->sock,
               ns_inet_ntoa((struct sockaddr *)&req->sa),
               htons(req->content.var.pkt.block),
               req->block,
               nsent);
    }
    return nsent;
}

static ssize_t
TFTPSendError(TFTPRequest *req, int errcode, const char *msg, int err)
{
    ssize_t nsent;

    req->reply.opcode = htons(5);
    req->reply.block = htons(errcode);
    strcpy(req->reply.data, msg);
    nsent = sendto(req->sock, (char*)&req->reply, strlen(req->reply.data)+5, 0, (struct sockaddr*)&req->sa, sizeof(struct sockaddr_in));
    Ns_Log(Error,"TFTP: FD %d: %s: %s %s: %s",
           req->sock,
           ns_inet_ntoa((struct sockaddr *)&req->sa),
           msg,
           req->file,
           strerror(err));
    return nsent;
}

static ssize_t
TFTPSendACK(TFTPRequest *req, char *buf, size_t len)
{
    int retries = 0;

    do {
       if (TFTPSend(req, buf, len) <= 0) {
           return -1;
       }
       do {
          if (Ns_SockWait(req->sock, NS_SOCK_READ, req->timeout) != NS_OK) {
              Ns_Log(Error, "TFTP: FD %d: %s: timeout %d secs",
                     req->sock,
                     ns_inet_ntoa((struct sockaddr *)&req->sa),
                     req->timeout);
              return 0;
          }
          if (TFTPRecv(req) <= 0) {
              return -1;
          }
       } while (htons(req->content.var.pkt.opcode) != 4 && htons(req->content.var.pkt.block) != req->block);
    } while (req->pktsize == 0 && retries++ < req->server->retries);
    if (req->pktsize <= 0) {
        Ns_Log(Error,"TFTP: FD %d: %s: no ACK for block %d",
               req->sock,
               ns_inet_ntoa((struct sockaddr *)&req->sa),
               req->block);
        return -1;
    }
    return req->pktsize;
}

static int
TFTPCmd(ClientData UNUSED(clientdata), Tcl_Interp *interp, int objc, Tcl_Obj *const objv[])
{
    Tcl_DString ds;
    TFTPRequest req;
    socklen_t salen = sizeof(struct sockaddr_in);
    char *address = NULL, *filename = NULL, *outfile = NULL;
    int len, port, total = 0, outfd = -1, rc = TCL_OK, blksize = 512, timeout = 5, dowrite = 0;
    ssize_t length;
    Ns_ObjvSpec opts[] = {
        {"-write",    Ns_ObjvBool,   &dowrite, NULL},
        {"-timeout",  Ns_ObjvInt,    &timeout, NULL},
        {"-blksize",  Ns_ObjvInt,    &blksize, NULL},
        {"-outfile",  Ns_ObjvString, &outfile, NULL},
        {"--",        Ns_ObjvBreak,   NULL,    NULL},
        {NULL, NULL, NULL, NULL}
    };
    Ns_ObjvSpec args[] = {
        {"address",  Ns_ObjvString, &address, NULL},
        {"port",     Ns_ObjvInt,    &port, NULL},
        {"filename", Ns_ObjvString, &filename, &len},
        {NULL, NULL, NULL, NULL}
    };

    if (Ns_ParseObjv(opts, args, interp, 1, objc, objv) != NS_OK) {
        return TCL_ERROR;
    }
    length = len;
    if (Ns_GetSockAddr((struct sockaddr *)&req.sa, address, (unsigned short)port) != NS_OK) {
        sprintf(req.content.data, "%s:%d", address, port);
        Tcl_AppendResult(interp, "invalid address ", req.content.data, 0);
        return TCL_ERROR;
    }
    if (outfile != NULL) {
        outfd = open(outfile, O_CREAT|O_RDWR, 0644);
        if (outfd == -1) {
            Tcl_AppendResult(interp, "error opening file ", outfile, ": ", strerror(errno), 0);
            return TCL_ERROR;
        }
    }
    req.sock = socket(AF_INET, SOCK_DGRAM, 0);
    if (req.sock < 0) {
        Tcl_AppendResult(interp, "socket error ", strerror(errno), 0);
        return TCL_ERROR;
    }
    req.content.var.ack.opcode = htons(1);
    length = snprintf(req.content.data, sizeof(req.content.data), "%s%cblksize%c%d%ctsize%c0%ctimeout%c%d",
                      filename, 0, 0, blksize, 0, 0, 0, 0, timeout);
    if (sendto(req.sock, (char*)&req.content.var.ack, (size_t)(length+5), 0, (struct sockaddr*)&req.sa, salen) < 0) {
        Tcl_AppendResult(interp, "sendto error ", strerror(errno), 0);
        return TCL_ERROR;
    }
    Tcl_DStringInit(&ds);
    do {
       if (Ns_SockWait(req.sock, NS_SOCK_READ, timeout) != NS_OK) {
           Tcl_DStringSetLength(&ds, 0);
           Ns_DStringPrintf(&ds, "timeout");
           rc = TCL_ERROR;
           goto done;
       }
           length = recvfrom(req.sock, &req.reply, sizeof(req.reply), 0, (struct sockaddr*)&req.sa, &salen);
           if (length > 0) {
               if (outfile != NULL) {
                   write(outfd, req.reply.data, (size_t)length);
               } else {
                   Tcl_DStringAppend(&ds, req.reply.data, (int)length);
               }
               total += length;
           }
    } while (1);
done:
    ns_sockclose(req.sock);
    if (outfile != NULL) {
        ns_sockclose(outfd);
        Tcl_SetObjResult(interp, Tcl_NewIntObj(total));
    } else {
        Tcl_SetObjResult(interp, Tcl_NewByteArrayObj((unsigned char*)ds.string, ds.length));
    }
    Tcl_DStringFree(&ds);
    return rc;
}

/*
 * Local Variables:
 * mode: c
 * c-basic-offset: 4
 * fill-column: 78
 * indent-tabs-mode: nil
 * End:
 */
