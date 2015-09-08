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
#define DEFAULT_VARS_SIZE 128

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

/* Close the network connection and free variables */
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


//Code based on from http://long.ccaba.upc.edu/long/045Guidelines/eva/ipv6.html
static int connect_client (const char *hostname,
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


static int wxt_split_varval(char *p_line, size_t p_line_size, const char p_var_term, const char p_val_term, char **p_vars, char **p_vals, size_t *p_vars_size )
{
	assert(p_line);
	assert(p_vars);
	assert(p_vals);
	assert(p_vars_size);
	assert(p_line_size);

	p_vars[0] = p_line;
	char **now_var = p_vars;
	char **now_val = p_vals;
	size_t vars_size = 0;
	while (vars_size < *p_vars_size)
	{
		//Extract variable name
		char *now_var_end = memchr(*now_var,p_var_term,p_line_size);
		if (now_var_end == NULL)
		{
			//No variable terminator found
			break;
		}
		if (now_var_end == *now_var + p_line_size)
		{
			//No space for values
			break;
		}
		*now_var_end = 0;
		p_line_size -= now_var_end - *now_var +1;
//		DEBUG("wxt plugin: Found variable \"%s\" (remaining size = %lu)", *now_var,p_line_size);
		*now_val      = now_var_end+1;
		++now_var;

		//Extract variable value
		char *now_val_end = memchr(*now_val,p_val_term,p_line_size);
		if (now_val_end == NULL)
		{
			now_val_end = memchr(*now_val,'\r',p_line_size); //End of line
			if (now_val_end == NULL)
			{
				//No value terminator found -> end of line
				if (p_line_size == 0)
				{
					//No space left
					break;
				} else {
					now_val_end = *now_val + p_line_size;
				}
			}
		}
		*now_val_end = 0;
//		DEBUG("wxt plugin: now_var_end = %s, *now_val = %s", now_var_end,*now_val);
		p_line_size -= now_var_end - *now_val +1;
//		DEBUG("wxt plugin: Found value \"%s\" (remaining size = %lu)", *now_val,p_line_size);
		*now_var = now_val_end+1;
		++now_val;
		++vars_size;
	}
	*p_vars_size = vars_size;
	return 0;
}

static int wxt_process_command(int p_socket, const char *p_request, const size_t p_resp_buf_size, const size_t p_line_count, char **p_line_starts, size_t *p_line_size)
{
	DEBUG("wxt plugin: Sending request");
	size_t request_size = strlen(p_request);
	int rc = send(p_socket,p_request,request_size,0);
	if (rc != request_size)
	{
		//Close connection
		DEBUG("wxt plugin: Error sending request. errno=%d, msg=\"%s\"", errno, strerror(errno));
		return -2;
	}

	//Receive response
	size_t resp_buf_used = 0;
	char *resp_buf_head = p_line_starts[0];
	size_t *now_line_used = &p_line_size[0];
	char **now_line_start = &p_line_starts[0];

	DEBUG("wxt plugin: Receiving response");
	while (1)
	{
		if (p_resp_buf_size<=resp_buf_used)
		{
			ERROR("wxt plugin: Buffer too small to fit response (size: %lu, needed: %lu)", p_resp_buf_size, resp_buf_used);
			return -3;

		}
		rc = recv(p_socket,resp_buf_head,p_resp_buf_size-resp_buf_used,0);
		if (rc == -1)
		{
			ERROR("wxt plugin: Error receiving data. errno = %d, msg = \"%s\"", errno,strerror(errno));
			return -4;
		} else if (rc == 0)
		{
			//Remote connection shutdown
			ERROR("wxt plugin: Remote connection shutdown");
			break;
		}
		resp_buf_head += rc;
		*resp_buf_head = 0;
		resp_buf_used  = resp_buf_head - p_line_starts[0];

		//Parse buffer
		*now_line_used = resp_buf_head - *now_line_start;
		DEBUG("wxt plugin: *now_line_start = %s", *now_line_start);
		DEBUG("wxt plugin: *now_line_used  = %lu", *now_line_used);

		//Loop as long as \r found
		char *now_line_end;
		while ((now_line_end = memchr(*now_line_start,'\r', *now_line_used)) != NULL)
		{
			if ((now_line_end + 1 >= resp_buf_head) || (now_line_end[1] != '\n'))
			{
				//No \r\n found; more data needed
				DEBUG("wxt plugin: No more line terminators found; waiting for more data");
				break;
			} else {
				//Received complete line
				*now_line_end = 0;
				DEBUG("wxt plugin: Line completed: %s", *now_line_start);
				*now_line_used = now_line_end - *now_line_start;

				//Check if last line
				if ((now_line_start-p_line_starts) >= p_line_count)
				{
					break;
				} else {
					//Move to next line
					++now_line_used;
					++now_line_start;
					*now_line_start = now_line_end + 2; //Move past \r\n
					*now_line_used  = resp_buf_head - *now_line_start;
					DEBUG("wxt plugin: resp_buf = %s, resp_buf_head-resp_buf = %lu, resp_buf_used = %lu, *now_line_used = %lu, **now_line_start = %s", p_line_starts[0], resp_buf_head-p_line_starts[0],resp_buf_used,*now_line_used,*now_line_start);
				}
			}
		}
		DEBUG("wxt plugin: Looping for more data");

		//Check if all lines are received
		if ((now_line_start-p_line_starts) >= p_line_count)
		{
			//received all lines
			break;
		}
		//Loop for more data
	}
	return 0;
}

static int wxt_process_split_command(int p_socket,const char *p_command, const size_t p_resp_n_lines, const char *p_line_header, const char p_var_term, const char p_val_term, char ***p_vars,char ***p_vals, size_t *p_vars_size)
{
	int i;
	const size_t resp_buf_size = 1024;
	char *resp_buf = malloc(resp_buf_size);
	memset(resp_buf,0,resp_buf_size);

	char **line_starts = malloc(sizeof(char*  )*p_resp_n_lines);
	size_t *line_size  = malloc(sizeof(size_t*)*p_resp_n_lines);

	line_starts[0] = resp_buf;
	memset(line_size,0,sizeof(size_t*)*p_resp_n_lines);

	if (wxt_process_command(p_socket,p_command,resp_buf_size, p_resp_n_lines, line_starts, line_size) != 0)
	{
		ERROR("wxt plugin:  wxt_process_command(p_socket,p_command,resp_buf_size, p_resp_n_lines, line_starts, line_size) failed");
		free(line_size);
		free(line_starts);
		free(resp_buf);
		return 1;
	}

/* typical data:
 * [2015-09-08 13:37:59] wxt plugin: Line[0] = "$WIXDR,C,26.7,C,0,H,30.2,P,0,P,971.5,H,0*46"
 * [2015-09-08 13:37:59] wxt plugin: Line[1] = "$WIXDR,V,0.09,M,0,Z,170,s,0,R,0.0,M,0,V,0.0,M,1,Z,0,s,1,R,0.0,M,1*6E"
 * [2015-09-08 13:37:59] wxt plugin: Line[2] = "$WIXDR,C,27.9,C,2,U,24.1,N,0,U,24.5,V,1,U,3.521,V,2*77"
 */
	
	DEBUG("wxt plugin: Received all data");
	for (i=0; i<p_resp_n_lines; ++i)
	{
		DEBUG("wxt plugin: Line[%d] = \"%s\" (size=%lu)", i,line_starts[i],line_size[i]);
	}
	
	//Skip header
	if ((line_size[0]>strlen(p_line_header)) && (memcmp(line_starts[0], p_line_header,strlen(p_line_header)) == 0))
	{
		line_starts[0] += strlen(p_line_header);
		line_size[0]   -= strlen(p_line_header); 
	} else {
		WARNING("wxt plugin: Line[0]: Missing header");
	}

	//Split into variable - value pairs
	size_t vars_size;
	char **vars;
	char **vals;
	assert(p_vars_size);
	if (*p_vars_size == 0)
	{
		vars_size = DEFAULT_VARS_SIZE;
		vars = malloc(sizeof(char*)*vars_size);		
		vals = malloc(sizeof(char*)*vars_size);		
		*p_vars = vars;
		*p_vals = vals;
	} else {
		assert(p_vars);
		assert(p_vals);
		vars = *p_vars;
		vals = *p_vals;
		vars_size = *p_vars_size;
	}
	DEBUG("wxt plugin: Splitting into up to %lu var/val pairs", vars_size);
	wxt_split_varval(line_starts[0], line_size[0], p_var_term, p_val_term, vars, vals, &vars_size );

	int now_var;
	for (now_var = 0; now_var < vars_size; ++now_var)
	{
		DEBUG("wxt plugin: var[%d]: \"%s\" = \"%s\"", now_var, vars[now_var], vals[now_var]);
	}
	*p_vars_size = vars_size;
	
	free(line_starts);
	free(line_size);
	return 0;
}

static char *wxt_pair_get(char **p_vars, char **p_vals, const size_t p_vars_size, const char *p_searched_var)
{
	size_t now_var;
	assert(p_vars);
	assert(p_vals);
	for (now_var = 0; now_var < p_vars_size; ++now_var)
	{
		if (p_vars[now_var] == NULL)
		{
			continue;
		}
		if (strcmp(p_vars[now_var], p_searched_var) == 0)
		{
			return p_vals[now_var];
		}
	}
	return NULL;
}

/* Get and print status from weather station */
static int wxt_query(const char *p_host, const char *p_port, struct wxt_detail_s *p_wxt_detail)
{
#if 1
	assert(p_host);assert(p_port);

	//Retry
	int now_try = 0;
	int failed = 1;
	int sock = -1;
	DEBUG("wxt plugin: Connecting to %s:%s", p_host, p_port);
	while (1)
	{
		//Resolve host, Open socket, Set timeout and connect to Serial2Tcp server
		sock = connect_client(p_host, p_port, AF_UNSPEC, SOCK_STREAM,g_conf_timeout);
		
		if (sock != -1)
		{
			failed = 0;
			break;
		}
		if (++now_try>g_conf_retries)
		{
			break;
		}

		DEBUG("wxt plugin: Retrying...");
	}
	if (failed)
	{
		DEBUG("wxt plugin: Connection failed. errno=%d, msg=\"%s\"", errno, strerror(errno));
		return -1;
	} 

	failed = 1;
	size_t vars_size = DEFAULT_VARS_SIZE;
	char **vars=malloc(sizeof(char*)*DEFAULT_VARS_SIZE);
	char **vals=malloc(sizeof(char*)*DEFAULT_VARS_SIZE);
	do
	{
		//Request data
		#define WXT_COMMAND_COM_SETTINGS "0XU,M\r\n"
		#define WXT_COMMAND_COM_SET_ASCII "0XU,M=P\r\n"
		#define WXT_COMMAND_GET_WIND_DATA "0R1\r\n"
		#define WXT_COMMAND_GET_THP_DATA  "0R2\r\n"
		#define WXT_COMMAND_GET_RAIN_DATA "0R3\r\n"
		#define WXT_RESP_COM_SET_ASCII_N_LINES 1
		#define WXT_RESP_COM_SETTINGS_N_LINES 1
		#define WXT_RESP_WIND_DATA_N_LINES 1
		#define WXT_RESP_THP_DATA_N_LINES 1
		#define WXT_RESP_RAIN_DATA_N_LINES 1
		#define WXT_RESP_0R_N_LINES 3
		if (wxt_process_split_command(sock,WXT_COMMAND_COM_SETTINGS,WXT_RESP_COM_SETTINGS_N_LINES,"0XU,",'=',',',&vars,&vals,&vars_size) != 0)
		{
			break;
		}

		const char *wxt_protocol = wxt_pair_get(vars,vals,vars_size,"M");
		if (wxt_protocol == NULL)
		{
			ERROR("wxt plugin: Didn't receive protocol info");
			break;
		}
		if (strcmp(wxt_protocol,"P") != 0)
		{
			//FIXME: Automatically change protocol to "NMEA 0183 v3.0 query"
			INFO("wxt plugin: Unexpected WXT protocol (M=%s). Changing to ASCII, polled (M=P)",wxt_protocol);
			if (wxt_process_split_command(sock,WXT_COMMAND_COM_SET_ASCII,WXT_RESP_COM_SET_ASCII_N_LINES,"0XU,",'=',',',&vars,&vals,&vars_size) != 0)
			{
				break;
			}
		}

		vars_size = DEFAULT_VARS_SIZE;
		if (wxt_process_split_command(sock,WXT_COMMAND_GET_WIND_DATA,WXT_RESP_WIND_DATA_N_LINES,"0R1,",'=',',',&vars,&vals,&vars_size) != 0)
		{
			break;
		}

		vars_size = DEFAULT_VARS_SIZE;
		if (wxt_process_split_command(sock,WXT_COMMAND_GET_THP_DATA,WXT_RESP_THP_DATA_N_LINES,"0R2,",'=',',',&vars,&vals,&vars_size) != 0)
		{
			break;
		}

		vars_size = DEFAULT_VARS_SIZE;
		if (wxt_process_split_command(sock,WXT_COMMAND_GET_RAIN_DATA,WXT_RESP_RAIN_DATA_N_LINES,"0R3,",'=',',',&vars,&vals,&vars_size) != 0)
		{
			break;
		}

		failed = 0;
	} while (0);

	if (vars != NULL)
	{
		free(vars);
	}
	if (vals != NULL)
	{
		free(vals);
	}
	if (failed)
	{
	}
#if 0
	//Perform next request
	memset(resp_buf,0,sizeof(resp_buf));
	line_starts[0] = resp_buf;
	line_size[0]   = 0;
	wxt_process_command(sock,"0XU\r\n",resp_buf_size, WXT_RESP_0XU_N_LINES, line_starts, line_size);

/* typical data:
 * 0XU,A=0,M=Q,T=0,C=2,I=0,B=19200,D=8,P=N,S=1,L=20,N=WXT520,V=2.14\r\n
 */
	assert(line_size[0] != 0);
	DEBUG("wxt plugin: Received all data");
	DEBUG("wxt plugin: Line[0] = \"%s\" (size=%lu)", line_starts[0],line_size[0]);

	//Skip header
	#define WXT_RESP_0XU_HEADER0 "0XU,"
	if ((line_size[0]>strlen(WXT_RESP_0XU_HEADER0)) && (memcmp(line_starts[0], WXT_RESP_0XU_HEADER0,strlen(WXT_RESP_0XU_HEADER0)) == 0))
	{
		line_starts[0] += strlen(WXT_RESP_0XU_HEADER0);
	} else {
		WARNING("wxt plugin: Line[0]: Missing header");
	}

	vars_size = VARS_SIZE;
	wxt_split_varval(line_starts[0], line_size[0], '=', ',', vars, vals, &vars_size );
	for (now_var = 0; now_var < vars_size; ++now_var)
	{
		DEBUG("wxt plugin: var[%d]: \"%s\" = \"%s\"", now_var, vars[now_var], vals[now_var]);
	}
	//Close connection
	close(sock);

	return (0);
#endif
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

/*
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
*/
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

//	value_submit (&wxt_detail);

	return (0);
} /* wxt_read */

void module_register (void)
{
	plugin_register_config   ("wxt", wxt_config, g_config_keys, g_config_keys_num);
	plugin_register_read     ("wxt", wxt_read);
	plugin_register_shutdown ("wxt", wxt_shutdown);
} /* void module_register */
