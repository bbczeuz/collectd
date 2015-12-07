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
static int   g_conf_timeout = 2;
static int   g_conf_retries = 3; //First try doesn't count

static const char *g_config_keys[] =
{
	"Host",
	"Port",
	"Timeout",
	"Retries",
//	"ReportSeconds"
};
static int g_config_keys_num = STATIC_ARRAY_SIZE (g_config_keys);


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

//Code based on http://long.ccaba.upc.edu/long/045Guidelines/eva/ipv6.html
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
	assert(p_host);assert(p_port);
	//Try to connect to the weather station (and retry if needed)
	int now_try =  0;
	int failed  =  0;
	int sock    = -1;
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
		if (++now_try > g_conf_retries)
		{
			failed = 1;
			break;
		}
	}
	if (failed != 0)
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
}

static int wxt_config(const char *p_key, const char *p_value)
{
	//Parse configuration p_key
	//Returns 0 if OK, 1 if parameter error, -1 if key error
	if (strcasecmp (p_key, "host") == 0)
	{
		if (g_conf_host != NULL)
		{
			free (g_conf_host);
			g_conf_host = NULL;
		}
		if ((g_conf_host = strdup (p_value)) == NULL)
		{
			return (1);
		}
	} else if (strcasecmp (p_key, "port") == 0)
	{
		if (g_conf_port != NULL)
		{
			free (g_conf_port);
			g_conf_port = NULL;
		}
		if ((g_conf_port = strdup (p_value)) == NULL)
		{
			return (1);
		}
	} else if (strcasecmp (p_key, "timeout") == 0)
	{
		g_conf_timeout = atoi(p_value);
		if (g_conf_timeout <= 0)
		{
			return (1);
		}
	} else if (strcasecmp (p_key, "retries") == 0)
	{
		g_conf_retries = atoi(p_value);
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
