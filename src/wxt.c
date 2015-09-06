/*
 * collectd - src/wxt.c
 * Copyright (C) 2015       Claudius Zingerli
 *
 * Code framework based on apcups plugin of (collectd: src/apcups.c)
 * Tested with Vaisala Weather Transmitter WXT520 connected via a transparent serial to TCP converter
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of version 2 of the GNU General
 * Public License as published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU
 * General Public License for more details.
 *
 * You should have received a copy of the GNU General Public
 * License along with this program; if not, write to the Free
 * Software Foundation, Inc., 59 Temple Place - Suite 330, Boston,
 * MA 02111-1307, USA.
 *
 * Authors:
 *   Claudius Zingerli <bugs-wxtmail@zeuz.ch> and the developers of apcups.c
 **/

/* Request: 0R\r\n
 * Response:
 * $WIXDR,C,24.1,C,0,H,43.7,P,0,P,969.9,H,0*46\r\n
 * $WIXDR,V,0.00,M,0,Z,10,s,0,R,0.0,M,0,V,0.0,M,1,Z,0,s,1,R,0.0,M,1*50\r\n
 * $WIXDR,C,23.5,C,2,U,24.1,N,0,U,24.5,V,1,U,3.520,V,2*7E\r\n
 * Request: 0XU\r\n
 * Response:
 * 0XU,A=0,M=Q,T=0,C=2,I=0,B=19200,D=8,P=N,S=1,L=20,N=WXT520,V=2.14\r\n
 */
#include "collectd.h"
#include "common.h"      /* rrd_update_file */
#include "plugin.h"      /* plugin_register, plugin_submit */
#include "configfile.h"  /* cf_register */

#if HAVE_SYS_TYPES_H
# include <sys/types.h>
#endif
#if HAVE_SYS_SOCKET_H
# include <sys/socket.h>
#endif
#if HAVE_NETDB_H
# include <netdb.h>
#endif

#if HAVE_NETINET_IN_H
# include <netinet/in.h>
#endif

#define MAXSTRING   1024
#define MODULE_NAME "wxt"

#define S2T_DEFAULT_HOST "localhost"
#define S2T_DEFAULT_PORT "4001"

/*
 * Private data types
 */
struct wxt_detail_s
{
	double temp_air;
	double temp_heating;
	double humi_air;
	double pres_air;
	double rain_mm;
	double rain_sec;
	double rain_mmh;
	double hail_mm;
	double hail_sec;
	double hail_hith;
	double rain_peak_mmh;
	double hail_peak_hith;
	double wind_dir_min;
	double wind_dir_avg;
	double wind_dir_max;
	double wind_speed_min;
	double wind_speed_avg;
	double wind_speed_max;
	double volt_supply;
	double volt_heating;
	double volt_reference;
};

/*
 * Private variables
 */
/* Default values for contacting daemon */
static char *g_conf_host = NULL;
static char *g_conf_port = NULL;
static int g_conf_timeout = 2;
static int g_conf_retries = 3; //First try doesn't count

//static int global_sockfd = -1;

//static int count_retries = 0;
//static int count_iterations = 0;
//static _Bool close_socket = 0;

static const char *g_config_keys[] =
{
	"Host",
	"Port",
	"Timeout",
	"Retries",
//	"ReportSeconds"
};
static int g_config_keys_num = STATIC_ARRAY_SIZE (g_config_keys);

#if 0
static int net_shutdown (int *fd)
{
	uint16_t packet_size = 0;

	if ((fd == NULL) || (*fd < 0))
		return (EINVAL);

	swrite (*fd, (void *) &packet_size, sizeof (packet_size));
	close (*fd);
	*fd = -1;

	return (0);
} /* int net_shutdown */
#endif

/* Close the network connection */
static int wxt_shutdown (void)
{
	if (g_conf_host != NULL)
	{
		free (g_conf_host);
		g_conf_host = NULL;
	}
	if (g_conf_port != NULL)
	{
		free (g_conf_port);
		g_conf_port = NULL;
	}
	
	return (0);
} /* int wxt_shutdown */

/*
 * Open a TCP connection to the UPS network server
 * Returns -1 on error
 * Returns socket file descriptor otherwise
 */
#if 0
static int net_open (char *host, int port)
{
	int              sd;
	int              status;
	char             port_str[8];
	struct addrinfo  ai_hints;
	struct addrinfo *ai_return;
	struct addrinfo *ai_list;

	assert ((port > 0x00000000) && (port <= 0x0000FFFF));

	/* Convert the port to a string */
	ssnprintf (port_str, sizeof (port_str), "%i", port);

	/* Resolve name */
	memset ((void *) &ai_hints, '\0', sizeof (ai_hints));
	ai_hints.ai_family   = AF_INET; /* XXX: Change this to `AF_UNSPEC' if wxtd can handle IPv6 */
	ai_hints.ai_socktype = SOCK_STREAM;

	status = getaddrinfo (host, port_str, &ai_hints, &ai_return);
	if (status != 0)
	{
		char errbuf[1024];
		INFO ("getaddrinfo failed: %s",
				(status == EAI_SYSTEM)
				? sstrerror (errno, errbuf, sizeof (errbuf))
				: gai_strerror (status));
		return (-1);
	}

	/* Create socket */
	sd = -1;
	for (ai_list = ai_return; ai_list != NULL; ai_list = ai_list->ai_next)
	{
		sd = socket (ai_list->ai_family, ai_list->ai_socktype, ai_list->ai_protocol);
		if (sd >= 0)
			break;
	}
	/* `ai_list' still holds the current description of the socket.. */

	if (sd < 0)
	{
		DEBUG ("Unable to open a socket");
		freeaddrinfo (ai_return);
		return (-1);
	}

	status = connect (sd, ai_list->ai_addr, ai_list->ai_addrlen);

	freeaddrinfo (ai_return);

	if (status != 0) /* `connect(2)' failed */
	{
		char errbuf[1024];
		INFO ("connect failed: %s",
				sstrerror (errno, errbuf, sizeof (errbuf)));
		close (sd);
		return (-1);
	}

	DEBUG ("Done opening a socket %i", sd);

	return (sd);
} /* int net_open (char *host, char *service, int port) */
#endif

/*
 * Receive a message from the other end. Each message consists of
 * two packets. The first is a header that contains the size
 * of the data that follows in the second packet.
 * Returns number of bytes read
 * Returns 0 on end of file
 * Returns -1 on hard end of file (i.e. network connection close)
 * Returns -2 on error
 */
#if 0
static int net_recv (int *sockfd, char *buf, int buflen)
{
	uint16_t packet_size;

	/* get data size -- in short */
	if (sread (*sockfd, (void *) &packet_size, sizeof (packet_size)) != 0)
	{
		close (*sockfd);
		*sockfd = -1;
		return (-1);
	}

	packet_size = ntohs (packet_size);
	if (packet_size > buflen)
	{
		ERROR ("wxt plugin: Received %"PRIu16" bytes of payload "
				"but have only %i bytes of buffer available.",
				packet_size, buflen);
		close (*sockfd);
		*sockfd = -1;
		return (-2);
	}

	if (packet_size == 0)
		return (0);

	/* now read the actual data */
	if (sread (*sockfd, (void *) buf, packet_size) != 0)
	{
		close (*sockfd);
		*sockfd = -1;
		return (-1);
	}

	return ((int) packet_size);
} /* static int net_recv (int *sockfd, char *buf, int buflen) */
#endif

/*
 * Send a message over the network. The send consists of
 * two network packets. The first is sends a short containing
 * the length of the data packet which follows.
 * Returns zero on success
 * Returns non-zero on error
 */
#if 0
static int net_send (int *p_sock, const char *p_buf, size_t p_buf_size)
{
	uint16_t packet_size;

	assert (*p_sock >= 0);

	/* send short containing size of data packet */
	packet_size = htons ((uint16_t) p_buf_size);

	if (swrite (*p_sock, (void *) &packet_size, sizeof (packet_size)) != 0)
	{
		close (*p_sock);
		*p_sock = -1;
		return (-1);
	}

	/* send data packet */
	if (swrite (*p_sock, (void *) p_buf, p_buf_size) != 0)
	{
		close (*p_sock);
		*p_sock = -1;
		return (-2);
	}

	return (0);
}
#endif

//Code based on from http://long.ccaba.upc.edu/long/045Guidelines/eva/ipv6.html
int connect_client (const char *hostname,
                const char *service,
                int         family,
                int         socktype,
		double     p_timeout)
{
	struct addrinfo hints, *res, *ressave;
	int n, sockfd;
	assert(p_timeout>0);

	memset(&hints, 0, sizeof(struct addrinfo));

	hints.ai_family = family;
	hints.ai_socktype = socktype;

	n = getaddrinfo(hostname, service, &hints, &res);

	if (n <0)
	{
		fprintf(stderr, "getaddrinfo error:: [%s]\n", gai_strerror(n));
		return -1;
	}

	ressave = res;

	sockfd=-1;
	while (res)
	{
		sockfd = socket(res->ai_family, res->ai_socktype, res->ai_protocol);

		if (!(sockfd < 0))
		{
			struct timeval timeout;
			timeout.tv_sec  = p_timeout;
			timeout.tv_usec = (p_timeout-timeout.tv_sec)*1000000;

			if (setsockopt (sockfd, SOL_SOCKET, SO_RCVTIMEO, (char *)&timeout, sizeof(timeout)) < 0)
			{
				fprintf(stderr, "setsockopt failed\n");
			}

			if (connect(sockfd, res->ai_addr, res->ai_addrlen) == 0)
			{
				break;
			}

			close(sockfd);
			sockfd=-1;
		}
		res=res->ai_next;
	}

	freeaddrinfo(ressave);
	return sockfd;
}


/* Get and print status from weather station */
static int wxt_query(const char *p_host, const char *p_port, struct wxt_detail_s *p_wxt_detail)
{
#if 1
	assert(p_host);assert(p_port);
	//Retry
	int now_try = 0;
	int failed = 0;
	int sock = -1;
	while (1)
	{
		//Resolve host, Open socket, Set timeout and connect to Serial2Tcp server
		sock = connect_client(p_host, p_port, AF_UNSPEC, SOCK_STREAM,g_conf_timeout);
		
		if (sock != -1)
		{
			//Request data
			
			//Close connection
			close(sock);
			break;
		}
		if (++now_try>g_conf_retries)
		{
			failed = 1;
			break;
		}
	}
	if (failed)
	{
		return -1;
	} 

	//Request data
	const char request[] = "0R\r\n";
	size_t request_size = strlen(request);
	int rc = send(sock,request,request_size,0);
	if (rc != request_size)
	{
		//Close connection
		close(sock);
		return -2;
	}

	//Receive response
	//We expect precisely three lines (\r\n) of data
	const size_t resp_buf_size = 1024;
	size_t resp_buf_used = 0;
	char resp_buf[resp_buf_size];
	char *resp_buf_head = resp_buf;
	char *line_starts[3] = { resp_buf,NULL,NULL };
	size_t line_size[3]  = { 0,0,0 };
	size_t **next_line_used = &line_size[0];
	char *next_line_start = &line_starts[0];

	while (1)
	{
		if (resp_buf_size<=resp_buf_used)
		{
			//TODO: Nicer error handling
			//Close connection
			close(sock);
			return -3;

		}
		rc = recv(sock,resp_buf_head,resp_buf_size-resp_buf_used,0);
		if (rc == -1)
		{
			//TODO: Nicer error handling
			//Close connection
			close(sock);
			return -4;
		} else if (rc == 0)
		{
			//Remote connection shutdown
			break;
		}
		resp_buf_head += rc;
		resp_buf_used  = resp_buf_head - resp_buf;
		*next_line_used = resp_buf_head - next_line_start;
		char *next_line_end = memchr(*next_line_start,'\r', *next_line_used);
		if (next_line_end == NULL)
		{
			//No \n found
			continue;
		}
		if ((next_line_end + 1 >= resp_buf_head) || (next_line_end[1] != '\n'))
		{
			//No \n\r found
			continue;
		}
		next_line_used = next_line_end - next_line_start;
		++next_line_used;
		++next_line_start;
		*next_line_start = next_line_end + 2;
		if (next_line_used == line_size+(sizeof(line_size)/sizeof(line_size[0])))
		{
			//received all lines
			break;
		}
	}
	

	//Close connection
	close(sock);

	return (0);
#else
	int     n;
	char    recvline[1024];
	char   *tokptr;
	char   *toksaveptr;
	char   *key;
	double  value;
	_Bool retry = 1;
	int status;


#if APCMAIN
# define PRINT_VALUE(name, val) printf("  Found property: name = %s; value = %f;\n", name, val)
#else
# define PRINT_VALUE(name, val) /**/
#endif

	while (retry)
	{
		if (global_sockfd < 0)
		{
			global_sockfd = net_open (host, port);
			if (global_sockfd < 0)
			{
				ERROR ("wxt plugin: Connecting to the " "wxtd failed.");
				return (-1);
			}
		}


		status = net_send (&global_sockfd, "status", strlen ("status"));
		if (status != 0)
		{
			/* net_send is closing the socket on error. */
			assert (global_sockfd < 0);
			if (retry)
			{
				retry = 0;
				count_retries++;
				continue;
			}

			ERROR ("wxt plugin: Writing to the socket failed.");
			return (-1);
		}

		break;
	} /* while (retry) */

        /* When collectd's collection interval is larger than wxtd's
         * timeout, we would have to retry / re-connect each iteration. Try to
         * detect this situation and shut down the socket gracefully in that
         * case. Otherwise, keep the socket open to avoid overhead. */
	count_iterations++;
	if ((count_iterations == 10) && (count_retries > 2))
	{
		NOTICE ("wxt plugin: There have been %i retries in the "
				"first %i iterations. Will close the socket "
				"in future iterations.",
				count_retries, count_iterations);
		close_socket = 1;
	}

	while ((n = net_recv (&global_sockfd, recvline, sizeof (recvline) - 1)) > 0)
	{
		assert ((unsigned int)n < sizeof (recvline));
		recvline[n] = '\0';
#if APCMAIN
		printf ("net_recv = `%s';\n", recvline);
#endif /* if APCMAIN */

		toksaveptr = NULL;
		tokptr = strtok_r (recvline, " :\t", &toksaveptr);
		while (tokptr != NULL)
		{
			key = tokptr;
			if ((tokptr = strtok_r (NULL, " :\t", &toksaveptr)) == NULL)
				continue;
			value = atof (tokptr);

			PRINT_VALUE (key, value);

			if (strcmp ("LINEV", key) == 0)
				p_wxt_detail->linev = value;
			else if (strcmp ("BATTV", key) == 0)
				p_wxt_detail->battv = value;
			else if (strcmp ("ITEMP", key) == 0)
				p_wxt_detail->itemp = value;
			else if (strcmp ("LOADPCT", key) == 0)
				p_wxt_detail->loadpct = value;
			else if (strcmp ("BCHARGE", key) == 0)
				p_wxt_detail->bcharge = value;
			else if (strcmp ("OUTPUTV", key) == 0)
				p_wxt_detail->outputv = value;
			else if (strcmp ("LINEFREQ", key) == 0)
				p_wxt_detail->linefreq = value;
			else if (strcmp ("TIMELEFT", key) == 0)
			{
				p_wxt_detail->timeleft = value;
			}

			tokptr = strtok_r (NULL, ":", &toksaveptr);
		} /* while (tokptr != NULL) */
	}
	status = errno; /* save errno, net_shutdown() may re-set it. */

	if (close_socket)
		net_shutdown (&global_sockfd);

	if (n < 0)
	{
		char errbuf[1024];
		ERROR ("wxt plugin: Reading from socket failed: %s",
				sstrerror (status, errbuf, sizeof (errbuf)));
		return (-1);
	}
#endif
	return (0);
}

static int wxt_config (const char *key, const char *value)
{
	if (strcasecmp (key, "host") == 0)
	{
		if (g_conf_host != NULL)
		{
			free (g_conf_host);
			g_conf_host = NULL;
		}
		if ((g_conf_host = strdup (value)) == NULL)
		{
			return (1);
		}
	} else if (strcasecmp (key, "port") == 0)
	{
		if (g_conf_port != NULL)
		{
			free (g_conf_port);
			g_conf_port = NULL;
		}
		if ((g_conf_port = strdup (value)) == NULL)
		{
			return (1);
		}
	} else if (strcasecmp (key, "timeout") == 0)
	{
		g_conf_timeout = atoi(value);
		if (g_conf_timeout <= 0)
		{
			return (1);
		}
	} else if (strcasecmp (key, "retries") == 0)
	{
		g_conf_retries = atoi(value);
		if (g_conf_retries <= 1)
		{
			return (1);
		}
	} else {
		return (-1);
	}
	return (0);
}

static void value_submit_generic (char *p_value_type, char *p_value_type_instance, double p_value)
{
	assert(p_value_type);
	assert(p_value_type_instance);
	value_t values[1];
	value_list_t vl = VALUE_LIST_INIT;

	values[0].gauge = p_value;

	vl.values     = values;
	vl.values_len = 1;

	sstrncpy (vl.host,            hostname_g, sizeof (vl.host));
	sstrncpy (vl.plugin,          "wxt",      sizeof (vl.plugin));
	sstrncpy (vl.plugin_instance, "",         sizeof (vl.plugin_instance));
	sstrncpy (vl.type,            p_value_type,             sizeof (vl.type));
	sstrncpy (vl.type_instance,   p_value_type_instance,  sizeof (vl.type_instance));

	plugin_dispatch_values (&vl);
}

static void value_submit (struct wxt_detail_s *p_wxt_detail)
{
	value_submit_generic ("temperature", "temp_air",      p_wxt_detail->temp_air);
	value_submit_generic ("temperature", "temp_heating",  p_wxt_detail->temp_heating);
	value_submit_generic ("humidity",    "humi_air",      p_wxt_detail->humi_air);
	value_submit_generic ("pressure",    "pres_air",      p_wxt_detail->pres_air);
	value_submit_generic ("counter",     "rain_mm",  p_wxt_detail->rain_mm);
	value_submit_generic ("counter",     "rain_sec", p_wxt_detail->rain_sec);
	value_submit_generic ("gauge",       "rain_mm_per_h",  p_wxt_detail->rain_mmh);
	value_submit_generic ("counter",     "hail_hit",       p_wxt_detail->hail_mm);
	value_submit_generic ("counter",     "hail_sec",       p_wxt_detail->hail_sec);
	value_submit_generic ("gauge",       "hail_hit_per_h", p_wxt_detail->hail_hith);
	value_submit_generic ("gauge",       "rain_peak_mm_per_h",  p_wxt_detail->rain_peak_mmh);
	value_submit_generic ("gauge",       "hail_peak_hit_per_h", p_wxt_detail->hail_peak_hith);
	value_submit_generic ("angle",       "wind_dir_min",   p_wxt_detail->wind_dir_min);
	value_submit_generic ("angle",       "wind_dir_avg",   p_wxt_detail->wind_dir_avg);
	value_submit_generic ("angle",       "wind_dir_max",   p_wxt_detail->wind_dir_max);
	value_submit_generic ("speed",       "wind_speed_min", p_wxt_detail->wind_speed_min);
	value_submit_generic ("speed",       "wind_speed_avg", p_wxt_detail->wind_speed_avg);
	value_submit_generic ("speed",       "wind_speed_max", p_wxt_detail->wind_speed_max);
	value_submit_generic ("voltage",     "volt_supply",    p_wxt_detail->volt_supply);
	value_submit_generic ("voltage",     "volt_heating",   p_wxt_detail->volt_heating);
	value_submit_generic ("voltage",     "volt_reference", p_wxt_detail->volt_reference);
}

static int wxt_read (void)
{
	struct wxt_detail_s wxt_detail;
	int status;

	wxt_detail.temp_air = -1.0;
	wxt_detail.temp_air = -1.0;
	wxt_detail.temp_heating = -1.0;
	wxt_detail.humi_air = -1.0;
	wxt_detail.pres_air = -1.0;
	wxt_detail.rain_mm = -1.0;
	wxt_detail.rain_sec = -1.0;
	wxt_detail.rain_mmh = -1.0;
	wxt_detail.hail_mm = -1.0;
	wxt_detail.hail_sec = -1.0;
	wxt_detail.hail_hith = -1.0;
	wxt_detail.rain_peak_mmh = -1.0;
	wxt_detail.hail_peak_hith = -1.0;
	wxt_detail.wind_dir_min = -1.0;
	wxt_detail.wind_dir_avg = -1.0;
	wxt_detail.wind_dir_max = -1.0;
	wxt_detail.wind_speed_min = -1.0;
	wxt_detail.wind_speed_avg = -1.0;
	wxt_detail.wind_speed_max = -1.0;
	wxt_detail.volt_supply = -1.0;
	wxt_detail.volt_heating = -1.0;
	wxt_detail.volt_reference = -1.0;

	status = wxt_query(g_conf_host == NULL ? S2T_DEFAULT_HOST : g_conf_host,
			   g_conf_port == NULL ? S2T_DEFAULT_PORT : g_conf_port,
			   &wxt_detail);

	/*
	 * if we did not connect then do not bother submitting
	 * zeros. We want rrd files to have NAN.
	 */
	if (status != 0)
	{
		DEBUG ("wxt_query(%s, %s) = %i (errno = %i)",
				g_conf_host == NULL ? S2T_DEFAULT_HOST : g_conf_host,
				g_conf_port == NULL ? S2T_DEFAULT_PORT : g_conf_port,
				status,errno);
		return (-1);
	}

	value_submit (&wxt_detail);

	return (0);
} /* wxt_read */

void module_register (void)
{
	plugin_register_config   ("wxt", wxt_config, g_config_keys, g_config_keys_num);
	plugin_register_read     ("wxt", wxt_read);
	plugin_register_shutdown ("wxt", wxt_shutdown);
} /* void module_register */
