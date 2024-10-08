/* $OpenBSD: netcat.c,v 1.89 2007/02/20 14:11:17 jmc Exp $ */
/*
 * Copyright (c) 2001 Eric Jackson <ericj@monkey.org>
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 *
 * 1. Redistributions of source code must retain the above copyright
 *   notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *   notice, this list of conditions and the following disclaimer in the
 *   documentation and/or other materials provided with the distribution.
 * 3. The name of the author may not be used to endorse or promote products
 *   derived from this software without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE AUTHOR ``AS IS'' AND ANY EXPRESS OR
 * IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES
 * OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE DISCLAIMED.
 * IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR ANY DIRECT, INDIRECT,
 * INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT
 * NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
 * DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
 * THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF
 * THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

/*
 * Re-written nc(1) for OpenBSD. Original implementation by
 * *Hobbit* <hobbit@avian.org>.
 */

#include <sys/socket.h>
#include <sys/time.h>
#include <sys/types.h>
#include <sys/un.h>

#include <arpa/inet.h>
#include <arpa/telnet.h>
#include <netinet/in.h>
#include <netinet/in_systm.h>
#include <netinet/ip.h>
#include <netinet/tcp.h>

#include "atomicio.h"
#include <err.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <netdb.h>
#include <poll.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <glib.h>

#ifndef SUN_LEN
#define SUN_LEN(su)                                                            \
  (sizeof(*(su)) - sizeof((su)->sun_path) + strlen((su)->sun_path))
#endif

#define PORT_MAX 65535
#define PORT_MAX_LEN 6

#define CONNECTION_SUCCESS 0
#define CONNECTION_FAILED 1
#define CONNECTION_TIMEOUT 2

#define UDP_SCAN_TIMEOUT 3 /* Seconds */

/* Command Line Options */
int Cflag = 0;  /* CRLF line-ending */
int dflag;      /* detached, no stdin */
int iflag;      /* Interval Flag */
int jflag;      /* use jumbo frames if we can */
int kflag;      /* More than one connect */
int lflag;      /* Bind to local port */
int nflag;      /* Don't do name look up */
char *Pflag;    /* Proxy username */
char *pflag;    /* Localport flag */
int qflag = -1; /* Quit after some secs */
int rflag;      /* Random ports flag */
char *sflag;    /* Source Address */
int tflag;      /* Telnet Emulation */
int uflag;      /* UDP - Default to TCP */
int vflag;      /* Verbosity */
int xflag;      /* Socks proxy */
int zflag;      /* Port Scan Flag */
int Dflag;      /* sodebug */
int Sflag;      /* TCP MD5 signature option */
int Tflag = -1; /* IP Type of Service */

int timeout = -1;
int family = AF_UNSPEC;
char *portlist[PORT_MAX + 1];

void atelnet(int, unsigned char *, unsigned int);
void build_ports(char *);
void help(void);
int local_listen(char *, char *, struct addrinfo);
void readwrite(int);
int remote_connect(const char *, const char *, struct addrinfo);
int socks_connect(const char *, const char *, struct addrinfo, const char *,
                  const char *, struct addrinfo, int, const char *);
int udptest(int);
int unix_connect(char *);
int unix_listen(char *);
void set_common_sockopts(int);
int parse_iptos(char *);
void report_sock(const char *, const struct sockaddr *, socklen_t, char *);
void usage(int);
char *proto_name(int);

static int connect_with_timeout(int fd, const struct sockaddr *sa,
                                socklen_t salen, int ctimeout);
static void quit();

int main(int argc, char *argv[]) {
  int ch, s, ret, socksv;
  char *host, *uport, *endp;
  struct addrinfo hints;
  struct servent *sv;
  socklen_t len;
  struct sockaddr_storage cliaddr;
  char *proxy = NULL;
  const char *proxyhost = "", *proxyport = NULL;
  struct addrinfo proxyhints;

  ret = 1;
  s = -1;
  socksv = 5;
  host = NULL;
  uport = NULL;
  endp = NULL;
  sv = NULL;

  while ((ch = getopt(argc, argv, "46Ddhi:jklnP:p:q:rSs:tT:UuZvw:X:x:zC")) !=
         -1) {
    switch (ch) {
    case '4':
      family = AF_INET;
      break;
    case '6':
      family = AF_INET6;
      break;
    case 'U':
      family = AF_UNIX;
      break;
    case 'X':
      if (strcasecmp(optarg, "connect") == 0)
        socksv = -1; /* HTTP proxy CONNECT */
      else if (strcmp(optarg, "4") == 0)
        socksv = 4; /* SOCKS v.4 */
      else if (strcmp(optarg, "5") == 0)
        socksv = 5; /* SOCKS v.5 */
      else
        errx(1, "unsupported proxy protocol");
      break;
    case 'd':
      dflag = 1;
      break;
    case 'h':
      help();
      break;
    case 'i':
      iflag = (int)strtoul(optarg, &endp, 10);
      if (iflag < 0 || *endp != '\0')
        errx(1, "interval cannot be negative");
      break;
    case 'j':
      jflag = 1;
      break;
    case 'k':
      kflag = 1;
      break;
    case 'l':
      lflag = 1;
      break;
    case 'n':
      nflag = 1;
      break;
    case 'P':
      Pflag = optarg;
      break;
    case 'p':
      pflag = optarg;
      break;
    case 'q':
      qflag = (int)strtoul(optarg, &endp, 10);
      break;
    case 'r':
      rflag = 1;
      break;
    case 's':
      sflag = optarg;
      break;
    case 't':
      tflag = 1;
      break;
    case 'u':
      uflag = 1;
      break;
    case 'v':
      vflag = 1;
      break;
    case 'w':
      timeout = (int)strtoul(optarg, &endp, 10);
      if (timeout < 0 || *endp != '\0')
        errx(1, "timeout cannot be negative");
      if (timeout >= (INT_MAX / 1000))
        errx(1, "timeout too large");
      timeout *= 1000;
      break;
    case 'x':
      xflag = 1;
      if ((proxy = strdup(optarg)) == NULL)
        err(1, NULL);
      break;
    case 'z':
      zflag = 1;
      break;
    case 'D':
      Dflag = 1;
      break;
    case 'S':
      Sflag = 1;
      break;
    case 'T':
      Tflag = parse_iptos(optarg);
      break;
    case 'C':
      Cflag = 1;
      break;
    default:
      usage(1);
    }
  }
  argc -= optind;
  argv += optind;

  /* Cruft to make sure options are clean, and used properly. */
  if (argc == 1 && family == AF_UNIX) {
    host = argv[0];
  } else if (argc == 1 && lflag) {
    uport = argv[0];
  } else if (argc == 2) {
    host = argv[0];
    uport = argv[1];
  } else
    usage(1);

  if (lflag && sflag)
    errx(1, "cannot use -s and -l");
  if (lflag && pflag)
    errx(1, "cannot use -p and -l");
  if (lflag && zflag)
    errx(1, "cannot use -z and -l");
  if (!lflag && kflag)
    errx(1, "must use -l with -k");

  /* Initialize addrinfo structure. */
  if (family != AF_UNIX) {
    memset(&hints, 0, sizeof(struct addrinfo));
    hints.ai_family = family;
    hints.ai_socktype = uflag ? SOCK_DGRAM : SOCK_STREAM;
    hints.ai_protocol = uflag ? IPPROTO_UDP : IPPROTO_TCP;
    if (nflag)
      hints.ai_flags |= AI_NUMERICHOST;
  }

  if (xflag) {
    if (uflag)
      errx(1, "no proxy support for UDP mode");

    if (lflag)
      errx(1, "no proxy support for listen");

    if (family == AF_UNIX)
      errx(1, "no proxy support for unix sockets");

    /* XXX IPv6 transport to proxy would probably work */
    if (family == AF_INET6)
      errx(1, "no proxy support for IPv6");

    if (sflag)
      errx(1, "no proxy support for local source address");

    proxyhost = strsep(&proxy, ":");
    proxyport = proxy;

    memset(&proxyhints, 0, sizeof(struct addrinfo));
    proxyhints.ai_family = family;
    proxyhints.ai_socktype = SOCK_STREAM;
    proxyhints.ai_protocol = IPPROTO_TCP;
    if (nflag)
      proxyhints.ai_flags |= AI_NUMERICHOST;
  }

  if (lflag) {
    int connfd;
    ret = 0;

    if (family == AF_UNIX)
      s = unix_listen(host);

    /* Allow only one connection at a time, but stay alive. */
    for (;;) {
      if (family != AF_UNIX) {
        if (s != -1)
          close(s);
        s = local_listen(host, uport, hints);
      }
      if (s < 0)
        err(1, NULL);
      /*
       * For UDP, we will use recvfrom() initially
       * to wait for a caller, then use the regular
       * functions to talk to the caller.
       */
      if (uflag) {
        int rv, plen;
        char buf[8192];

        len = sizeof(cliaddr);
        plen = jflag ? 8192 : 1024;
        rv =
            recvfrom(s, buf, plen, MSG_PEEK, (struct sockaddr *)&cliaddr, &len);
        if (rv < 0)
          err(1, "recvfrom");

        rv = connect(s, (struct sockaddr *)&cliaddr, len);
        if (rv < 0)
          err(1, "connect");

        connfd = s;
      } else {
        len = sizeof(cliaddr);
        connfd = accept(s, (struct sockaddr *)&cliaddr, &len);
      }

      if (vflag) {
        if (vflag)
          report_sock("Connection received", (struct sockaddr *)&cliaddr, len,
                      family == AF_UNIX ? host : NULL);
      }

      readwrite(connfd);
      close(connfd);
      if (family != AF_UNIX)
        close(s);

      if (!kflag)
        break;
    }
  } else if (family == AF_UNIX) {
    ret = 0;

    if ((s = unix_connect(host)) > 0 && !zflag) {
      readwrite(s);
      close(s);
    } else
      ret = 1;

    exit(ret);

  } else {
    int i = 0;

    /* Construct the portlist[] array. */
    build_ports(uport);

    /* Cycle through portlist, connecting to each port. */
    for (i = 0; portlist[i] != NULL; i++) {
      if (s)
        close(s);

      if (xflag)
        s = socks_connect(host, portlist[i], hints, proxyhost, proxyport,
                          proxyhints, socksv, Pflag);
      else
        s = remote_connect(host, portlist[i], hints);

      if (s < 0)
        continue;

      ret = 0;
      if (vflag || zflag) {
        int print_info = 1;
        /* For UDP, make sure we are connected. */
        if (uflag) {
          /* No info on failed or skipped test. */
          if ((print_info = udptest(s)) == -1) {
            ret = 1;
            continue;
          }
        }

        /* Don't look up port if -n. */
        if (nflag)
          sv = NULL;
        else {
          sv = getservbyport(ntohs(atoi(portlist[i])), uflag ? "udp" : "tcp");
        }

        if (print_info == 1)
          fprintf(stderr,
                  "Connection to %s %s port [%s/%s] "
                  "succeeded!\n",
                  host, portlist[i], uflag ? "udp" : "tcp",
                  sv ? sv->s_name : "*");
      }
      if (!zflag)
        readwrite(s);
    }
  }

  if (s != -1)
    close(s);

  exit(ret);
}

/*
 * unix_connect()
 * Returns a socket connected to a local unix socket. Returns -1 on failure.
 */
int unix_connect(char *path) {
  struct sockaddr_un sun;
  int s;

  if ((s = socket(AF_UNIX, SOCK_STREAM, 0)) < 0)
    return (-1);
  (void)fcntl(s, F_SETFD, 1);

  memset(&sun, 0, sizeof(struct sockaddr_un));
  sun.sun_family = AF_UNIX;

  if (g_strlcpy(sun.sun_path, path, sizeof(sun.sun_path)) >=
      sizeof(sun.sun_path)) {
    close(s);
    errno = ENAMETOOLONG;
    return (-1);
  }
  if (connect(s, (struct sockaddr *)&sun, SUN_LEN(&sun)) < 0) {
    close(s);
    return (-1);
  }
  return (s);
}

/*
 * unix_listen()
 * Create a unix domain socket, and listen on it.
 */
int unix_listen(char *path) {
  struct sockaddr_un sun;
  int s;

  /* Create unix domain socket. */
  if ((s = socket(AF_UNIX, SOCK_STREAM, 0)) < 0)
    return (-1);

  memset(&sun, 0, sizeof(struct sockaddr_un));
  sun.sun_family = AF_UNIX;

  if (g_strlcpy(sun.sun_path, path, sizeof(sun.sun_path)) >=
      sizeof(sun.sun_path)) {
    close(s);
    errno = ENAMETOOLONG;
    return (-1);
  }

  if (bind(s, (struct sockaddr *)&sun, SUN_LEN(&sun)) < 0) {
    close(s);
    return (-1);
  }

  if (listen(s, 5) < 0) {
    close(s);
    return (-1);
  }
  return (s);
}

char *proto_name(int uflag) {

  char *proto = NULL;
  if (uflag) {
    proto = "udp";
  } else {
    proto = "tcp";
  }

  return proto;
}

/*
 * remote_connect()
 * Returns a socket connected to a remote host. Properly binds to a local
 * port or source address if needed. Returns -1 on failure.
 */
int remote_connect(const char *host, const char *port, struct addrinfo hints) {
  struct addrinfo *res, *res0;
  int s, error;

  if ((error = getaddrinfo(host, port, &hints, &res)))
    errx(1, "getaddrinfo: %s", gai_strerror(error));

  res0 = res;
  do {
    if ((s = socket(res0->ai_family, res0->ai_socktype, res0->ai_protocol)) < 0)
      continue;

    /* Bind to a local port or source address if specified. */
    if (sflag || pflag) {
      struct addrinfo ahints, *ares;

      memset(&ahints, 0, sizeof(struct addrinfo));
      ahints.ai_family = res->ai_family;
      ahints.ai_socktype = uflag ? SOCK_DGRAM : SOCK_STREAM;
      ahints.ai_protocol = uflag ? IPPROTO_UDP : IPPROTO_TCP;
      ahints.ai_flags = AI_PASSIVE;
      if ((error = getaddrinfo(sflag, pflag, &ahints, &ares)))
        errx(1, "getaddrinfo: %s", gai_strerror(error));

      if (bind(s, (struct sockaddr *)ares->ai_addr, ares->ai_addrlen) < 0)
        errx(1, "bind failed: %s", strerror(errno));
      freeaddrinfo(ares);
    }

    set_common_sockopts(s);
    char *proto = proto_name(uflag);

    if ((error = connect_with_timeout(s, res0->ai_addr, res0->ai_addrlen,
                                      timeout)) == CONNECTION_SUCCESS) {
      break;
    } else if (vflag && error == CONNECTION_FAILED) {
      warn("connect to %s port %s (%s) failed", host, port, proto);
    } else if (vflag && error == CONNECTION_TIMEOUT) {
      warn("connect to %s port %s (%s) timed out", host, port, proto);
    }

    close(s);
    s = -1;
  } while ((res0 = res0->ai_next) != NULL);

  freeaddrinfo(res);

  return (s);
}

static int connect_with_timeout(int fd, const struct sockaddr *sa,
                                socklen_t salen, int ctimeout) {
  int err;
  struct timeval tv, *tvp = NULL;
  fd_set connect_fdset;
  socklen_t len;
  int orig_flags;

  orig_flags = fcntl(fd, F_GETFL, 0);
  if (fcntl(fd, F_SETFL, orig_flags | O_NONBLOCK) < 0) {
    warn("can't set O_NONBLOCK - timeout not avaliable");
    if (connect(fd, sa, salen) == 0)
      return CONNECTION_SUCCESS;
    else
      return CONNECTION_FAILED;
  }

  /* set connect timeout */
  if (ctimeout > 0) {
    tv.tv_sec = (time_t)ctimeout / 1000;
    tv.tv_usec = 0;
    tvp = &tv;
  }

  /* attempt the connection */
  err = connect(fd, sa, salen);

  if (err != 0 && errno == EINPROGRESS) {
    /* connection is proceeding
     * it is complete (or failed) when select returns */

    /* initialize connect_fdset */
    FD_ZERO(&connect_fdset);
    FD_SET(fd, &connect_fdset);

    /* call select */
    do {
      err = select(fd + 1, NULL, &connect_fdset, NULL, tvp);
    } while (err < 0 && errno == EINTR);

    /* select error */
    if (err < 0)
      errx(1, "select error: %s", strerror(errno));

    /* we have reached a timeout */
    if (err == 0)
      return CONNECTION_TIMEOUT;

    /* select returned successfully, but we must test socket
     * error for result */
    len = sizeof(err);
    if (getsockopt(fd, SOL_SOCKET, SO_ERROR, &err, &len) < 0)
      errx(1, "getsockopt error: %s", strerror(errno));

    /* setup errno according to the result returned by
     * getsockopt */
    if (err != 0)
      errno = err;
  }

  /* return aborted if an error occured, and valid otherwise */
  fcntl(fd, F_SETFL, orig_flags);
  return (err != 0) ? CONNECTION_FAILED : CONNECTION_SUCCESS;
}

/*
 * local_listen()
 * Returns a socket listening on a local port, binds to specified source
 * address. Returns -1 on failure.
 */
int local_listen(char *host, char *port, struct addrinfo hints) {
  struct addrinfo *res, *res0;
  int s, ret, x = 1;
  int error;

  /* Allow nodename to be null. */
  hints.ai_flags |= AI_PASSIVE;

  /*
   * In the case of binding to a wildcard address
   * default to binding to an ipv4 address.
   */
  if (host == NULL && hints.ai_family == AF_UNSPEC)
    hints.ai_family = AF_INET;

  if ((error = getaddrinfo(host, port, &hints, &res)))
    errx(1, "getaddrinfo: %s", gai_strerror(error));

  res0 = res;
  do {
    if ((s = socket(res0->ai_family, res0->ai_socktype, res0->ai_protocol)) < 0)
      continue;

    ret = setsockopt(s, SOL_SOCKET, SO_REUSEADDR, &x, sizeof(x));
    if (ret == -1)
      err(1, NULL);
#ifdef SO_REUSEPORT
    ret = setsockopt(s, SOL_SOCKET, SO_REUSEPORT, &x, sizeof(x));
    if (ret == -1)
      err(1, NULL);
#endif
    set_common_sockopts(s);

    if (bind(s, (struct sockaddr *)res0->ai_addr, res0->ai_addrlen) == 0)
      break;

    close(s);
    s = -1;
  } while ((res0 = res0->ai_next) != NULL);

  if (!uflag && s != -1) {
    if (listen(s, 1) < 0)
      err(1, "listen");
  }

  if (vflag && s != -1) {
    struct sockaddr_storage ss;
    socklen_t len;

    len = sizeof(ss);
    if (getsockname(s, (struct sockaddr *)&ss, &len) == -1)
      err(1, "getsockname");
    report_sock(uflag ? "Bound" : "Listening", (struct sockaddr *)&ss, len,
                NULL);
  }

  freeaddrinfo(res);

  return (s);
}

/*
 * readwrite()
 * Loop that polls on the network file descriptor and stdin.
 */
void readwrite(int nfd) {
  struct pollfd pfd[2];
  unsigned char buf[8192];
  int n, wfd = fileno(stdin);
  int lfd = fileno(stdout);
  int plen;

  plen = jflag ? 8192 : 1024;

  /* Setup Network FD */
  pfd[0].fd = nfd;
  pfd[0].events = POLLIN;

  /* Set up STDIN FD. */
  pfd[1].fd = wfd;
  pfd[1].events = POLLIN;

  while (pfd[0].fd != -1) {
    if (iflag)
      sleep(iflag);

    if ((n = poll(pfd, 2 - dflag, timeout)) < 0) {
      close(nfd);
      err(1, "Polling Error");
    }

    if (n == 0)
      return;

    if (pfd[0].revents & POLLIN) {
      if ((n = read(nfd, buf, plen)) < 0)
        return;
      else if (n == 0) {
        goto shutdown_rd;
      } else {
        if (tflag)
          atelnet(nfd, buf, n);
        if (atomicio(vwrite, lfd, buf, n) != n)
          return;
      }
    } else if (pfd[0].revents & POLLHUP) {
    shutdown_rd:
      shutdown(nfd, SHUT_RD);
      pfd[0].fd = -1;
      pfd[0].events = 0;
    }

    if (!dflag) {
      if (pfd[1].revents & POLLIN) {
        if ((n = read(wfd, buf, plen)) < 0)
          return;
        else if (n == 0) {
          goto shutdown_wr;
        } else {
          if ((Cflag) && (buf[n - 1] == '\n')) {
            if (atomicio(vwrite, nfd, buf, n - 1) != (n - 1))
              return;
            if (atomicio(vwrite, nfd, "\r\n", 2) != 2)
              return;
          } else {
            if (atomicio(vwrite, nfd, buf, n) != n)
              return;
          }
        }
      } else if (pfd[1].revents & POLLHUP) {
      shutdown_wr:
        /* if user asked to die after a while, arrange for it */
        if (qflag > 0) {
          signal(SIGALRM, quit);
          alarm(qflag);
        } else {
          shutdown(nfd, SHUT_WR);
        }
        pfd[1].fd = -1;
        pfd[1].events = 0;
      }
    }
  }
}

/* Deal with RFC 854 WILL/WONT DO/DONT negotiation. */
void atelnet(int nfd, unsigned char *buf, unsigned int size) {
  unsigned char *p, *end;
  unsigned char obuf[4];

  end = buf + size;
  obuf[0] = '\0';

  for (p = buf; p < end; p++) {
    if (*p != IAC)
      break;

    obuf[0] = IAC;
    p++;
    if ((*p == WILL) || (*p == WONT))
      obuf[1] = DONT;
    if ((*p == DO) || (*p == DONT))
      obuf[1] = WONT;
    p++;
    obuf[2] = *p;
    obuf[3] = '\0';
    if (atomicio(vwrite, nfd, obuf, 3) != 3)
      warn("Write Error!");
    obuf[0] = '\0';
  }
}

/*
 * build_ports()
 * Build an array or ports in portlist[], listing each port
 * that we should try to connect to.
 */
void build_ports(char *p) {
  struct servent *sv;
  char *n, *endp;
  int hi, lo, cp;
  int x = 0;
  char *proto = proto_name(uflag);
  sv = getservbyname(p, proto);
  if (sv) {
    portlist[0] = calloc(1, PORT_MAX_LEN);
    if (portlist[0] == NULL)
      err(1, NULL);
    snprintf(portlist[0], PORT_MAX_LEN, "%d", ntohs(sv->s_port));
  } else if ((n = strchr(p, '-')) != NULL) {
    if (lflag)
      errx(1, "Cannot use -l with multiple ports!");

    *n = '\0';
    n++;

    /* Make sure the ports are in order: lowest->highest. */
    hi = (int)strtoul(n, &endp, 10);
    if (hi <= 0 || hi > PORT_MAX || *endp != '\0')
      errx(1, "port range not valid");
    lo = (int)strtoul(p, &endp, 10);
    if (lo <= 0 || lo > PORT_MAX || *endp != '\0')
      errx(1, "port range not valid");

    if (lo > hi) {
      cp = hi;
      hi = lo;
      lo = cp;
    }

    /* Load ports sequentially. */
    for (cp = lo; cp <= hi; cp++) {
      portlist[x] = calloc(1, PORT_MAX_LEN);
      if (portlist[x] == NULL)
        err(1, NULL);
      snprintf(portlist[x], PORT_MAX_LEN, "%d", cp);
      x++;
    }

    /* Randomly swap ports. */
    if (rflag) {
      int y;
      char *c;

      for (x = 0; x <= (hi - lo); x++) {
        /* use random instead of arc4random */
        y = (random() & 0xFFFF) % (hi - lo);
        c = portlist[x];
        portlist[x] = portlist[y];
        portlist[y] = c;
      }
    }
  } else {
    hi = (int)strtoul(p, &endp, 10);
    if (hi <= 0 || hi > PORT_MAX || *endp != '\0')
      errx(1, "port range not valid");
    portlist[0] = calloc(1, PORT_MAX_LEN);
    if (portlist[0] == NULL)
      err(1, NULL);
    portlist[0] = p;
  }
}

/*
 * udptest()
 * Do a few writes to see if the UDP port is there.
 * XXX - Better way of doing this? Doesn't work for IPv6.
 * Also fails after around 100 ports checked.
 */
int udptest(int s) {
  int i, t;

  if ((write(s, "X", 1) != 1) ||
      ((write(s, "X", 1) != 1) && (errno == ECONNREFUSED)))
    return -1;

  /* Give the remote host some time to reply. */
  for (i = 0, t = (timeout == -1) ? UDP_SCAN_TIMEOUT : (timeout / 1000); i < t;
       i++) {
    sleep(1);
    if ((write(s, "X", 1) != 1) && (errno == ECONNREFUSED))
      return -1;
  }
  return 1;
}

void set_common_sockopts(int s) {
  int x = 1;

#ifdef TCP_MD5SIG
  if (Sflag) {
    if (setsockopt(s, IPPROTO_TCP, TCP_MD5SIG, &x, sizeof(x)) == -1)
      err(1, NULL);
  }
#endif
  if (Dflag) {
    if (setsockopt(s, SOL_SOCKET, SO_DEBUG, &x, sizeof(x)) == -1)
      err(1, NULL);
  }
#ifdef SO_JUMBO
  if (jflag) {
    if (setsockopt(s, SOL_SOCKET, SO_JUMBO, &x, sizeof(x)) == -1)
      err(1, NULL);
  }
#endif
  if (Tflag != -1) {
    if (setsockopt(s, IPPROTO_IP, IP_TOS, &Tflag, sizeof(Tflag)) == -1)
      err(1, "set IP ToS");
  }
}

int parse_iptos(char *s) {
  int tos = -1;

  if (strcmp(s, "lowdelay") == 0)
    return (IPTOS_LOWDELAY);
  if (strcmp(s, "throughput") == 0)
    return (IPTOS_THROUGHPUT);
  if (strcmp(s, "reliability") == 0)
    return (IPTOS_RELIABILITY);

  if (sscanf(s, "0x%x", &tos) != 1 || tos < 0 || tos > 0xff)
    errx(1, "invalid IP Type of Service");
  return (tos);
}

void report_sock(const char *msg, const struct sockaddr *sa, socklen_t salen,
                 char *path) {
  char host[NI_MAXHOST], port[NI_MAXSERV];
  int herr;
  int flags = NI_NUMERICSERV;

  if (path != NULL) {
    fprintf(stderr, "%s on %s\n", msg, path);
    return;
  }

  if (nflag)
    flags |= NI_NUMERICHOST;

  herr = getnameinfo(sa, salen, host, sizeof(host), port, sizeof(port), flags);
  switch (herr) {
  case 0:
    break;
  case EAI_SYSTEM:
    err(1, "getnameinfo");
  default:
    errx(1, "getnameinfo: %s", gai_strerror(herr));
  }

  fprintf(stderr, "%s on %s %s\n", msg, host, port);
}

void help(void) {
  fprintf(stderr, "OpenBSD netcat )\n");
  usage(0);
  fprintf(stderr, "\tCommand Summary:\n\
	\t-4		Use IPv4\n\
	\t-6		Use IPv6\n\
	\t-D		Enable the debug socket option\n\
	\t-d		Detach from stdin\n\
	\t-h		This help text\n\
	\t-i secs\t	Delay interval for lines sent, ports scanned\n\
	\t-k		Keep inbound sockets open for multiple connects\n\
	\t-l		Listen mode, for inbound connects\n\
	\t-n		Suppress name/port resolutions\n\
	\t-P proxyuser\tUsername for proxy authentication\n\
	\t-p port\t	Specify local port for remote connects\n\
	\t-q secs\t	quit after EOF on stdin and delay of secs\n\
	\t-r		Randomize remote ports\n "
#ifdef TCP_MD5SIG
                  "	\t-S		Enable the TCP MD5 signature option\n"
#endif
                  "	\t-s addr\t	Local source address\n\
	\t-T ToS\t	Set IP Type of Service\n\
	\t-C		Send CRLF as line-ending\n\
	\t-t		Answer TELNET negotiation\n\
	\t-U		Use UNIX domain socket\n\
	\t-u		UDP mode\n\
	\t-v		Verbose\n\
	\t-w secs\t	Timeout for connects and final net reads\n\
	\t-X proto	Proxy protocol: \"4\", \"5\" (SOCKS) or \"connect\"\n\
	\t-x addr[:port]\tSpecify proxy address and port\n\
	\t-z		Zero-I/O mode [used for scanning]\n\
	Port numbers can be individual or ranges: lo-hi [inclusive]\n");
  exit(0);
}

void usage(int ret) {
  fprintf(stderr, "This is nc from the netcat-openbsd package. An alternative "
                  "nc is available\n");
  fprintf(stderr, "in the netcat-traditional package.\n");
  fprintf(stderr, "usage: nc [-46DdhklnrStUuvzC] [-i interval] [-P "
                  "proxy_username] [-p source_port]\n");
  fprintf(
      stderr,
      "\t  [-s source_ip_address] [-T ToS] [-w timeout] [-X proxy_protocol]\n");
  fprintf(stderr, "\t  [-x proxy_address[:port]] [hostname] [port[s]]\n");
  if (ret)
    exit(1);
}

/*
 * quit()
 * handler for a "-q" timeout (exit 0 instead of 1)
 */
static void quit() {
  /* XXX: should explicitly close fds here */
  exit(0);
}
