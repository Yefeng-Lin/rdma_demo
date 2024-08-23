#include <getopt.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <errno.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <netdb.h>
#include <byteswap.h>
#include <semaphore.h>
#include <arpa/inet.h>
#include <pthread.h>
#include <inttypes.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <sys/time.h>
#include <dirent.h>
#include <libgen.h>
#include <linux/limits.h>
#include <unistd.h>
#include <rdma/rdma_cma.h>
#include <infiniband/arch.h>

#define MAX_WR 10
#define CQ_DEPTH 10

struct ibv_cq *cq;
struct ibv_pd *pd;
struct ibv_qp *qp;
struct ibv_comp_channel *channel;

char* buf;
struct ibv_mr *mr;
struct ibv_mr *s_mr;

struct buf_info {
	int      id;
	uint64_t buf;
	uint32_t rkey;
	uint32_t size;
}b_info;

int on_addr_resolved(struct rdma_cm_id *id)
{
    printf("addr resolved.\n");
    pd = ibv_alloc_pd(id->verbs);
    channel = ibv_create_comp_channel(id->verbs);
    cq = ibv_create_cq(id->verbs,CQ_DEPTH,NULL,channel,0);
    ibv_req_notify_cq(cq,0);

    struct ibv_qp_init_attr init_attr;
    memset(&init_attr, 0, sizeof(init_attr));
	init_attr.cap.max_send_wr = MAX_WR;
	init_attr.cap.max_recv_wr = MAX_WR;
	init_attr.cap.max_recv_sge = 1;
	init_attr.cap.max_send_sge = 1;
	init_attr.qp_type = IBV_QPT_RC;
	init_attr.send_cq = cq;
	init_attr.recv_cq = cq;

    rdma_create_qp(id,pd,&init_attr);
    qp = id->qp;
    rdma_resolve_route(id,500);

    return 0;
}

int on_route_resolved(struct rdma_cm_id *id)
{
  struct rdma_conn_param cm_params;

  printf("route resolved.\n");
  rdma_connect(id, NULL);

  return 0;
}

int on_connection(void *context)
{

    printf("connected. posting send...\n");

    //char* tmp = "Hello World RDMA!!";

    struct ibv_send_wr wr, *bad_wr = NULL;
    struct ibv_sge sge;
    //printf("%s\n",tmp);
    memset(&wr, 0, sizeof(wr));
    mr = ibv_reg_mr(pd,buf,100,IBV_ACCESS_LOCAL_WRITE | IBV_ACCESS_REMOTE_WRITE |IBV_ACCESS_REMOTE_READ);
    s_mr = ibv_reg_mr(pd,&b_info,sizeof(struct buf_info),IBV_ACCESS_LOCAL_WRITE | IBV_ACCESS_REMOTE_WRITE |IBV_ACCESS_REMOTE_READ);
    b_info.buf = buf;
    b_info.rkey = mr->rkey;
    b_info.size = 100;
    sge.addr = (uintptr_t)&b_info;
    sge.length = sizeof(struct buf_info);
    sge.lkey = s_mr->lkey;

    //wr.wr_id = (uintptr_t)&b_info;
    wr.opcode = IBV_WR_SEND;
    wr.sg_list = &sge;
    wr.num_sge = 1;
    wr.send_flags = IBV_SEND_SIGNALED;

    //printf("%s\n",tmp);
    ibv_post_send(qp, &wr, &bad_wr);

    return 0;
}

void die(const char *reason)
{
  fprintf(stderr, "%s\n", reason);
  exit(EXIT_FAILURE);
}

int on_event(struct rdma_cm_event *event)
{
  int r = 0;

  if (event->event == RDMA_CM_EVENT_ADDR_RESOLVED)
    r = on_addr_resolved(event->id);
  else if (event->event == RDMA_CM_EVENT_ROUTE_RESOLVED)
    r = on_route_resolved(event->id);
  else if (event->event == RDMA_CM_EVENT_ESTABLISHED)
    r = on_connection(event->id->context);
  else if (event->event == RDMA_CM_EVENT_DISCONNECTED)
    r = on_disconnect(event->id);
  else
    die("on_event: unknown event.");

  return r;
}

int on_disconnect(struct rdma_cm_id *id)
{
  printf("disconnected.\n");

  rdma_destroy_qp(id);

  rdma_destroy_id(id);

  return 1; /* exit event loop */
}

int main(int argc, char **argv)
{
    struct addrinfo *c_addr;
    struct rdma_event_channel *rec = NULL;
    struct rdma_cm_id *cid= NULL;
    struct rdma_cm_event *event = NULL;

    buf = malloc(100);

    getaddrinfo(argv[1],argv[2],NULL,&c_addr);
    memcpy(buf,argv[3],strlen(argv[3])+1);
    rec = rdma_create_event_channel();
    rdma_create_id(rec,&cid,NULL,RDMA_PS_TCP);
    rdma_resolve_addr(cid,NULL,c_addr->ai_addr,500);

    freeaddrinfo(c_addr);
    while(rdma_get_cm_event(rec,&event) == 0)
    {
        struct rdma_cm_event event_copy;

        memcpy(&event_copy, event, sizeof(*event));
        rdma_ack_cm_event(event);

        if (on_event(&event_copy))
            break;
    }
}