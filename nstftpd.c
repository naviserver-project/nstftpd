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
   int drivermode;
   int umask;
   int blksize;
   int timeout;
   int retries;
   int debug;
   int sock;
   int port;
   char *address;
   char *proc;
} TFTPServer;

typedef struct
{
   TFTPServer *server;
   char *file;
   char mode[16];
   int fd;
   int op;
   int sock;
   int block;
   short blksize;
   short timeout;
   struct stat fstat;
   struct sockaddr_in sa;
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
          };
        } ack;
     };
   };
   struct {
     unsigned short opcode;
     unsigned short block;
     char data[MAX_BLKSIZE];
   } reply;
} TFTPRequest;

static Ns_DriverProc TFTPProc;
static TFTPRequest *TFTPNew(TFTPServer *server);
static void TFTPProcessRequest(TFTPRequest *arg);
static void TFTPFree(TFTPRequest *req);
static int TFTPRequestProc(void *arg, Ns_Conn *conn);
static int TFTPRecv(TFTPRequest *req);
static int TFTPSend(TFTPRequest *req, char *buf, int len);
static int TFTPSendACK(TFTPRequest *req, char *buf, int len);
static int TFTPSendError(TFTPRequest *req, int errcode, char *msg, int err);
static int TFTPCallback(SOCKET sock, void *arg, int when);
static int TFTPInterpInit(Tcl_Interp *interp, void *arg);
static int TFTPCmd(ClientData arg, Tcl_Interp *interp,int objc,Tcl_Obj *CONST objv[]);

NS_EXPORT int Ns_ModuleVersion = 1;

NS_EXPORT int Ns_ModuleInit(char *server, char *module)
{
    char *path;
    Tcl_DString ds;
    TFTPServer *srvPtr;
    Ns_DriverInitData init;

    srvPtr = ns_calloc(1, sizeof(TFTPServer));
    path = Ns_ConfigGetPath(server,module,NULL);
    Ns_ConfigGetBool(path, "drivermode", &srvPtr->drivermode);
    srvPtr->debug = Ns_ConfigIntRange(path, "debug", 0, 0, 10);
    srvPtr->umask = Ns_ConfigIntRange(path, "umask", 0644, 0, INT_MAX);
    srvPtr->retries = Ns_ConfigIntRange(path, "retries", 1, 1, 10);
    srvPtr->blksize = Ns_ConfigIntRange(path, "blksize", 512, 512, MAX_BLKSIZE);
    srvPtr->timeout = Ns_ConfigIntRange(path, "timeout", 5, 1, 1000);
    srvPtr->rootpath = ns_strcopy(Ns_ConfigGetValue(path, "rootpath"));
    srvPtr->proc = Ns_ConfigGetValue(path, "proc");
    srvPtr->port = Ns_ConfigIntRange(path, "port", 69, 1, 99999);
    srvPtr->address = Ns_ConfigGetValue(path, "address");

    if (srvPtr->drivermode) {
        init.version = NS_DRIVER_VERSION_1;
        init.name = "nstftp";
        init.proc = TFTPProc;
        init.opts = NS_DRIVER_UDP;
        init.arg = srvPtr;
        init.path = NULL;
        if (Ns_DriverInit(server, module, &init) != NS_OK) {
            Ns_Log(Error, "nstftpd: driver init failed.");
            ns_free(srvPtr);
            return NS_ERROR;
        }
        Ns_RegisterRequest(server, "TFTP",  "/", TFTPRequestProc, NULL, srvPtr, 0);
    } else {
        if ((srvPtr->sock = Ns_SockListenUdp(srvPtr->address, srvPtr->port)) == -1) {
            Ns_Log(Error,"nstftp: %s:%d: couldn't create socket: %s", srvPtr->address, srvPtr->port, strerror(errno));
            ns_free(srvPtr);
            return NS_ERROR;
        }
        Ns_SockCallback(srvPtr->sock, TFTPCallback, srvPtr, NS_SOCK_READ|NS_SOCK_EXIT|NS_SOCK_EXCEPTION);
    }
    srvPtr->server = ns_strdup(server);
    Tcl_DStringInit(&ds);
    if (srvPtr->rootpath == NULL) {
        srvPtr->rootpath = ns_strcopy(Ns_PagePath(&ds, server, "", 0));
    }
    Ns_TclRegisterTrace(server, TFTPInterpInit, srvPtr, NS_TCL_TRACE_CREATE);
    Tcl_DStringFree(&ds);
    return NS_OK;
}

static int
TFTPInterpInit(Tcl_Interp *interp, void *arg)
{
    Tcl_CreateObjCommand(interp, "ns_tftp", TFTPCmd, arg, NULL);
    return NS_OK;
}

static int
TFTPProc(Ns_DriverCmd cmd, Ns_Sock *sock, struct iovec *bufs, int nbufs)
{
    int len;
    Ns_DString *ds;
    struct iovec iobuf;
    socklen_t salen = sizeof(struct sockaddr_in);

    switch (cmd) {
     case DriverAccept:
         /*
          * Read the packet and store it in the request buffer, registered proc
          * then will use that data for processing
          */

         if (Ns_DriverSockRequest(sock, "TFTP / TFTP/1.0") == NS_OK) {
             ds = Ns_DriverSockContent(sock);
             Tcl_DStringSetLength(ds, sock->driver->bufsize);
             iobuf.iov_base = ds->string;
             iobuf.iov_len = ds->length;
             ds->length = TFTPProc(DriverRecv, sock, &iobuf, 1);
             return NS_OK;
         }
         return NS_FATAL;

     case DriverRecv:
         len = recvfrom(sock->sock, bufs->iov_base, bufs->iov_len, 0, (struct sockaddr*)&sock->sa, (socklen_t*)&salen);
         if (len == -1) {
             Ns_Log(Error,"DriverRecv: %s: FD %d: recv from %s: %s", sock->driver->name, sock->sock, ns_inet_ntoa(sock->sa.sin_addr), strerror(errno));
         }
         return len;

     case DriverSend:
         len = sendto(sock->sock, bufs->iov_base, bufs->iov_len, 0, (struct sockaddr*)&sock->sa, sizeof(struct sockaddr_in));
         if (len == -1) {
             Ns_Log(Error,"DriverSend: %s: FD %d: sendto %d bytes to %s: %s", sock->driver->name, sock->sock, len, ns_inet_ntoa(sock->sa.sin_addr), strerror(errno));
         }
         return len;

     case DriverClose:
     case DriverKeep:
         break;
    }
    return NS_ERROR;
}

static int
TFTPRequestProc(void *arg, Ns_Conn *conn)
{
    TFTPServer *server = arg;
    Ns_Sock *sock = Ns_ConnSockPtr(conn);
    Ns_DString *ds = Ns_ConnSockContent(conn);
    TFTPRequest *req = TFTPNew(server);

    req->sa = sock->sa;
    if (ds != NULL) {
        /* Data was read in the driver's proc during accept */
        memcpy(req->data, ds->string, ds->length);
        req->pktsize = ds->length;
        TFTPProcessRequest(req);
        /* For access log file */
        if (req->file) {
            ns_free(conn->request->line);
            snprintf(req->data, sizeof(req->data), "%s %s TFTP/2.0", req->op == 1 ? "GET" : "PUT", req->file);
            conn->request->line = ns_strdup(req->data);
            Ns_ConnSetContentSent(conn, req->fstat.st_size);
        }
    } else {
        Ns_Log(Error, "TFTP: FD %d: %s: invalid connection", req->sock, ns_inet_ntoa(req->sa.sin_addr));
    }
    TFTPFree(req);
    return NS_OK;
}

static void
TFTPThread(void *arg)
{
    TFTPRequest *req = (TFTPRequest*)arg;

    Ns_ThreadSetName("tftp:%d:%s", htons(req->pkt.opcode), ns_inet_ntoa(req->sa.sin_addr));
    TFTPProcessRequest(req);
    TFTPFree(req);
}

static int
TFTPCallback(SOCKET sock, void *arg, int when)
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
    close(sock);
    return NS_FALSE;
}

static void
TFTPProcessRequest(TFTPRequest* req)
{
    TFTPServer *server = req->server;
    int rc, nread;
    char *str, *ptr;
    Ns_Time timeout;
    struct sockaddr_in sa;

    if (server->debug > 1) {
        Ns_Log(Notice, "TFTP: FD %d: %s: connected, %d bytes", req->sock, ns_inet_ntoa(req->sa.sin_addr), req->pktsize);
    }
    req->op = htons(req->pkt.opcode);
    /* Check request type */
    switch (req->op) {
     case 1:
         break;
     case 2:
         break;
     default:
         Ns_Log(Notice, "TFTP: FD %d: %s: invalid request, opcode=%d, %d bytes", req->sock, ns_inet_ntoa(req->sa.sin_addr), htons(req->pkt.opcode), req->pktsize);
         req->sock = -1;
         goto done;
    }
    req->sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    sa.sin_family = AF_INET;
    sa.sin_addr.s_addr = INADDR_ANY;
    sa.sin_port = 0;
    if (bind(req->sock, (struct sockaddr*)&sa, sizeof(sa)) == -1) {
        Ns_Log(Notice, "TFTP: FD %d: %s: bind error: %s", req->sock, ns_inet_ntoa(req->sa.sin_addr), strerror(errno));
        goto done;
    }
    connect(req->sock, (struct sockaddr*)&req->sa, sizeof(struct sockaddr_in));

    ptr = req->data + 2;
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
            rc = Tcl_VarEval(interp, server->proc, " ", req->op == 1 ? "r" : "w", " {", req->file, "} ", ns_inet_ntoa(req->sa.sin_addr), 0);
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

    req->fd = open(req->file, req->op == 1 ? O_RDONLY : O_CREAT|O_RDWR, server->umask);
    if (req->fd <= 0) {
        TFTPSendError(req, 1, "Invalid File", errno);
        goto done;
    }
    fstat(req->fd, &req->fstat);
    if (server->debug > 2) {
        Ns_Log(Notice,"TFTP: FD %d: %s: file %s, size %lu", req->sock, ns_inet_ntoa(req->sa.sin_addr), req->file, req->fstat.st_size);
    }

    /*
     * Parse TFTP parameters
     */

    if (*ptr) {
    	memset(&req->pkt, 0, sizeof(req->pkt));
    	req->pkt.opcode = htons(6);
    	str = req->ack.data;
    	while (*ptr) {
    	    if (!strcasecmp(ptr, "blksize")) {
    	    	strcpy(str, ptr);
    	    	str += strlen(str) + 1;
    	    	ptr += strlen(ptr) + 1;
    	    	req->blksize = atoi(ptr);
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
    	    	sprintf(str, "%lu", req->fstat.st_size);
    	    	str += strlen(str) + 1;
    	    } else
            if (!strcasecmp(ptr, "timeout")) {
    	    	strcpy(str, ptr);
    	    	str += strlen(str) + 1;
    	    	ptr += strlen(ptr) + 1;
    	    	req->timeout = atoi(ptr);
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
        if (TFTPSendACK(req, (char*)&req->pkt, str - (char*)&req->pkt) == -1) {
            goto done;
        }
        if (server->debug > 3) {
            Ns_Log(Notice, "TFTP: FD %d: %s: parameters: timeout=%d blksize=%d", req->sock, ns_inet_ntoa(req->sa.sin_addr), req->timeout, req->blksize);
        }
    } else
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
             nread = read(req->fd, req->reply.data, req->blksize);
             if (nread <= 0) {
                 Ns_Log(Error, "TFTP: FD %d: %s: read: %s", req->sock, ns_inet_ntoa(req->sa.sin_addr), strerror(errno));
                 break;
             }
             // Last block read
             if (nread < req->blksize) {
                 close(req->fd);
                 req->fd = -1;
             }
             // Send ACK and wait for the next block
             if (TFTPSendACK(req, (char*)&req->reply, nread + 4) == -1) {
                 break;
             }
         }
         break;

     case 2:
         while (req->fd > 0) {
             Ns_GetTime(&timeout);
             Ns_IncrTime(&timeout, req->timeout, 0);
             if (Ns_SockTimedWait(req->sock, NS_SOCK_READ, &timeout) != NS_OK) {
                 Ns_Log(Error, "TFTP: FD %d: %s: timeout %d secs", req->sock, ns_inet_ntoa(req->sa.sin_addr), req->timeout);
                 goto done;
             }
             if (TFTPRecv(req) <= 0) {
                 goto done;
             }
             if (htons(req->pkt.opcode) == 3) {
                 if (htons(req->pkt.block) == req->block + 1) {
                     if (req->pktsize > 4) {
                         if (write(req->fd, req->pkt.data, req->pktsize - 4) == -1) {
                             TFTPSendError(req, 3, "Unable to write", errno);
                             goto done;
                         }
                         req->block++;
                     }
                     if (req->block == USHRT_MAX) {
                         TFTPSendError(req, 3, "File Too Large", EFBIG);
                         goto done;
                     }
                     if (req->pktsize - 4 < req->blksize) {
                         close(req->fd);
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
        Ns_Log(Notice, "TFTP: FD %d: %s: disconnected", req->sock, ns_inet_ntoa(req->sa.sin_addr));
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
            close(req->sock);
        }
        if (req->fd > 0) {
    	    close(req->fd);
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

    req->pktsize = recvfrom(req->sock, req->data, sizeof(req->data), 0, (struct sockaddr*)&req->sa, &salen);
    if (req->pktsize <= 0) {
        Ns_Log(Error,"TFTP: FD %d: %s: recvfrom: %s", req->sock, ns_inet_ntoa(req->sa.sin_addr), strerror(errno));
        return -1;
    }
    if (req->server->debug > 5) {
        Ns_Log(Notice,"TFTP: FD %d: %s: recv: block %d, op %d, %d bytes", req->sock, ns_inet_ntoa(req->sa.sin_addr), req->block, htons(req->pkt.opcode), req->pktsize);
    }
    return req->pktsize;
}

static int
TFTPSend(TFTPRequest *req, char *buf, int len)
{
    int nsent;

    nsent = sendto(req->sock, buf, len, 0, (struct sockaddr*)&req->sa, sizeof(struct sockaddr_in));
    if (nsent <= 0) {
        Ns_Log(Error, "TFTP: FD %d: %s: sendto: len=%d: %s", req->sock, ns_inet_ntoa(req->sa.sin_addr), len, strerror(errno));
        return -1;
    }
    if (req->server->debug > 5) {
        Ns_Log(Notice,"TFTP: FD %d: %s: send: block %d, %d bytes", req->sock, ns_inet_ntoa(req->sa.sin_addr), req->block, nsent);
    }
    return nsent;
}

static int
TFTPSendError(TFTPRequest *req, int errcode, char *msg, int err)
{
    int nsent;

    req->reply.opcode = htons(5);
    req->reply.block = htons(errcode);
    strcpy(req->reply.data, msg);
    nsent = sendto(req->sock, (char*)&req->reply, strlen(req->reply.data)+5, 0, (struct sockaddr*)&req->sa, sizeof(struct sockaddr_in));
    Ns_Log(Error,"TFTP: FD %d: %s: %s %s: %s", req->sock, ns_inet_ntoa(req->sa.sin_addr), msg, req->file, strerror(err));
    return nsent;
}

static int
TFTPSendACK(TFTPRequest *req, char *buf, int len)
{
    Ns_Time timeout;
    int retries = 0;

    do {
       if (TFTPSend(req, buf, len) <= 0) {
           return -1;
       }
       do {
          Ns_GetTime(&timeout);
          Ns_IncrTime(&timeout, req->timeout, 0);
          if (Ns_SockTimedWait(req->sock, NS_SOCK_READ, &timeout) != NS_OK) {
              Ns_Log(Error, "TFTP: FD %d: %s: timeout %d secs", req->sock, ns_inet_ntoa(req->sa.sin_addr), req->timeout);
              return 0;
          }
          if (TFTPRecv(req) <= 0) {
              return -1;
          }
       } while (htons(req->pkt.opcode) != 4 && htons(req->pkt.block) != req->block);
    } while (req->pktsize == 0 && retries++ < req->server->retries);
    if (req->pktsize <= 0) {
        Ns_Log(Error,"TFTP: FD %d: %s: no ACK for block %d", req->sock, ns_inet_ntoa(req->sa.sin_addr), req->block);
        return -1;
    }
    return req->pktsize;
}

static int
TFTPCmd(ClientData arg, Tcl_Interp *interp, int objc, Tcl_Obj *CONST objv[])
{
    Ns_Time now;
    Tcl_DString ds;
    TFTPRequest req;
    socklen_t salen = sizeof(struct sockaddr_in);
    char *address = NULL, *filename = NULL, *outfile = NULL;
    int len, port, total = 0, outfd = -1, rc = TCL_OK, blksize = 512, timeout = 5, dowrite = 0;

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
    if (Ns_GetSockAddr(&req.sa, address, port) != NS_OK) {
        sprintf(req.data, "%s:%d", address, port);
        Tcl_AppendResult(interp, "invalid address ", req.data, 0);
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
    req.ack.opcode = htons(1);
    len = snprintf(req.ack.data, sizeof(req.pkt.data), "%s%cblksize%c%d%ctsize%c0%ctimeout%c%d",
                   filename, 0, 0, blksize, 0, 0, 0, 0, timeout);
    if (sendto(req.sock, (char*)&req.ack, len+5, 0, (struct sockaddr*)&req.sa, salen) < 0) {
        Tcl_AppendResult(interp, "sendto error ", strerror(errno), 0);
        return TCL_ERROR;
    }
    Tcl_DStringInit(&ds);
    do {
       Ns_GetTime(&now);
       Ns_IncrTime(&now, timeout, 0);
       if (Ns_SockTimedWait(req.sock, NS_SOCK_READ, &now) != NS_OK) {
           Tcl_DStringSetLength(&ds, 0);
           Ns_DStringPrintf(&ds, "timeout");
           rc = TCL_ERROR;
           goto done;
       }
           len = recvfrom(req.sock, &req.reply, sizeof(req.reply), 0, (struct sockaddr*)&req.sa, &salen);
           if (len > 0) {
               if (outfile != NULL) {
                   write(outfd, req.reply.data, len);
               } else {
                   Tcl_DStringAppend(&ds, req.reply.data, len);
               }
               total += len;
           }
    } while (1);
done:
    close(req.sock);
    if (outfile != NULL) {
        close(outfd);
        Tcl_SetObjResult(interp, Tcl_NewIntObj(total));
    } else {
        Tcl_SetObjResult(interp, Tcl_NewByteArrayObj((unsigned char*)ds.string, ds.length));
    }
    Tcl_DStringFree(&ds);
    return rc;
}
