/*
 * Copyright (C) 2009-2010 Nippon Telegraph and Telephone Corporation.
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License version
 * 2 as published by the Free Software Foundation.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program. If not, see <http://www.gnu.org/licenses/>.
 */
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <netinet/tcp.h>
#include <sys/epoll.h>

#include "sheep_priv.h"

int is_io_request(unsigned op)
{
	int ret = 0;

	switch (op) {
	case SD_OP_CREATE_AND_WRITE_OBJ:
	case SD_OP_REMOVE_OBJ:
	case SD_OP_READ_OBJ:
	case SD_OP_WRITE_OBJ:
		ret = 1;
		break;
	default:
		break;
	}

	return ret;
}

void resume_pending_requests(void)
{
	struct request *next, *tmp;

	list_for_each_entry_safe(next, tmp, &sys->req_wait_for_obj_list,
				 r_wlist) {
		struct cpg_event *cevent = &next->cev;

		list_del(&next->r_wlist);
		list_add_tail(&cevent->cpg_event_list, &sys->cpg_event_siblings);
	}

	if (!list_empty(&sys->cpg_event_siblings))
		start_cpg_event_work();
}

static int is_access_local(struct sheepdog_node_list_entry *e, int nr_nodes,
			   uint64_t oid, int copies)
{
	int i, n;

	for (i = 0; i < copies; i++) {
		n = obj_to_sheep(e, nr_nodes, oid, i);

		if (is_myself(&e[n]))
			return 1;
	}

	return 0;
}

static void setup_access_to_local_objects(struct request *req)
{
	struct sd_obj_req *hdr = (struct sd_obj_req *)&req->rq;
	int copies;

	if (hdr->flags & SD_FLAG_CMD_DIRECT) {
		req->local_oid[0] = hdr->oid;

		if (hdr->flags & SD_FLAG_CMD_COW)
			req->local_oid[1] = hdr->cow_oid;

		return;
	}

	copies = hdr->copies;
	if (!copies)
		copies = sys->nr_sobjs;

	if (is_access_local(req->entry, req->nr_nodes, hdr->oid, copies))
		req->local_oid[0] = hdr->oid;

	if ((hdr->flags & SD_FLAG_CMD_COW) &&
	    is_access_local(req->entry, req->nr_nodes, hdr->cow_oid, copies))
		req->local_oid[1] = hdr->cow_oid;
}

static void __done(struct work *work, int idx)
{
	struct request *req = container_of(work, struct request, work);
	struct sd_req *hdr = (struct sd_req *)&req->rq;
	int again = 0;

	switch (hdr->opcode) {
	case SD_OP_NEW_VDI:
	case SD_OP_DEL_VDI:
	case SD_OP_LOCK_VDI:
	case SD_OP_RELEASE_VDI:
	case SD_OP_GET_VDI_INFO:
	case SD_OP_MAKE_FS:
	case SD_OP_SHUTDOWN:
		/* request is forwarded to cpg group */
		return;
	}

	if (is_io_request(hdr->opcode)) {
		struct cpg_event *cevent = &req->cev;

		list_del(&req->r_wlist);

		sys->nr_outstanding_io--;
		/*
		 * TODO: if the request failed due to epoch unmatch,
		 * we should retry here (adds this request to the tail
		 * of sys->cpg_event_siblings.
		 */

		if (!(req->rq.flags & SD_FLAG_CMD_DIRECT) &&
		    (req->rp.result == SD_RES_OLD_NODE_VER ||
		     req->rp.result == SD_RES_NEW_NODE_VER)) {


			req->rq.epoch = sys->epoch;
			req->nr_nodes = setup_ordered_sd_node_list(req);
			setup_access_to_local_objects(req);

			list_add_tail(&cevent->cpg_event_list, &sys->cpg_event_siblings);
			again = 1;
		}

		resume_pending_requests();
		resume_recovery_work();
	}

	if (!again)
		req->done(req);
}

static void queue_request(struct request *req)
{
	struct cpg_event *cevent = &req->cev;

	struct sd_req *hdr = (struct sd_req *)&req->rq;
	struct sd_rsp *rsp = (struct sd_rsp *)&req->rp;

	if (hdr->opcode == SD_OP_KILL_NODE) {
		log_close();
		exit(1);
	}

	if (sys->status == SD_STATUS_SHUTDOWN) {
		rsp->result = SD_RES_SHUTDOWN;
		req->done(req);
		return;
	}

	if (sys->status == SD_STATUS_WAIT_FOR_FORMAT ||
	    sys->status == SD_STATUS_WAIT_FOR_JOIN) {
		/* TODO: cleanup */
		switch (hdr->opcode) {
		case SD_OP_STAT_CLUSTER:
		case SD_OP_MAKE_FS:
		case SD_OP_GET_NODE_LIST:
		case SD_OP_READ_VDIS:
			break;
		default:
			if (sys->status == SD_STATUS_WAIT_FOR_FORMAT)
				rsp->result = SD_RES_WAIT_FOR_FORMAT;
			else
				rsp->result = SD_RES_WAIT_FOR_JOIN;
			req->done(req);
			return;
		}
	}

	switch (hdr->opcode) {
	case SD_OP_CREATE_AND_WRITE_OBJ:
	case SD_OP_REMOVE_OBJ:
	case SD_OP_READ_OBJ:
	case SD_OP_WRITE_OBJ:
	case SD_OP_STAT_SHEEP:
	case SD_OP_GET_OBJ_LIST:
		req->work.fn = store_queue_request;
		break;
	case SD_OP_GET_NODE_LIST:
	case SD_OP_GET_VM_LIST:
	case SD_OP_NEW_VDI:
	case SD_OP_DEL_VDI:
	case SD_OP_LOCK_VDI:
	case SD_OP_RELEASE_VDI:
	case SD_OP_GET_VDI_INFO:
	case SD_OP_MAKE_FS:
	case SD_OP_SHUTDOWN:
	case SD_OP_STAT_CLUSTER:
		req->work.fn = cluster_queue_request;
		break;
	case SD_OP_READ_VDIS:
		rsp->result = read_vdis(req->data, hdr->data_length, &rsp->data_length);
		req->done(req);
		return;
	default:
		eprintf("unknown operation %d\n", hdr->opcode);
		rsp->result = SD_RES_SYSTEM_ERROR;
		req->done(req);
		return;
	}

	req->work.done = __done;

	list_del(&req->r_wlist);

	/*
	 * we set epoch for non direct requests here. Note that we
	 * can't access to sys->epoch after calling
	 * start_cpg_event_work(that is, passing requests to work
	 * threads).
	 */
	if (!(hdr->flags & SD_FLAG_CMD_DIRECT))
		hdr->epoch = sys->epoch;

	req->nr_nodes = setup_ordered_sd_node_list(req);
	setup_access_to_local_objects(req);

	cevent->ctype = CPG_EVENT_REQUEST;
	list_add_tail(&cevent->cpg_event_list, &sys->cpg_event_siblings);
	start_cpg_event_work();
}

static struct request *alloc_request(struct client_info *ci, int data_length)
{
	struct request *req;

	req = zalloc(sizeof(struct request) + data_length);
	if (!req)
		return NULL;

	req->ci = ci;
	if (data_length)
		req->data = (char *)req + sizeof(*req);

	list_add(&req->r_siblings, &ci->reqs);
	INIT_LIST_HEAD(&req->r_wlist);

	return req;
}

static void free_request(struct request *req)
{
	list_del(&req->r_siblings);
	free(req);
}

static void req_done(struct request *req)
{
	list_add(&req->r_wlist, &req->ci->done_reqs);
	conn_tx_on(&req->ci->conn);
}

static void init_rx_hdr(struct client_info *ci)
{
	ci->conn.c_rx_state = C_IO_HEADER;
	ci->rx_req = NULL;
	ci->conn.rx_length = sizeof(struct sd_req);
	ci->conn.rx_buf = &ci->conn.rx_hdr;
}

static void client_rx_handler(struct client_info *ci)
{
	int ret;
	uint64_t data_len;
	struct connection *conn = &ci->conn;
	struct sd_req *hdr = &conn->rx_hdr;
	struct request *req;

	switch (conn->c_rx_state) {
	case C_IO_HEADER:
		ret = rx(conn, C_IO_DATA_INIT);
		if (!ret || conn->c_rx_state != C_IO_DATA_INIT)
			break;
	case C_IO_DATA_INIT:
		data_len = hdr->data_length;

		req = alloc_request(ci, data_len);
		if (!req) {
			conn->c_rx_state = C_IO_CLOSED;
			break;
		}
		ci->rx_req = req;

		/* use le_to_cpu */
		memcpy(&req->rq, hdr, sizeof(req->rq));

		if (data_len && hdr->flags & SD_FLAG_CMD_WRITE) {
			conn->c_rx_state = C_IO_DATA;
			conn->rx_length = data_len;
			conn->rx_buf = req->data;
		} else {
			conn->c_rx_state = C_IO_END;
			break;
		}
	case C_IO_DATA:
		ret = rx(conn, C_IO_END);
		break;
	default:
		eprintf("BUG: unknown state %d\n", conn->c_rx_state);
	}

	if (is_conn_dead(conn) || conn->c_rx_state != C_IO_END)
		return;

	/* now we have a complete command */

	req = ci->rx_req;

	init_rx_hdr(ci);

	if (hdr->flags & SD_FLAG_CMD_WRITE)
		req->rp.data_length = 0;
	else
		req->rp.data_length = hdr->data_length;

	req->done = req_done;

	queue_request(req);
}

static void init_tx_hdr(struct client_info *ci)
{
	struct sd_rsp *rsp = (struct sd_rsp *)&ci->conn.tx_hdr;
	struct request *req;

	if (ci->tx_req || list_empty(&ci->done_reqs))
		return;

	memset(rsp, 0, sizeof(*rsp));

	req = list_first_entry(&ci->done_reqs, struct request, r_wlist);
	list_del(&req->r_wlist);

	ci->tx_req = req;
	ci->conn.tx_length = sizeof(*rsp);
	ci->conn.c_tx_state = C_IO_HEADER;
	ci->conn.tx_buf = rsp;

	/* use cpu_to_le */
	memcpy(rsp, &req->rp, sizeof(*rsp));

	rsp->epoch = sys->epoch;
	rsp->opcode = req->rq.opcode;
	rsp->id = req->rq.id;
}

static void client_tx_handler(struct client_info *ci)
{
	int ret, opt;
	struct sd_rsp *rsp = (struct sd_rsp *)&ci->conn.tx_hdr;

again:
	init_tx_hdr(ci);
	if (!ci->tx_req) {
		conn_tx_off(&ci->conn);
		return;
	}

	opt = 1;
	setsockopt(ci->conn.fd, SOL_TCP, TCP_CORK, &opt, sizeof(opt));

	switch (ci->conn.c_tx_state) {
	case C_IO_HEADER:
		if (rsp->data_length)
			ret = tx(&ci->conn, C_IO_DATA_INIT, MSG_MORE);
		else
			ret = tx(&ci->conn, C_IO_DATA_INIT, 0);

		if (!ret)
			break;

		if (rsp->data_length) {
			ci->conn.tx_length = rsp->data_length;
			ci->conn.tx_buf = ci->tx_req->data;
			ci->conn.c_tx_state = C_IO_DATA;
		} else {
			ci->conn.c_tx_state = C_IO_END;
			break;
		}
	case C_IO_DATA:
		ret = tx(&ci->conn, C_IO_END, 0);
		if (!ret)
			break;
	default:
		break;
	}

	opt = 0;
	setsockopt(ci->conn.fd, SOL_TCP, TCP_CORK, &opt, sizeof(opt));

	if (is_conn_dead(&ci->conn) || ci->conn.c_tx_state != C_IO_END)
		return;

	if (ci->conn.c_tx_state == C_IO_END) {
		free_request(ci->tx_req);
		ci->tx_req = NULL;
		goto again;
	}
}

static void destroy_client(struct client_info *ci)
{
	close(ci->conn.fd);
	free(ci);
}

static struct client_info *create_client(int fd, struct cluster_info *cluster)
{
	struct client_info *ci;

	ci = zalloc(sizeof(*ci));
	if (!ci)
		return NULL;

	ci->conn.fd = fd;

	INIT_LIST_HEAD(&ci->reqs);
	INIT_LIST_HEAD(&ci->done_reqs);

	init_rx_hdr(ci);

	return ci;
}

static void client_handler(int fd, int events, void *data)
{
	struct client_info *ci = (struct client_info *)data;

	if (events & EPOLLIN)
		client_rx_handler(ci);

	if (!is_conn_dead(&ci->conn) && events & EPOLLOUT)
		client_tx_handler(ci);

	if (is_conn_dead(&ci->conn)) {
		dprintf("closed a connection, %d\n", fd);
		unregister_event(fd);
		destroy_client(ci);
	}
}

static void listen_handler(int listen_fd, int events, void *data)
{
	struct sockaddr_storage from;
	socklen_t namesize;
	int fd, ret, opt;
	struct client_info *ci;

	namesize = sizeof(from);
	fd = accept(listen_fd, (struct sockaddr *)&from, &namesize);
	if (fd < 0) {
		eprintf("can't accept a new connection, %m\n");
		return;
	}

	opt = 1;
	ret = setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &opt, sizeof(opt));
	if (ret) {
		close(fd);
		return;
	}

	ci = create_client(fd, data);
	if (!ci) {
		close(fd);
		return;
	}

	ret = register_event(fd, client_handler, ci);
	if (ret) {
		destroy_client(ci);
		return;
	}

	dprintf("accepted a new connection, %d\n", fd);
}

static int create_listen_port_fn(int fd, void *data)
{
	return register_event(fd, listen_handler, data);
}

int create_listen_port(int port, void *data)
{
	return create_listen_ports(port, create_listen_port_fn, data);
}

int write_object(struct sheepdog_node_list_entry *e,
		 int nodes, uint32_t node_version,
		 uint64_t oid, char *data, unsigned int datalen,
		 uint64_t offset, int nr, int create)
{
	struct sd_obj_req hdr;
	int i, n, fd, ret, success = 0;
	uint16_t vosts[3];
	char name[128];

	for (i = 0; i < nr; i++) {
		unsigned rlen = 0, wlen = datalen;

		n = obj_to_sheep(e, nodes, oid, i);

		addr_to_str(name, sizeof(name), e[n].addr, 0);

		fd = connect_to(name, e[n].port);
		if (fd < 0) {
			eprintf("can't connect to vost %u, %s\n", vosts[i], name);
			continue;
		}

		memset(&hdr, 0, sizeof(hdr));
		hdr.epoch = node_version;
		if (create)
			hdr.opcode = SD_OP_CREATE_AND_WRITE_OBJ;
		else
			hdr.opcode = SD_OP_WRITE_OBJ;

		hdr.oid = oid;
		hdr.copies = nr;

		hdr.flags = SD_FLAG_CMD_WRITE | SD_FLAG_CMD_DIRECT;
		hdr.data_length = wlen;
		hdr.offset = offset;

		ret = exec_req(fd, (struct sd_req *)&hdr, data, &wlen, &rlen);
		close(fd);
		if (ret)
			eprintf("can't update vost %u, %s\n", vosts[i], name);
		else
			success++;
	}

	return !success;
}

int read_object(struct sheepdog_node_list_entry *e,
		int nodes, uint32_t node_version,
		uint64_t oid, char *data, unsigned int datalen,
		uint64_t offset, int nr)
{
	struct sd_obj_req hdr;
	struct sd_obj_rsp *rsp = (struct sd_obj_rsp *)&hdr;
	char name[128];
	int i = 0, n, fd, ret;

	for (i = 0; i < nr; i++) {
		unsigned wlen = 0, rlen = datalen;

		n = obj_to_sheep(e, nodes, oid, i);

		addr_to_str(name, sizeof(name), e[n].addr, 0);

		fd = connect_to(name, e[n].port);
		if (fd < 0) {
			printf("%s(%d): %s, %m\n", __func__, __LINE__,
			       name);
			return -1;
		}

		memset(&hdr, 0, sizeof(hdr));
		hdr.epoch = node_version;
		hdr.opcode = SD_OP_READ_OBJ;
		hdr.oid = oid;

		hdr.flags =  SD_FLAG_CMD_DIRECT;
		hdr.data_length = rlen;
		hdr.offset = offset;

		ret = exec_req(fd, (struct sd_req *)&hdr, data, &wlen, &rlen);
		close(fd);

		if (!ret) {
			if (rsp->result == SD_RES_SUCCESS)
				return rsp->data_length;
		}
	}

	return -1;
}

int remove_object(struct sheepdog_node_list_entry *e,
		  int nodes, uint32_t node_version,
		  uint64_t oid, int nr)
{
	char name[128];
	struct sd_obj_req hdr;
	struct sd_obj_rsp *rsp = (struct sd_obj_rsp *)&hdr;
	int i = 0, n, fd, ret;

	if (nr > nodes)
		nr = nodes;

	for (i = 0; i < nr; i++) {
		unsigned wlen = 0, rlen = 0;

		n = obj_to_sheep(e, nodes, oid, i);

		addr_to_str(name, sizeof(name), e[n].addr, 0);

		fd = connect_to(name, e[n].port);
		if (fd < 0) {
			rsp->result = SD_RES_EIO;
			return -1;
		}

		memset(&hdr, 0, sizeof(hdr));
		hdr.epoch = node_version;
		hdr.opcode = SD_OP_REMOVE_OBJ;
		hdr.oid = oid;

		hdr.flags = 0;
		hdr.data_length = rlen;

		ret = exec_req(fd, (struct sd_req *)&hdr, NULL, &wlen, &rlen);
		close(fd);

		if (ret)
			return -1;
	}

	if (rsp->result != SD_RES_SUCCESS)
		return -1;

	return 0;
}