/*
 * lanserv_emu.c
 *
 * MontaVista IPMI code for creating a LAN interface to an emulated
 * SMI interface.
 *
 * Author: MontaVista Software, Inc.
 *         Corey Minyard <minyard@mvista.com>
 *         source@mvista.com
 *
 * Copyright 2003 MontaVista Software Inc.
 *
 *  This program is free software; you can redistribute it and/or
 *  modify it under the terms of the GNU Lesser General Public License
 *  as published by the Free Software Foundation; either version 2 of
 *  the License, or (at your option) any later version.
 *
 *
 *  THIS SOFTWARE IS PROVIDED ``AS IS'' AND ANY EXPRESS OR IMPLIED
 *  WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES OF
 *  MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE DISCLAIMED.
 *  IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR ANY DIRECT, INDIRECT,
 *  INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING,
 *  BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS
 *  OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND
 *  ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR
 *  TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE
 *  USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 *
 *  You should have received a copy of the GNU Lesser General Public
 *  License along with this program; if not, write to the Free
 *  Software Foundation, Inc., 675 Mass Ave, Cambridge, MA 02139, USA.  */

#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <sys/stat.h>
#include <sys/poll.h>
#include <time.h>
#include <sys/time.h>
#include <fcntl.h>
#include <stdio.h>
#include <unistd.h>
#include <string.h>
#include <netdb.h>
#include <stdlib.h>
#include <stdarg.h>
#include <popt.h> /* Option parsing made easy */
#include <malloc.h>
#include <sys/ioctl.h>
#include <termios.h>

#include <OpenIPMI/log.h>
#include <OpenIPMI/ipmi_err.h>
#include <OpenIPMI/ipmi_msgbits.h>
#include <OpenIPMI/lanserv.h>

#include "emu.h"

#define MAX_ADDR 4

typedef struct misc_data
{
    int lan_fd[MAX_ADDR];
    char *config_file;
} misc_data_t;

static void *
ialloc(lan_data_t *lan, int size)
{
    return malloc(size);
}

static void
ifree(lan_data_t *lan, void *data)
{
    return free(data);
}

typedef struct lan_addr_s
{
    struct sockaddr addr;
    socklen_t       addr_len;
    int             xmit_fd;
} lan_addr_t;

static void
lan_send(lan_data_t *lan,
	 struct iovec *data, int vecs,
	 void *addr, int addr_len)
{
    struct msghdr msg;
    lan_addr_t    *l = addr;
    int           rv;

    /* When we send messages to ourself, we set the address to NULL so
       it won't be used. */
    if (!l)
	return;

    msg.msg_name = &(l->addr);
    msg.msg_namelen = l->addr_len;
    msg.msg_iov = data;
    msg.msg_iovlen = vecs;
    msg.msg_control = NULL;
    msg.msg_controllen = 0;
    msg.msg_flags = 0;

    rv = sendmsg(l->xmit_fd, &msg, 0);
    if (rv) {
	/* FIXME - log an error. */
    }
}

static emu_data_t *emu;

static int
smi_send(lan_data_t *lan, msg_t *msg)
{
    unsigned char    data[36];
    unsigned int     data_len = sizeof(data);
    ipmi_msg_t       imsg;

    imsg.netfn = msg->netfn;
    imsg.cmd = msg->cmd;
    imsg.data = msg->data;
    imsg.data_len = msg->len;

    ipmi_emu_handle_msg(emu, msg->rs_lun, &imsg, data, &data_len);

    ipmi_handle_smi_rsp(lan, msg, data, data_len);
    return 0;
}

static int
gen_rand(lan_data_t *lan, void *data, int size)
{
    int fd = open("/dev/urandom", O_RDONLY);
    int rv;

    if (fd == -1)
	return errno;

    rv = read(fd, data, size);

    close(fd);
    return rv;
}

static void
handle_msg_lan(int lan_fd, lan_data_t *lan)
{
    int                len;
    lan_addr_t         l;
    unsigned char      data[256];

    l.addr_len = sizeof(l.addr);
    len = recvfrom(lan_fd, data, sizeof(data), 0, &(l.addr), &(l.addr_len));
    if (len < 0) {
	if (errno != EINTR) {
	    perror("Error receiving message");
	    exit(1);
	}
	return;
    }
    l.xmit_fd = lan_fd;

    if (len < 4)
	return;

    if (data[0] != 6)
	return; /* Invalid version */

    /* Check the message class. */
    switch (data[3]) {
	case 6:
	    handle_asf(lan, data, len, &l, sizeof(l));
	    break;

	case 7:
	    ipmi_handle_lan_msg(lan, data, len, &l, sizeof(l));
	    break;
    }
}

static int
open_lan_fd(struct sockaddr *addr, socklen_t addr_len)
{
    int                fd;
    int                rv;

    fd = socket(PF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (fd == -1) {
	perror("Unable to create socket");
	exit(1);
    }

    rv = bind(fd, addr, addr_len);
    if (rv == -1)
    {
	fprintf(stderr, "Unable to bind to LAN port: %s\n",
		strerror(errno));
	exit(1);
    }

    return fd;
}

static void
diff_timeval(struct timeval *dest,
	     struct timeval *left,
	     struct timeval *right)
{
    if (   (left->tv_sec < right->tv_sec)
	|| (   (left->tv_sec == right->tv_sec)
	    && (left->tv_usec < right->tv_usec)))
    {
	/* If left < right, just force to zero, don't allow negative
           numbers. */
	dest->tv_sec = 0;
	dest->tv_usec = 0;
	return;
    }

    dest->tv_sec = left->tv_sec - right->tv_sec;
    dest->tv_usec = left->tv_usec - right->tv_usec;
    while (dest->tv_usec < 0) {
	dest->tv_usec += 1000000;
	dest->tv_sec--;
    }
}

static void
log(int logtype, msg_t *msg, char *format, ...)
{
    va_list ap;
    struct timeval tod;
    struct tm ltime;
    char timebuf[30];
    int timelen;
    char fullformat[256];

    va_start(ap, format);
    gettimeofday(&tod, NULL);
    localtime_r(&tod.tv_sec, &ltime);
    asctime_r(&ltime, timebuf);
    timelen = strlen(timebuf);
    timebuf[timelen-1] = '\0'; /* Nuke the '\n'. */
    timelen--;
    if ((timelen + strlen(format) + 2) >= sizeof(fullformat)) {
	vprintf(format, ap);
    } else {
	strcpy(fullformat, timebuf);
	strcat(fullformat, ": ");
	strcat(fullformat, format);
	vprintf(fullformat, ap);
    }
    printf("\n");
    va_end(ap);
}

static char *config_file = "/etc/ipmi_lan.conf";
static char *command_string = NULL;
static int debug = 0;

static struct poptOption poptOpts[]=
{
    {
	"config-file",
	'c',
	POPT_ARG_STRING,
	&config_file,
	'c',
	"configuration file",
	""
    },
    {
	"command-string",
	'x',
	POPT_ARG_STRING,
	&command_string,
	'x',
	"command string",
	""
    },
    {
	"debug",
	'd',
	POPT_ARG_NONE,
	NULL,
	'd',
	"debug",
	""
    },
    POPT_AUTOHELP
    {
	NULL,
	0,
	0,
	NULL,
	0		 
    }	
};

static void
write_config(lan_data_t *lan)
{
//    misc_data_t *info = lan->user_info;
}

struct sockaddr addr[MAX_ADDR];
socklen_t addr_len[MAX_ADDR];
int num_addr = 0;

static char buffer[1024];
static int pos = 0;

static void
handle_user_char(char c)
{
    switch(c) {
    case 8:
    case 0x7f:
	if (pos > 0) {
	    pos--;
	    printf("\b \b");
	}
	break;

    case 4:
	if (pos == 0) {
	    printf("\n");
	    ipmi_emu_shutdown();
	}
	break;

    case 10:
    case 13:
	printf("\n");
	buffer[pos] = '\0';
	ipmi_emu_cmd(emu, buffer);
	printf("> ");
	pos = 0;
	break;

    default:
	if (pos >= sizeof(buffer)-1) {
	    printf("\nCommand is too long, max of %d characters\n",
		   sizeof(buffer)-1);
	} else {
	    buffer[pos] = c;
	    pos++;
	    printf("%c", c);
	}
    }
}

static void
handle_user_data_ready(void)
{
    char rc;
    int count;

    count = read(0, &rc, 1);
    while (count > 0) {
	handle_user_char(rc);
	count = read(0, &rc, 1);
    }
}

struct termios old_termios;
int old_flags;

static void
init_term(void)
{
    struct termios new_termios;

    tcgetattr(0, &old_termios);
    new_termios = old_termios;
    new_termios.c_iflag &= ~(IGNBRK|BRKINT|PARMRK|ISTRIP
			     |INLCR|IGNCR|ICRNL|IXON);
    new_termios.c_lflag &= ~(ECHO|ECHONL|ICANON|ISIG|IEXTEN);
    tcsetattr(0, TCSADRAIN, &new_termios);
    old_flags = fcntl(0, F_GETFL) & O_ACCMODE;
    fcntl(0, F_SETFL, old_flags | O_NONBLOCK);
}

void
ipmi_emu_shutdown(void)
{
    tcsetattr(0, TCSADRAIN, &old_termios);
    fcntl(0, F_SETFL, old_flags);
    tcdrain(0);
    exit(0);
}


int
main(int argc, const char *argv[])
{
    lan_data_t  lan;
    misc_data_t data;
    int max_fd;
    int rv;
    int o;
    int i;
    poptContext poptCtx;
    struct timeval timeout;
    struct timeval time_next;
    struct timeval time_now;

    poptCtx = poptGetContext(argv[0], argc, argv, poptOpts, 0);
    while ((o = poptGetNextOpt(poptCtx)) >= 0) {
	switch (o) {
	    case 'd':
		debug++;
		break;
	}
    }

    data.config_file = config_file;

    emu = ipmi_emu_alloc();

    memset(&lan, 0, sizeof(lan));
    lan.user_info = &data;
    lan.alloc = ialloc;
    lan.free = ifree;
    lan.lan_send = lan_send;
    lan.smi_send = smi_send;
    lan.gen_rand = gen_rand;
    lan.write_config = write_config;
    lan.log = log;
    lan.debug = debug;

    num_addr = MAX_ADDR;
    if (lanserv_read_config(&lan, data.config_file, (sockaddr_ip_t *) addr,
			    addr_len, &num_addr))
	exit(1);

    if (num_addr == 0) {
	struct sockaddr_in *ipaddr = (void *) &addr[0];
	ipaddr->sin_family = AF_INET;
	ipaddr->sin_port = htons(623);
	ipaddr->sin_addr.s_addr = INADDR_ANY;
	addr_len[0] = sizeof(*ipaddr);
	num_addr++;
    }

    for (i=0; i<num_addr; i++) {
	if (addr_len[i] == 0)
	    break;

	data.lan_fd[i] = open_lan_fd(&addr[i], addr_len[i]);
	if (data.lan_fd[i] == -1) {
	    fprintf(stderr, "Unable to open LAN address %d\n", i+1);
	    exit(1);
	}
    }

    rv = ipmi_lan_init(&lan);
    if (rv)
	return 1;

    init_term();
    printf("> ");

    max_fd = -1;
    for (i=0; i<num_addr; i++) {
	if (data.lan_fd[i] > max_fd)
	    max_fd = data.lan_fd[i];
    }
    max_fd++;

    if (command_string) {
	ipmi_emu_cmd(emu, command_string);
    }

    gettimeofday(&time_next, NULL);
    time_next.tv_sec += 10;
    for (;;) {
	fd_set readfds;

	fflush(stdout);

	FD_ZERO(&readfds);

	FD_SET(0, &readfds);
	for (i=0; i<num_addr; i++)
	    FD_SET(data.lan_fd[i], &readfds);

	gettimeofday(&time_now, NULL);
	diff_timeval(&timeout, &time_next, &time_now);
	rv = select(max_fd, &readfds, NULL, NULL, &timeout);
	if ((rv == -1) && (errno == EINTR))
	    continue;

	if (rv == 0) {
	    ipmi_lan_tick(&lan, 10);
	    time_next.tv_sec += 10;
	} else {
	    if (FD_ISSET(0, &readfds))
		handle_user_data_ready();

	    for (i=0; i<num_addr; i++) {
		if (FD_ISSET(data.lan_fd[i], &readfds))
		    handle_msg_lan(data.lan_fd[i], &lan);
	    }
	}
    }
}
