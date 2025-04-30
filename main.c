/******************************************************************************
* Copyright (c) 2024-present JC Wang. All rights reserved
*
*   https://github.com/sonic-ng/sonic-osmgr
*
* This Source Code Form is subject to the terms of the Apache License 2.0
* You can obtain a copy at http://www.apache.org/licenses/LICENSE-2.0
******************************************************************************/

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <pthread.h>
#include <ifaddrs.h>
#include <net/if.h>
#include <sys/socket.h>
#include <linux/netlink.h>
#include <linux/rtnetlink.h>
#include <crossdb.h>

xdb_conn_t	*g_pConn;

static int port_trig (xdb_conn_t *pConn, xdb_res_t *pRes, uint32_t type, xdb_row_t *pNewRow, xdb_row_t *pOldRow, void *pArg)
{
	const char *ifname;
	xdb_res_t *pRes2;
	xdb_row_t *pRow;
	char cmd[1024];
	switch (type) {
	case XDB_TRIG_AFT_INS:
	case XDB_TRIG_AFT_UPD:
		ifname = xdb_col_str(pRes, pNewRow, "ifname");
		pRes2 = xdb_bexec (pConn, "SELECT * FROM osmgr.intf WHERE ifname = ?", ifname);
		pRow = xdb_fetch_row (pRes2);
		if (pRow != NULL) {
			sprintf (cmd, "ip link set dev %s %s", xdb_col_str(pRes, pNewRow, "ifname"), xdb_col_bool(pRes, pNewRow, "admin_status")?"up":"down");
			printf ("%s\n", cmd);
			system (cmd);
			xdb_bexec (pConn, "INSERT INTO appl.port (ifname, lanes, speed) VALUES (?, ?, ?)", ifname, xdb_col_str(pRes, pNewRow, "lanes"), xdb_col_str(pRes, pNewRow, "speed"));
		} else {
			printf ("%s doesn't exist\n", ifname);
		}
		xdb_free_result (pRes2);
		break;
	}
	return 0;
}

static int intf_trig (xdb_conn_t *pConn, xdb_res_t *pRes, uint32_t type, xdb_row_t *pNewRow, xdb_row_t *pOldRow, void *pArg)
{
	const char *ifname;
	xdb_res_t *pRes2;
	xdb_row_t *pRow;
	char cmd[1024], prefix[128];
	const xdb_inet_t	*ipaddr;
	
	switch (type) {
	case XDB_TRIG_AFT_INS:
		ifname = xdb_col_str(pRes, pNewRow, "ifname");
		pRes2 = xdb_bexec (pConn, "SELECT * FROM osmgr.intf WHERE ifname = ?", ifname);
		pRow = xdb_fetch_row (pRes2);
		if (pRow != NULL) {
			ipaddr = xdb_col_inet (pRes, pNewRow, "ipaddr");
			xdb_inet_sprintf (ipaddr, prefix, sizeof(prefix));
			sprintf (cmd, "ip addr add %s dev %s", prefix, xdb_col_str(pRes, pNewRow, "ifname"));
			printf ("%s\n", cmd);
			system (cmd);
			xdb_bexec (pConn, "INSERT INTO appl.intf (ifname, ipaddr) VALUES (?, ?)", ifname, ipaddr);
		} else {
			printf ("%s doesn't exist\n", ifname);
		}
		xdb_free_result (pRes2);
		break;

	case XDB_TRIG_AFT_DEL:
		ifname = xdb_col_str(pRes, pNewRow, "ifname");
		pRes2 = xdb_bexec (pConn, "SELECT * FROM osmgr.intf WHERE ifname = ?", ifname);
		pRow = xdb_fetch_row (pRes2);
		if (pRow != NULL) {
			ipaddr = xdb_col_inet (pRes, pNewRow, "ipaddr");
			xdb_inet_sprintf (ipaddr, prefix, sizeof(prefix));
			sprintf (cmd, "ip addr del %s dev %s", prefix, xdb_col_str(pRes, pNewRow, "ifname"));
			printf ("%s\n", cmd);
			system (cmd);
			xdb_bexec (pConn, "DELETE FROM appl.intf WHERE ifname = ? AND ipaddr = ?", ifname, ipaddr);
		} else {
			printf ("%s doesn't exist\n", ifname);
		}
		break;
	}
	return 0;
}

static int interface_dump ()
{
	xdb_res_t	*pRes;
	struct ifaddrs *ifaddr, *ifa;
	int exists = 0;

	if (getifaddrs(&ifaddr) == -1) {
		perror("getifaddrs");
		return -1;
	}

	for (ifa = ifaddr; ifa != NULL; ifa = ifa->ifa_next) {
		if (ifa->ifa_name == NULL) {
			continue;
		}
		int family = ifa->ifa_addr->sa_family;
		printf("%-8s %s (%d)\n",
			   ifa->ifa_name,
			   (family == AF_PACKET) ? "AF_PACKET" :
			   (family == AF_INET) ? "AF_INET" :
			   (family == AF_INET6) ? "AF_INET6" : "???",
			   family);
		if (AF_PACKET == family) {
			pRes = xdb_bexec (g_pConn, "INSERT INTO osmgr.intf (ifname) VALUES (?)", ifa->ifa_name);
			XDB_RESCHK (pRes, "");
		}
	}

	freeifaddrs(ifaddr);
	return exists;
}

#define BUFFER_SIZE 8192

void parse_rtattr(struct rtattr *tb[], int max, struct rtattr *rta, int len)
{
	memset(tb, 0, sizeof(struct rtattr *) * (max + 1));
	while (RTA_OK(rta, len)) {
		if (rta->rta_type <= max)
			tb[rta->rta_type] = rta;
		rta = RTA_NEXT(rta, len);
	}
}

static void *interface_netlink() 
{
	xdb_res_t	*pRes;
	int sock_fd;
	struct sockaddr_nl sa;
	struct nlmsghdr *nh;
	char buffer[BUFFER_SIZE];
	struct iovec iov = { buffer, sizeof(buffer) };
	struct msghdr msg = { (void*)&sa, sizeof(sa), &iov, 1, NULL, 0, 0 };

	// Create a Netlink socket
	sock_fd = socket(AF_NETLINK, SOCK_RAW, NETLINK_ROUTE);
	if (sock_fd < 0) {
		perror("socket");
		return NULL;
	}

	// Initialize the sockaddr_nl structure
	memset(&sa, 0, sizeof(sa));
	sa.nl_family = AF_NETLINK;
	sa.nl_groups = RTMGRP_LINK;

	// Bind the socket
	if (bind(sock_fd, (struct sockaddr*)&sa, sizeof(sa)) < 0) {
		perror("bind");
		close(sock_fd);
		return NULL;
	}

	// Listen for messages
	while (1) {
		ssize_t len = recvmsg(sock_fd, &msg, 0);
		if (len < 0) {
			perror("recvmsg");
			close(sock_fd);
			return NULL;
		}

		for (nh = (struct nlmsghdr*)buffer; NLMSG_OK(nh, len); nh = NLMSG_NEXT(nh, len)) {
			if (nh->nlmsg_type == NLMSG_DONE) {
				break;
			} else if (nh->nlmsg_type == RTM_NEWLINK) {
				struct rtattr *tb[IFLA_MAX + 1];
				struct ifinfomsg *ifi;
				ifi = NLMSG_DATA(nh);
				parse_rtattr(tb, IFLA_MAX, IFLA_RTA(ifi), nh->nlmsg_len - NLMSG_LENGTH(sizeof(*ifi)));
				if (tb[IFLA_IFNAME]) {
					const char *ifname = (char*)RTA_DATA(tb[IFLA_IFNAME]);
					printf("New network interface created: %s\n", ifname);
					pRes = xdb_bexec (g_pConn, "INSERT INTO osmgr.intf (ifname) VALUES (?)", ifname);
					XDB_RESCHK (pRes, "");
				} else {
			   		printf("Network interface created name not found\n");
				}
			}
		}
	}

	close(sock_fd);
	return NULL;
}

int main (int argc, char **argv)
{
	xdb_res_t	*pRes;
	xdb_row_t	*pRow;


	xdb_conn_t	*pConn = xdb_open (argc > 1 ? argv[1] : ":memory:");
	XDB_CHECK (NULL != pConn, printf ("failed to create DB\n"); return -1;);
	g_pConn = pConn;

	// Create my own database
	pRes = xdb_exec (pConn, "CREATE DATABASE osmgr ENGINE = MEMORY");
	XDB_RESCHK (pRes, "");
	pRes = xdb_exec (pConn, "CREATE TABLE IF NOT EXISTS osmgr.intf (ifname VARCHAR PRIMARY KEY, admin_status BOOL)");
	XDB_RESCHK (pRes, "");

	// Get Linux interfaces list
	pthread_t pid;
	pthread_create (&pid, NULL, interface_netlink, NULL);
	interface_dump ();

	pRes = xdb_exec (pConn, "CREATE DATABASE config ENGINE = MEMORY");
	XDB_RESCHK (pRes, "");
	pRes = xdb_exec (pConn, "USE config");
	XDB_RESCHK (pRes, "");
	pRes = xdb_exec (pConn, "SOURCE '/sonic-ng/schema/config/port.sql'");
	pRes = xdb_exec (pConn, "SOURCE '/sonic-ng/schema/config/intf.sql'");
	XDB_RESCHK (pRes, "");

	pRes = xdb_exec (pConn, "CREATE DATABASE appl ENGINE = MEMORY");
	XDB_RESCHK (pRes, "");
	pRes = xdb_exec (pConn, "USE appl");
	XDB_RESCHK (pRes, "");
	pRes = xdb_exec (pConn, "SOURCE '/sonic-ng/schema/appl/port.sql'");
	pRes = xdb_exec (pConn, "SOURCE '/sonic-ng/schema/appl/intf.sql'");
	XDB_RESCHK (pRes, "");

	xdb_create_func ("port_trig", XDB_FUNC_TRIG, "C", port_trig, NULL);
	pRes = xdb_exec (pConn, "CREATE TRIGGER port_ins AFTER INSERT ON config.port CALL port_trig");
	XDB_RESCHK (pRes, "");
	pRes = xdb_exec (pConn, "CREATE TRIGGER port_upd AFTER UPDATE ON config.port CALL port_trig");
	XDB_RESCHK (pRes, "");
	pRes = xdb_exec (pConn, "CREATE TRIGGER port_del AFTER DELETE ON config.port CALL port_trig");
	XDB_RESCHK (pRes, "");

	xdb_create_func ("intf_trig", XDB_FUNC_TRIG, "C", intf_trig, NULL);
	pRes = xdb_exec (pConn, "CREATE TRIGGER port_ins AFTER INSERT ON config.intf CALL intf_trig");
	XDB_RESCHK (pRes, "");
	pRes = xdb_exec (pConn, "CREATE TRIGGER port_upd AFTER UPDATE ON config.intf CALL intf_trig");
	XDB_RESCHK (pRes, "");
	pRes = xdb_exec (pConn, "CREATE TRIGGER port_del AFTER DELETE ON config.intf CALL intf_trig");
	XDB_RESCHK (pRes, "");

	pRes = xdb_exec (pConn, "CREATE REPLICA sysdb2osmgr HOST='127.0.0.1', PORT=7777, DO_TABLE=(config.port,config.intf)");
	XDB_RESCHK (pRes, "");

	pRes = xdb_exec (pConn, "CREATE SERVER sysdb PORT=8001");
	XDB_RESCHK (pRes, "");

	// Publish appl port
	xdb_conn_t	*pSysDbConn = xdb_connect (NULL, NULL, NULL, NULL, 7777);
	if (pSysDbConn == NULL) {
		printf ("!!! SysDB is not started yet\n");
	} else {
		pRes = xdb_exec (pSysDbConn, "CREATE REPLICA osmgr2sysdb HOST='127.0.0.1', PORT=8001, DO_TABLE=(appl.port,appl.intf)");
	}

	xdb_exec (pConn, "SHELL PROMPT='OsMgr> '");

	return 0;
}
