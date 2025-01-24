#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <arpa/inet.h>
#include <rdma/rdma_cma.h>
#include <openssl/sha.h>

// Function declarations
void compute_mr_hash(struct ibv_mr *mr, unsigned char *output);
void print_hash(const unsigned char *hash, size_t length);
int print_mr_hashes(struct ibv_mr *mr1);
int parse_arguments(int argc, char *argv[], int *dst_port, size_t *buffer_size, char **dst_ip, int *validate);

// Refactored RDMA server logic function
int perform_rdma_server(int dst_port, size_t buffer_size, char *server_ip, int validate) {
    struct rdma_event_channel *ec = NULL;
    struct rdma_cm_id *listener = NULL, *id = NULL;
    struct rdma_cm_event *event = NULL;
    struct rdma_conn_param cm_params;
    struct ibv_pd *pd = NULL;
    struct ibv_mr *mr = NULL;
    struct ibv_comp_channel *comp_chan = NULL;
    struct ibv_cq *cq = NULL;
    struct ibv_qp_init_attr qp_attr;
    char *buffer;
    int ret;

    ec = rdma_create_event_channel();
    if (!ec) {
        perror("rdma_create_event_channel failed");
        return 1;
    }

    ret = rdma_create_id(ec, &listener, NULL, RDMA_PS_TCP);
    if (ret) {
        perror("rdma_create_id failed");
        return 1;
    }

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons(dst_port);
    inet_pton(AF_INET, server_ip, &addr.sin_addr);

    ret = rdma_bind_addr(listener, (struct sockaddr *)&addr);
    if (ret) {
        perror("rdma_bind_addr failed");
        return 1;
    }

    ret = rdma_listen(listener, 1);
    if (ret) {
        perror("rdma_listen failed");
        return 1;
    }

    printf("Server listening on port %d...\n", dst_port);

    ret = rdma_get_cm_event(ec, &event);
    if (ret) {
        perror("rdma_get_cm_event failed");
        return 1;
    }

    if (event->event != RDMA_CM_EVENT_CONNECT_REQUEST) {
        fprintf(stderr, "Unexpected event: %d\n", event->event);
        return 1;
    }

    id = event->id;
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
    if (!buffer) {
        perror("malloc failed");
        return 1;
    }

    mr = ibv_reg_mr(pd, buffer, buffer_size, IBV_ACCESS_LOCAL_WRITE |
                    IBV_ACCESS_REMOTE_WRITE | IBV_ACCESS_REMOTE_READ);
    if (!mr) {
        perror("ibv_reg_mr failed");
        return 1;
    }

    memset(&cm_params, 0, sizeof(cm_params));
    cm_params.initiator_depth = 1;
    cm_params.responder_resources = 1;
    cm_params.rnr_retry_count = 7;

    ret = rdma_accept(id, &cm_params);
    if (ret) {
        perror("rdma_accept failed");
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

    printf("Client connected!\n");

    // Post a receive request
    struct ibv_sge sge;
    memset(&sge, 0, sizeof(sge));
    sge.addr = (uintptr_t)buffer;
    sge.length = buffer_size;
    sge.lkey = mr->lkey;

    struct ibv_recv_wr recv_wr, *bad_recv_wr = NULL;
    memset(&recv_wr, 0, sizeof(recv_wr));
    recv_wr.wr_id = (uintptr_t)buffer;
    recv_wr.sg_list = &sge;
    recv_wr.num_sge = 1;

    ret = ibv_post_recv(id->qp, &recv_wr, &bad_recv_wr);
    if (ret) {
        fprintf(stderr, "ibv_post_recv failed: %d\n", ret);
        return 1;
    } else {
        printf("Posted receive request successfully\n");
    }

    ret = ibv_req_notify_cq(cq, 0);
    if (ret) {
        perror("ibv_req_notify_cq failed");
        return 1;
    }

    // Wait for completion
    struct ibv_wc wc;
    printf("Waiting for completion...\n");
    while (1) {
        ret = ibv_poll_cq(cq, 1, &wc);
        if (ret < 0) {
            perror("ibv_poll_cq failed");
            break;
        } else if (ret == 0) {
            continue;  // No completion, keep polling
        }

        if (wc.status != IBV_WC_SUCCESS) {
            fprintf(stderr, "Completion with error, status: %s (%d), wr_id: %lu\n",
                    ibv_wc_status_str(wc.status), wc.status, wc.wr_id);
            break;
        }

        printf("Completion event received, wr_id: %lu, byte_len: %u\n",
               wc.wr_id, wc.byte_len);
        break;  // Exit after processing one successful completion
    }

    if (validate) {
        print_mr_hashes(mr);
    }

    // Cleanup
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

// Main function with the loop
int main(int argc, char *argv[]) {
    int dst_port = 23456;
    size_t buffer_size = 1024 * 1024 * 100;
    char *server_ip = "127.0.0.1"; // Default to localhost
    int validate = 0;             // Feature disabled by default
    int port_increment = 10;      // Number of ports to test

    if (parse_arguments(argc, argv, &dst_port, &buffer_size, &server_ip, &validate) < 0) {
        return 1;
    }

    for (int i = 0; i < port_increment; i++) {
        int current_port = dst_port + i;
        printf("Starting server on port %d...\n", current_port);
        int ret = perform_rdma_server(current_port, buffer_size, server_ip, validate);
        if (ret) {
            fprintf(stderr, "Server operation failed on port %d\n", current_port);
        }
    }

    return 0;
}


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

int parse_arguments(int argc, char *argv[], int *dst_port, size_t *buffer_size, char **dst_ip,  int *validate) {
    int opt;
    while ((opt = getopt(argc, argv, "p:s:i:h:v")) != -1) { // Added 'i' for IP address
        switch (opt) {
            case 'p': // Destination Port
                *dst_port = atoi(optarg);
                break;
            case 's': // Buffer Size
                *buffer_size = (size_t)atol(optarg);
                break;
            case 'i': // Destination IP
                *dst_ip = optarg;
                break;
            case 'h':
                fprintf(stderr, "Usage: %s -p <dst-port> -s <buffer-size> -i <dst-ip> -v <validate> -h <help>\n", argv[0]);
                break;
            case 'v': // Feature Flag
                *validate = 1; // Enable feature if -f is present
                break;
            default:
                fprintf(stderr, "Usage: %s -p <dst-port> -s <buffer-size> -i <dst-ip> -v <validate> -h <help>\n", argv[0]);
                return -1;
        }
    }
    return 0;
}
