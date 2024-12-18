#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <arpa/inet.h>
#include <rdma/rdma_cma.h>
#include <openssl/sha.h>
#include <string.h>
#include <stdio.h>

// Function to compute hash for a memory region
void compute_mr_hash(struct ibv_mr *mr, unsigned char *output) {
    SHA256_CTX sha256;
    SHA256_Init(&sha256);
    SHA256_Update(&sha256, mr->addr, mr->length);
    SHA256_Final(output, &sha256);
}

// Function to print the hash
void print_hash(const unsigned char *hash, size_t length) {
    for (size_t i = 0; i < length; i++) {
        printf("%02x", hash[i]);
    }
    printf("\n");
}

// Function to compare memory regions
int print_mr_hashes(struct ibv_mr *mr1) {
    unsigned char hash1[SHA256_DIGEST_LENGTH];

    // Compute hashes for both memory regions
    compute_mr_hash(mr1, hash1);

    // Print the hashes for debugging
    printf("Hash of MR1: ");
    print_hash(hash1, SHA256_DIGEST_LENGTH);
}

void initialize_mr_random(struct ibv_mr *mr) {
    if (!mr || !mr->addr || mr->length == 0) {
        fprintf(stderr, "Invalid memory region.\n");
        return;
    }

    uint8_t *buffer = (uint8_t *)mr->addr;
    size_t length = mr->length;

    // Seed the random number generator
    srand(time(NULL));

    // Fill the memory region with random bytes
    for (size_t i = 0; i < length; i++) {
        buffer[i] = rand() % 256; // Random byte (0-255)
    }
}

int parse_arguments(int argc, char *argv[], int *dst_port, size_t *buffer_size, int *src_port, char **dst_ip,  char **src_ip, int *validate, int *fill_memory) {
    int opt;
    while ((opt = getopt(argc, argv, "d:s:p:r:i:vfh")) != -1) { // Added 'i' for IP address
        switch (opt) {
            case 'd': // Destination Port
                *dst_port = atoi(optarg);
                break;
            case 's': // Buffer Size
                *buffer_size = (size_t)atol(optarg);
                break;
            case 'p': // Source Port
                *src_port = atoi(optarg);
                break;
            case 'r': // Destination IP
                *dst_ip = optarg;
                break;
            case 'i': // Destination IP
                *src_ip = optarg;
                break;
            case 'v': // Feature Flag
                *validate = 1; // Enable feature if -f is present
                break;
            case 'f': // Feature Flag
                *fill_memory = 1; // Enable feature if -f is present
                break;
            case 'h':
                fprintf(stderr, "Usage: %s -d <dst-port> -s <buffer-size> -p <source-port> -i <src-ip> -r <remote-ip> -v <validate> -f <fill memory with random byte> -h <help>\n", argv[0]);
                return -1;
            default:
                fprintf(stderr, "Usage: %s -d <dst-port> -s <buffer-size> -p <source-port> -i <src-ip> -r <remote-ip> -v <validate> -f <fill memory with random byte> -h <help>\n", argv[0]);
                return -1;
        }
    }
    return 0;
}

int main(int argc, char *argv[]) {
    struct rdma_event_channel *ec = NULL;
    struct rdma_cm_id *id = NULL;
    struct rdma_cm_event *event = NULL;
    struct rdma_conn_param cm_params;
    struct ibv_pd *pd = NULL;
    struct ibv_mr *mr = NULL;
    struct ibv_comp_channel *comp_chan = NULL;
    struct ibv_cq *cq = NULL;
    struct ibv_qp_init_attr qp_attr;
    char *buffer;
    int ret;

    int dst_port = 23456;
    size_t buffer_size = 1024 * 1024 * 100;
    int src_port = 12345;
    char *dst_ip = "127.0.0.1"; // Default to localhost
    char *src_ip = "127.0.0.1"; // Default to localhost
    int validate = 0, fill_memory = 0;

    if (parse_arguments(argc, argv, &dst_port, &buffer_size, &src_port, &dst_ip, &src_ip, &validate, &fill_memory) < 0) {
        return 1; // Exit if parsing failed
    }

    // Bind the socket to the specific source port
    struct sockaddr_in src_addr;
    memset(&src_addr, 0, sizeof(src_addr));
    src_addr.sin_family = AF_INET;
    src_addr.sin_port = htons(src_port);  // Set the desired source port
    inet_pton(AF_INET, src_ip, &src_addr.sin_addr);


    ec = rdma_create_event_channel();
    if (!ec) {
        perror("rdma_create_event_channel failed");
        return 1;
    }

    ret = rdma_create_id(ec, &id, NULL, RDMA_PS_TCP);
    if (ret) {
        perror("rdma_create_id failed");
        return 1;
    }

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons(dst_port);
    inet_pton(AF_INET, dst_ip, &addr.sin_addr);

    ret = rdma_resolve_addr(id, (struct sockaddr *)&src_addr, (struct sockaddr *)&addr, 2000);
    if (ret) {
        perror("rdma_resolve_addr failed");
        return 1;
    }

    ret = rdma_get_cm_event(ec, &event);
    if (ret) {
        perror("rdma_get_cm_event failed");
        return 1;
    }

    if (event->event != RDMA_CM_EVENT_ADDR_RESOLVED) {
        fprintf(stderr, "Unexpected event: %d\n", event->event);
        return 1;
    }
    rdma_ack_cm_event(event);

    ret = rdma_resolve_route(id, 2000);
    if (ret) {
        perror("rdma_resolve_route failed");
        return 1;
    }

    ret = rdma_get_cm_event(ec, &event);
    if (ret) {
        perror("rdma_get_cm_event failed");
        return 1;
    }

    if (event->event != RDMA_CM_EVENT_ROUTE_RESOLVED) {
        fprintf(stderr, "Unexpected event: %d\n", event->event);
        return 1;
    }
    rdma_ack_cm_event(event);

    pd = ibv_alloc_pd(id->verbs);
    if (!pd) {
        perror("ibv_alloc_pd failed");
        return 1;
    }

    comp_chan = ibv_create_comp_channel(id->verbs);
    if (!comp_chan) {
        perror("ibv_create_comp_channel failed");
        return 1;
    }

    cq = ibv_create_cq(id->verbs, 10, NULL, comp_chan, 0);
    if (!cq) {
        perror("ibv_create_cq failed");
        return 1;
    }

    memset(&qp_attr, 0, sizeof(qp_attr));
    qp_attr.cap.max_send_wr = 10;
    qp_attr.cap.max_recv_wr = 10;
    qp_attr.cap.max_send_sge = 1;
    qp_attr.cap.max_recv_sge = 1;
    qp_attr.send_cq = cq;
    qp_attr.recv_cq = cq;
    qp_attr.qp_type = IBV_QPT_RC;

    ret = rdma_create_qp(id, pd, &qp_attr);
    if (ret) {
        perror("rdma_create_qp failed");
        return 1;
    }

    buffer = malloc(buffer_size);
    strcpy(buffer, "Hello from client");

    mr = ibv_reg_mr(pd, buffer, buffer_size, IBV_ACCESS_LOCAL_WRITE | IBV_ACCESS_REMOTE_WRITE);
    if (!mr) {
        perror("ibv_reg_mr failed");
        return 1;
    }
    if (fill_memory){
        initialize_mr_random(mr);
    }
    if (validate){
        print_mr_hashes(mr);
    }

    memset(&cm_params, 0, sizeof(cm_params));
    cm_params.initiator_depth = 1;
    cm_params.responder_resources = 1;
    cm_params.rnr_retry_count = 7;

    ret = rdma_connect(id, &cm_params);
    if (ret) {
        perror("rdma_connect failed");
        return 1;
    }

    ret = rdma_get_cm_event(ec, &event);
    if (ret) {
        perror("rdma_get_cm_event failed");
        return 1;
    }

    if (event->event != RDMA_CM_EVENT_ESTABLISHED) {
        fprintf(stderr, "Unexpected event: %d\n", event->event);
        return 1;
    }
    rdma_ack_cm_event(event);

    printf("Connected to server!\n");

    struct ibv_send_wr wr, *bad_wr = NULL;
    struct ibv_sge sge;

    memset(&wr, 0, sizeof(wr));
    wr.wr_id = (uintptr_t)buffer;
    wr.opcode = IBV_WR_SEND;
    wr.sg_list = &sge;
    wr.num_sge = 1;
    wr.send_flags = IBV_SEND_SIGNALED;

    sge.addr = (uintptr_t)buffer;
    sge.length = buffer_size;
    sge.lkey = mr->lkey;

    ret = ibv_post_send(id->qp, &wr, &bad_wr);
    if (ret) {
        perror("ibv_post_send failed");
        return 1;
    }

    struct ibv_wc wc;
    while ((ret = ibv_poll_cq(cq, 1, &wc)) == 0);

    if (ret < 0) {
        perror("ibv_poll_cq failed");
        return 1;
    }

    if (wc.status != IBV_WC_SUCCESS) {
        fprintf(stderr, "Failed status %s (%d) for wr_id %d\n", ibv_wc_status_str(wc.status), wc.status, (int) wc.wr_id);
        return 1;
    }

    printf("Message sent to server!\n");

    rdma_disconnect(id);
    rdma_destroy_qp(id);
    ibv_dereg_mr(mr);
    free(buffer);
    rdma_destroy_id(id);
    ibv_destroy_cq(cq);
    ibv_destroy_comp_channel(comp_chan);
    ibv_dealloc_pd(pd);
    rdma_destroy_event_channel(ec);

    return 0;
}
