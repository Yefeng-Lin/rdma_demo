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

struct ibv_send_wr w_rc;

pthread_t cq_poller_thread;

char* buf = NULL;
struct ibv_mr *mr;
struct ibv_mr *r_mr;

struct buf_info {
	int      id;
	uint64_t buf;
	uint32_t rkey;
	uint32_t size;
}b_info;

void die(const char *reason)
{
  fprintf(stderr, "%s\n", reason);
  exit(EXIT_FAILURE);
}

void on_completion(struct ibv_wc *wc)
{
    printf("On competion\n");
  if (wc->status != IBV_WC_SUCCESS)
    die("on_completion: status is not IBV_WC_SUCCESS.");

  if (wc->opcode & IBV_WC_RECV) {
    struct buf_info* b_info = (uintptr_t)wc->wr_id;
    struct ibv_send_wr *bad_wr;
    struct ibv_sge sge;
    printf("Received rkey %x addr %" PRIx64 " len %d from peer\n",
		  b_info->rkey, b_info->buf, b_info->size);
    sge.addr = buf;
    sge.length = 100;
    sge.lkey = mr->lkey;

    w_rc.send_flags = IBV_SEND_SIGNALED;
    w_rc.sg_list = &sge;
    w_rc.num_sge = 1;

    w_rc.opcode = IBV_WR_RDMA_READ;
    w_rc.wr.rdma.rkey = b_info->rkey;
    w_rc.wr.rdma.remote_addr = b_info->buf;
    w_rc.sg_list->length = b_info->size;
    w_rc.wr_id = 0;
    ibv_post_send(qp, &w_rc, &bad_wr);

  } else if (wc->opcode == IBV_WC_SEND) {
    printf("send completed successfully.\n");
  }
  if(wc->opcode & IBV_WC_RDMA_READ)
  {
    printf("%s\n",buf);
  }
}

void * poll_cq(void *ctx)
{
  struct ibv_cq *cq;
  struct ibv_wc wc;

  while (1) {
    ibv_get_cq_event(channel, &cq, &ctx);
    ibv_ack_cq_events(cq, 1);
    ibv_req_notify_cq(cq, 0);

    while (ibv_poll_cq(cq, 1, &wc))
      on_completion(&wc);
  }

  return NULL;
}

int on_connect_request(struct rdma_cm_id *cm_id)
{
    int ret = 0;
    pd = ibv_alloc_pd(cm_id->verbs);
    if(!pd)
    {
        fprintf(stderr, "ibv_alloc_pd failed\n");
        exit(-1);
    }
    channel = ibv_create_comp_channel(cm_id->verbs);
    if (!channel) {
		fprintf(stderr, "ibv_create_comp_channel failed\n");
		ret = errno;
		// TODO: use real labels. e.g. err_create_channel or free_pd
		exit(-1);
	}
    cq = ibv_create_cq(cm_id->verbs,CQ_DEPTH,NULL,channel,0);
    if (!cq) {
		fprintf(stderr, "ibv_create_cq failed\n");
		ret = errno;
		exit(-1);
	}
    ret = ibv_req_notify_cq(cq,0);

    pthread_create(&cq_poller_thread, NULL, poll_cq, NULL);

    struct ibv_qp_init_attr init_attr;
    memset(&init_attr, 0, sizeof(init_attr));
	init_attr.cap.max_send_wr = MAX_WR;
	init_attr.cap.max_recv_wr = MAX_WR;
	init_attr.cap.max_recv_sge = 1;
	init_attr.cap.max_send_sge = 1;
	init_attr.qp_type = IBV_QPT_RC;
	init_attr.send_cq = cq;
	init_attr.recv_cq = cq;
    ret = rdma_create_qp(cm_id,pd,&init_attr);
    qp = cm_id->qp;
    //printf("QP;%d\n",ret);
    mr = ibv_reg_mr(pd,buf,100,IBV_ACCESS_LOCAL_WRITE | IBV_ACCESS_REMOTE_WRITE | IBV_ACCESS_REMOTE_READ);
    r_mr = ibv_reg_mr(pd,&b_info,sizeof(struct buf_info),IBV_ACCESS_LOCAL_WRITE | IBV_ACCESS_REMOTE_WRITE | IBV_ACCESS_REMOTE_READ);

    struct ibv_sge sge;
    struct ibv_recv_wr wr, *bad_wr = NULL;
    sge.addr = (uintptr_t)&b_info;
    sge.length = sizeof(struct buf_info);
    sge.lkey = r_mr->lkey;

    wr.wr_id = (uintptr_t)&b_info;
    wr.next = NULL;
    wr.sg_list = &sge;
    wr.num_sge = 1;
    //fprintf(stderr, "mr FAILED!!\n");
    ibv_post_recv(qp,&wr,&bad_wr);
    //fprintf(stderr, "mr FAILED!!\n");
    ret = rdma_accept(cm_id,NULL);
    return ret;
}

int on_disconnect(struct rdma_cm_id *cm_id)
{
    rdma_destroy_qp(cm_id);
    rdma_destroy_id(cm_id);
    return 0;
}

int handler_cm_event(struct rdma_cm_event *event)
{
    int ret = 0;
    if(event->event == RDMA_CM_EVENT_CONNECT_REQUEST)
    {
        ret = on_connect_request(event->id);
    }
    else if(event->event == RDMA_CM_EVENT_ESTABLISHED)
    {
        ret = 0;
    }
    else if (event->event == RDMA_CM_EVENT_DISCONNECTED)
    {
        ret = on_disconnect(event->id);
    }
    return ret;
}

int main(void)
{
    struct rdma_event_channel *rec = NULL;
    struct rdma_cm_id* cid = NULL;
    struct sockaddr_in s_addr;
    struct rdma_cm_event *event = NULL;
    buf = malloc(0);

    rec = rdma_create_event_channel();
    rdma_create_id(rec, &cid, NULL, RDMA_PS_TCP);
    memset(&s_addr, 0, sizeof(s_addr));
    s_addr.sin_family = AF_INET;
    s_addr.sin_port = htons(22222);
    rdma_bind_addr(cid,(struct sockaddr*)&s_addr);
    rdma_listen(cid,10);

    while(rdma_get_cm_event(rec,&event) == 0) {
        struct rdma_cm_event event_copy;

        memcpy(&event_copy,event,sizeof(*event));
        rdma_ack_cm_event(event);
        if (handler_cm_event(&event_copy))
            break;
    }
    return 0;
}