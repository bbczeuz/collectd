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
	double rain_sum_mm;
	double rain_duration;
	double rain_intensity;
	double rain_intensity_peak;
	double hail_sum_hits;
	double hail_duration;
	double hail_intensity;
	double hail_intensity_peak;
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
static char *g_conf_host = NULL; //Address of the WXT weather station
static char *g_conf_port = NULL; //Service/Port number
static char *g_conf_host_service = NULL; //concatenated host-port
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
	if (g_conf_host_service != NULL)
	{
		free (g_conf_host_service);
		g_conf_host_service = NULL;
	}
	
	return 0;
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
//		DEBUG(MODULE_NAME " plugin: Found variable \"%s\" (remaining size = %lu)", *now_var,p_line_size);
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
//		DEBUG(MODULE_NAME " plugin: now_var_end = %s, *now_val = %s", now_var_end,*now_val);
		p_line_size -= now_var_end - *now_val +1;
//		DEBUG(MODULE_NAME " plugin: Found value \"%s\" (remaining size = %lu)", *now_val,p_line_size);
		*now_var = now_val_end+1;
		++now_val;
		++vars_size;
	}
	*p_vars_size = vars_size;
	return 0;
}


static int wxt_process_command(int p_socket, const char *p_request, const size_t p_resp_buf_size, const size_t p_line_count, char **p_line_starts, size_t *p_line_size)
{
	DEBUG(MODULE_NAME " plugin: Sending request");
	size_t request_size = strlen(p_request);
	int rc = send(p_socket,p_request,request_size,0);
	if (rc != request_size)
	{
		//Close connection
		DEBUG(MODULE_NAME " plugin: Error sending request. errno=%d, msg=\"%s\"", errno, strerror(errno));
		return -2;
	}

	//Receive response
	size_t resp_buf_used   = 0;
	char  *resp_buf_head   = p_line_starts[0];
	size_t *now_line_used  = &p_line_size[0];
	char  **now_line_start = &p_line_starts[0];

	DEBUG(MODULE_NAME " plugin: Receiving response");
	while (1)
	{
		if (p_resp_buf_size <= resp_buf_used)
		{
			ERROR(MODULE_NAME " plugin: Buffer too small to fit response (size: %lu, needed: %lu)", p_resp_buf_size, resp_buf_used);
			return -3;

		}
		rc = recv(p_socket,resp_buf_head,p_resp_buf_size-resp_buf_used,0);
		if (rc == -1)
		{
			ERROR(MODULE_NAME " plugin: Error receiving data. errno = %d, msg = \"%s\"", errno,strerror(errno));
			return -4;
		} else if (rc == 0)
		{
			//Remote connection shutdown
			ERROR(MODULE_NAME " plugin: Remote connection shutdown");
			break;
		}
		resp_buf_head += rc;
		*resp_buf_head = 0;
		resp_buf_used  = resp_buf_head - p_line_starts[0];

		//Parse buffer
		*now_line_used = resp_buf_head - *now_line_start;
		DEBUG(MODULE_NAME " plugin: *now_line_start = %s", *now_line_start);
		DEBUG(MODULE_NAME " plugin: *now_line_used  = %lu", *now_line_used);

		//Loop as long as \r found
		char *now_line_end;
		while ((now_line_end = memchr(*now_line_start,'\r', *now_line_used)) != NULL)
		{
			if ((now_line_end + 1 >= resp_buf_head) || (now_line_end[1] != '\n'))
			{
				//No \r\n found; more data needed
				DEBUG(MODULE_NAME " plugin: No more line terminators found; waiting for more data");
				break;
			} else {
				//Received complete line
				*now_line_end = 0;
				DEBUG(MODULE_NAME " plugin: Line completed: %s", *now_line_start);
				*now_line_used = now_line_end - *now_line_start;

				//Check if last line
				if ((now_line_start-p_line_starts) >= p_line_count)
				{
					break;
				} else {
					//Move to next line
					//FIXME: This code seems not to work correctly if there's just one line
					++now_line_used;
					++now_line_start;
					*now_line_start = now_line_end + 2; //Move past \r\n
					*now_line_used  = resp_buf_head - *now_line_start;
					DEBUG(MODULE_NAME " plugin: resp_buf = %s, resp_buf_head-resp_buf = %lu, resp_buf_used = %lu, *now_line_used = %lu, **now_line_start = %s", p_line_starts[0], resp_buf_head-p_line_starts[0],resp_buf_used,*now_line_used,*now_line_start);
				}
			}
		}
		DEBUG(MODULE_NAME " plugin: Looping for more data");

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
	const size_t resp_buf_size = 4096;
	char *resp_buf = malloc(resp_buf_size);
	memset(resp_buf,0,resp_buf_size);

	char **line_starts = malloc(sizeof(char*  )*p_resp_n_lines);
	size_t *line_size  = malloc(sizeof(size_t*)*p_resp_n_lines);
	memset(line_starts,0,sizeof(char*)*p_resp_n_lines);
	memset(line_size,  0,sizeof(size_t*)*p_resp_n_lines);

	line_starts[0] = resp_buf;

	if (wxt_process_command(p_socket, p_command, resp_buf_size, p_resp_n_lines, line_starts, line_size) != 0)
	{
		ERROR(MODULE_NAME " plugin:  wxt_process_command(p_socket,p_command,resp_buf_size, p_resp_n_lines, line_starts, line_size) failed");
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
	
	DEBUG(MODULE_NAME " plugin: Received all data");
	for (i=0; i<p_resp_n_lines; ++i)
	{
		DEBUG(MODULE_NAME " plugin: Line[%d] = \"%s\" (size=%lu)", i,line_starts[i],line_size[i]);
	}
	
	//Skip header
	if ((line_size[0]>strlen(p_line_header)) && (memcmp(line_starts[0], p_line_header,strlen(p_line_header)) == 0))
	{
		line_starts[0] += strlen(p_line_header);
		line_size[0]   -= strlen(p_line_header); 
	} else {
		WARNING(MODULE_NAME " plugin: Line[0]: Missing header");
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
	DEBUG(MODULE_NAME " plugin: Splitting into up to %lu var/val pairs", vars_size);
	wxt_split_varval(line_starts[0], line_size[0], p_var_term, p_val_term, vars, vals, &vars_size );

	int now_var;
	for (now_var = 0; now_var < vars_size; ++now_var)
	{
		DEBUG(MODULE_NAME " plugin: var[%d]: \"%s\" = \"%s\"", now_var, vars[now_var], vals[now_var]);
	}
	*p_vars_size = vars_size;
	
	free(line_starts);
	free(line_size);
	return 0;
}


static const char *wxt_pair_get(char **p_vars, char **p_vals, const size_t p_vars_size, const char *p_searched_var)
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


static void wxt_store_var(char **p_vars, char **p_vals, const size_t p_vars_size, const char *p_searched_var, char p_unit_valid, double *p_dest)
{
	const char *wxt_val;
	wxt_val = wxt_pair_get(p_vars, p_vals, p_vars_size, p_searched_var);
	if (wxt_val != NULL)
	{
		//The variable exists 
		char *wxt_unit;
		double val = strtod(wxt_val,&wxt_unit);
		DEBUG(MODULE_NAME " plugin: Variable %s: Value = %g, Unit: '%s', expected: '%c'", p_searched_var, val, wxt_unit?wxt_unit:"NULL", p_unit_valid);
		if ((p_unit_valid == 0) || ((wxt_unit != NULL) && (wxt_unit[0] == p_unit_valid)))
		{
			//If there is a unit check requested, only store the value if the unit is correct
			*p_dest = val;
		} else {
			if (wxt_unit != NULL)
			{
				DEBUG(MODULE_NAME " plugin: Variable %s: Invalid unit '%c' (Expected: '%c')", p_searched_var, wxt_unit[0], p_unit_valid);
			} else {
				DEBUG(MODULE_NAME " plugin: Variable %s: Invalid unit (none) (Expected: '%c')", p_searched_var, p_unit_valid); 
			}
		}
	}
}


/* Get and print status from weather station */
static int wxt_query(const char *p_host, const char *p_port, struct wxt_detail_s *p_wxt_detail)
{
	assert(p_host);assert(p_port);

	//Retry
	int now_try = 0;
	int failed = 1;
	int sock = -1;
	DEBUG(MODULE_NAME " plugin: Connecting to %s:%s", p_host, p_port);
	while (1)
	{
		//Resolve host, Open socket, Set timeout and connect to Serial2Tcp server
		sock = connect_client(p_host, p_port, AF_UNSPEC, SOCK_STREAM,g_conf_timeout);
		
		if (sock != -1)
		{
			failed = 0;
			break;
		}
		if (++now_try > g_conf_retries)
		{
			break;
		}

		DEBUG(MODULE_NAME " plugin: Retrying...");
	}
	if (failed)
	{
		DEBUG(MODULE_NAME " plugin: Connection failed. errno=%d, msg=\"%s\"", errno, strerror(errno));
		return -1;
	} 

	failed = 1;
	size_t vars_size = DEFAULT_VARS_SIZE;
	char **vars=malloc(sizeof(char*)*DEFAULT_VARS_SIZE);
	char **vals=malloc(sizeof(char*)*DEFAULT_VARS_SIZE);
	do
	{
		//Request data
		#define WXT_TAG_COM_SETTINGS       "0XU"
		#define WXT_COMMAND_COM_SETTINGS   WXT_TAG_COM_SETTINGS ",M\r\n"
		#define WXT_COMMAND_COM_SET_ASCII  WXT_TAG_COM_SETTINGS ",M=P\r\n"
		#define WXT_COMMAND_GET_WIND_DATA  "0R1\r\n"
		#define WXT_COMMAND_GET_THP_DATA   "0R2\r\n"
		#define WXT_COMMAND_GET_RAIN_DATA  "0R3\r\n"
		#define WXT_COMMAND_GET_OTHER_DATA "0R5\r\n"
		#define WXT_RESP_COM_SET_ASCII_N_LINES 1
		#define WXT_RESP_COM_SETTINGS_N_LINES  1
		#define WXT_RESP_WIND_DATA_N_LINES  1
		#define WXT_RESP_THP_DATA_N_LINES   1
		#define WXT_RESP_RAIN_DATA_N_LINES  1
		#define WXT_RESP_OTHER_DATA_N_LINES 1
		#define WXT_RESP_0R_N_LINES 3

		//Check settings of WXT
		if ((failed=wxt_process_split_command(sock, WXT_COMMAND_COM_SETTINGS, WXT_RESP_COM_SETTINGS_N_LINES, 
						WXT_TAG_COM_SETTINGS ",",'=',',',&vars,&vals,&vars_size)) != 0)
		{
			break;
		}

		const char *wxt_protocol = wxt_pair_get(vars,vals,vars_size,"M");
		if (wxt_protocol == NULL)
		{
			ERROR(MODULE_NAME " plugin: Didn't receive protocol info");
			break;
		}
		if (strcmp(wxt_protocol,"P") != 0)
		{
			//FIXME: Automatically change protocol to "NMEA 0183 v3.0 query"
			INFO(MODULE_NAME " plugin: Unexpected WXT protocol (M=%s). Changing to ASCII, polled (M=P)",wxt_protocol);
			if ((failed=wxt_process_split_command(sock,WXT_COMMAND_COM_SET_ASCII,WXT_RESP_COM_SET_ASCII_N_LINES,
						WXT_TAG_COM_SETTINGS ",",'=',',',&vars,&vals,&vars_size)) != 0)
			{
				break;
			}
		}

		//Get wind data
		vars_size = DEFAULT_VARS_SIZE;
		if ((failed=wxt_process_split_command(sock,WXT_COMMAND_GET_WIND_DATA,WXT_RESP_WIND_DATA_N_LINES,"0R1,",'=',',',&vars,&vals,&vars_size)) != 0)
		{
			break;
		}

		//Typical response: 0R1,Dn=236D,Dm=283D,Dx=031D,Sn=0.0M,Sm=1.0M,Sx=2.2M
		wxt_store_var(vars,vals,vars_size, "Dn", 'D', &p_wxt_detail->wind_dir_min);
		wxt_store_var(vars,vals,vars_size, "Dm", 'D', &p_wxt_detail->wind_dir_avg);
		wxt_store_var(vars,vals,vars_size, "Dx", 'D', &p_wxt_detail->wind_dir_max);
		wxt_store_var(vars,vals,vars_size, "Sn", 'M', &p_wxt_detail->wind_speed_min);
		wxt_store_var(vars,vals,vars_size, "Sm", 'M', &p_wxt_detail->wind_speed_avg);
		wxt_store_var(vars,vals,vars_size, "Sx", 'M', &p_wxt_detail->wind_speed_max);

		//Get temperature/humidity/pressure data
		vars_size = DEFAULT_VARS_SIZE;
		if ((failed=wxt_process_split_command(sock,WXT_COMMAND_GET_THP_DATA,WXT_RESP_THP_DATA_N_LINES,"0R2,",'=',',',&vars,&vals,&vars_size)) != 0)
		{
			break;
		}
		
		//Typical response: 0R2,Ta=23.6C,Ua=14.2P,Pa=1026.6H
		wxt_store_var(vars,vals,vars_size, "Ta", 'C', &p_wxt_detail->temp_air);
		wxt_store_var(vars,vals,vars_size, "Ua", 'P', &p_wxt_detail->humi_air); //Values received as a percentage -> collectd expects percentage
		wxt_store_var(vars,vals,vars_size, "Pa", 'H', &p_wxt_detail->pres_air); //Values received in hPa -> collectd expects Pa

		//Get rain/hail data
		vars_size = DEFAULT_VARS_SIZE;
		if ((failed=wxt_process_split_command(sock,WXT_COMMAND_GET_RAIN_DATA,WXT_RESP_RAIN_DATA_N_LINES,"0R3,",'=',',',&vars,&vals,&vars_size)) != 0)
		{
			INFO("Error decoding WXT_RAIN data"); 
			break;
		}
		//Typical response: 0R3,Rc=0.0M,Rd=0s,Ri=0.0M,Hc=0.0M,Hd=0s,Hi=0.0M,Rp=0.0M,Hp=0.0M
		wxt_store_var(vars,vals,vars_size, "Rc", 'M', &p_wxt_detail->rain_sum_mm);      //(mm)
		wxt_store_var(vars,vals,vars_size, "Rd", 's', &p_wxt_detail->rain_duration);    //(sec)
		wxt_store_var(vars,vals,vars_size, "Ri", 'M', &p_wxt_detail->rain_intensity);   //(mm/h)
		wxt_store_var(vars,vals,vars_size, "Rp", 'M', &p_wxt_detail->rain_intensity_peak); //(mm/h)
		wxt_store_var(vars,vals,vars_size, "Hc", 'M', &p_wxt_detail->hail_sum_hits);    //(hits/cm2)
		wxt_store_var(vars,vals,vars_size, "Hd", 's', &p_wxt_detail->hail_duration);    //(sec)
		wxt_store_var(vars,vals,vars_size, "Hi", 'M', &p_wxt_detail->hail_intensity);   //(hits/cm2/h)
		wxt_store_var(vars,vals,vars_size, "Hp", 'M', &p_wxt_detail->hail_intensity_peak); //(hits/cm2/h)

		//Get other data
		vars_size = DEFAULT_VARS_SIZE;
		if ((failed=wxt_process_split_command(sock,WXT_COMMAND_GET_OTHER_DATA,WXT_RESP_OTHER_DATA_N_LINES,"0R5,",'=',',',&vars,&vals,&vars_size)) != 0)
		{
			INFO("Error decoding WXT_OTHER data"); 
			break;
		}
		//Typical response: 0R5,Th=25.9C,Vh=12.0N,Vs=15.2V,Vr=3.475V,Id=HEL___
		p_wxt_detail->volt_heating = 0;
		wxt_store_var(vars,vals,vars_size, "Th", 'C', &p_wxt_detail->temp_heating);   //(degC)
		wxt_store_var(vars,vals,vars_size, "Vh", 'V', &p_wxt_detail->volt_heating);   //(Volt) Unit 'N' = off
		wxt_store_var(vars,vals,vars_size, "Vs", 'V', &p_wxt_detail->volt_supply);    //(Volt)
		wxt_store_var(vars,vals,vars_size, "Vr", 'V', &p_wxt_detail->volt_reference); //(Volt)

		failed = 0;
	} while (0);

	close(sock);

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
		INFO("Failed to receive all data");
	}

	//All good
	return 0;
}


static int wxt_config (const char *key, const char *value)
{
	int host_port_updated=0;
	if (strcasecmp (key, "host") == 0)
	{
		if (g_conf_host != NULL)
		{
			free (g_conf_host);
			g_conf_host = NULL;
		}
		if ((g_conf_host = strdup (value)) == NULL)
		{
			return 1;
		}
		host_port_updated=1;	
	} else if (strcasecmp (key, "port") == 0)
	{
		if (g_conf_port != NULL)
		{
			free (g_conf_port);
			g_conf_port = NULL;
		}
		if ((g_conf_port = strdup (value)) == NULL)
		{
			return 1;
		}
		host_port_updated=1;	
	} else if (strcasecmp (key, "timeout") == 0)
	{
		g_conf_timeout = atoi(value);
		if (g_conf_timeout <= 0)
		{
			return 1;
		}
	} else if (strcasecmp (key, "retries") == 0)
	{
		g_conf_retries = atoi(value);
		if (g_conf_retries <= 1)
		{
			return 1;
		}
	} else {
		return 2;
	}
	if ((host_port_updated != 0) && (g_conf_host != NULL) && (g_conf_port != NULL))
	{
		size_t host_service_size = strlen(g_conf_host)+strlen(g_conf_port)+2;
		if (g_conf_host_service != NULL)
		{
			free (g_conf_host_service );
			g_conf_host_service = NULL;
		}
		
		if ((g_conf_host_service = malloc(host_service_size)) == NULL)
		{
			return 1;
		}
		snprintf(g_conf_host_service,host_service_size,"%s-%s", g_conf_host, g_conf_port);
		host_port_updated=1;	
	}
	return 0;
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

	sstrncpy (vl.host,            hostname_g,            sizeof (vl.host));
	sstrncpy (vl.plugin,          MODULE_NAME,           sizeof (vl.plugin));
	sstrncpy (vl.plugin_instance, g_conf_host_service,   sizeof (vl.plugin_instance));
	sstrncpy (vl.type,            p_value_type,          sizeof (vl.type));
	sstrncpy (vl.type_instance,   p_value_type_instance, sizeof (vl.type_instance));

	plugin_dispatch_values (&vl);
}


static void value_submit (struct wxt_detail_s *p_wxt_detail)
{
	value_submit_generic ("temperature", "temp_air",            p_wxt_detail->temp_air);
	value_submit_generic ("temperature", "temp_heating",        p_wxt_detail->temp_heating);
	value_submit_generic ("humidity",    "humi_air",            p_wxt_detail->humi_air);
	value_submit_generic ("pressure",    "pres_air",            p_wxt_detail->pres_air);
	value_submit_generic ("gauge",       "rain_sum_mm",         p_wxt_detail->rain_sum_mm);
	value_submit_generic ("gauge",       "rain_duration",       p_wxt_detail->rain_duration);
	value_submit_generic ("gauge",       "rain_intensity",      p_wxt_detail->rain_intensity);
	value_submit_generic ("gauge",       "rain_intensity_peak", p_wxt_detail->rain_intensity_peak);
	value_submit_generic ("gauge",       "hail_sum_hit",        p_wxt_detail->hail_sum_hits);
	value_submit_generic ("gauge",       "hail_duration",       p_wxt_detail->hail_duration);
	value_submit_generic ("gauge",       "hail_intensity",      p_wxt_detail->hail_intensity);
	value_submit_generic ("gauge",       "hail_intensity_peak", p_wxt_detail->hail_intensity_peak);
	value_submit_generic ("angle",       "wind_dir_min",        p_wxt_detail->wind_dir_min);
	value_submit_generic ("angle",       "wind_dir_avg",        p_wxt_detail->wind_dir_avg);
	value_submit_generic ("angle",       "wind_dir_max",        p_wxt_detail->wind_dir_max);
	value_submit_generic ("speed",       "wind_speed_min",      p_wxt_detail->wind_speed_min);
	value_submit_generic ("speed",       "wind_speed_avg",      p_wxt_detail->wind_speed_avg);
	value_submit_generic ("speed",       "wind_speed_max",      p_wxt_detail->wind_speed_max);
	value_submit_generic ("voltage",     "volt_supply",         p_wxt_detail->volt_supply);
	value_submit_generic ("voltage",     "volt_heating",        p_wxt_detail->volt_heating);
	value_submit_generic ("voltage",     "volt_reference",      p_wxt_detail->volt_reference);
}


static int wxt_read (void)
{
	struct wxt_detail_s wxt_detail;
	int status;

	wxt_detail.temp_air       = -300.0;
	wxt_detail.temp_heating   = -300.0;
	wxt_detail.humi_air       = -1.0;
	wxt_detail.pres_air       = -1.0;
	wxt_detail.rain_sum_mm    = -1.0;
	wxt_detail.rain_duration  = -1.0;
	wxt_detail.rain_intensity = -1.0;
	wxt_detail.hail_sum_hits  = -1.0;
	wxt_detail.hail_duration  = -1.0;
	wxt_detail.hail_intensity = -1.0;
	wxt_detail.rain_intensity_peak = -1.0;
	wxt_detail.hail_intensity_peak = -1.0;
	wxt_detail.wind_dir_min   = -1.0;
	wxt_detail.wind_dir_avg   = -1.0;
	wxt_detail.wind_dir_max   = -1.0;
	wxt_detail.wind_speed_min = -1.0;
	wxt_detail.wind_speed_avg = -1.0;
	wxt_detail.wind_speed_max = -1.0;
	wxt_detail.volt_supply    = -1.0;
	wxt_detail.volt_heating   = -1.0;
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
		return 1;
	}


	INFO("Ta = %g degC, Th = %g degC, Ua = %g %%RH, Ua = %g hPa, Vs = %g V, Vh = %g V, Vr = %g V", 
		wxt_detail.temp_air, wxt_detail.temp_heating, wxt_detail.humi_air, wxt_detail.pres_air, wxt_detail.volt_supply, wxt_detail.volt_heating, wxt_detail.volt_reference
	);
	INFO("Rain: sum = %g mm, sec = %g s, mmh = %g mm/h, peak = %g mm/h    Hail: sum = %g hits/cm2, sec = %g s, hith = %g hits/cm2/h, peak = %g hits/cm2/h",
		wxt_detail.rain_sum_mm, wxt_detail.rain_duration, wxt_detail.rain_intensity, wxt_detail.rain_intensity_peak, wxt_detail.hail_sum_hits, wxt_detail.hail_duration, wxt_detail.hail_intensity, wxt_detail.hail_intensity_peak
	);
	INFO("Wind dir: last = %g deg, min = %g deg, max = %g deg    Wind speed: last = %g m/s, min = %g m/s, max = %g m/s",
		wxt_detail.wind_dir_avg, wxt_detail.wind_dir_min, wxt_detail.wind_dir_max, wxt_detail.wind_speed_avg, wxt_detail.wind_speed_min, wxt_detail.wind_speed_max
	);

	value_submit (&wxt_detail);

	return 0;
} /* wxt_read */


void module_register (void)
{
	plugin_register_config   ("wxt", wxt_config, g_config_keys, g_config_keys_num);
	plugin_register_read     ("wxt", wxt_read);
	plugin_register_shutdown ("wxt", wxt_shutdown);
} /* void module_register */
