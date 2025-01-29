#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <arpa/inet.h>
#include <rdma/rdma_cma.h>
#include <openssl/sha.h>
#include <string.h>
#include <stdio.h>

// Arguments structure for threads
typedef struct {
    int dst_port;
    size_t buffer_size;
    char *server_ip;
    int validate;
} qp_thread_args;

void *create_qp_thread(void *args) {
    qp_thread_args *qp_args = (qp_thread_args *)args;
    int ret = create_qp(qp_args->dst_port, qp_args->buffer_size, qp_args->server_ip, qp_args->validate);
    if (ret != 0) {
        fprintf(stderr, "create_qp failed for port %d\n", qp_args->dst_port);
    }
    return NULL;
}

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

int parse_arguments(int argc, char *argv[], int *dst_port, int **dst_ports, size_t *buffer_size, char **dst_ip, int *validate, int *number_of_qp) {
    int opt;
    int single_port = -1; // Store a single port if -d is used
    char *port_list = NULL;

    while ((opt = getopt(argc, argv, "p:P:s:i:q:h:v")) != -1) { 
        switch (opt) {
            case 'p': // Single Destination Port
                if (port_list) {
                    fprintf(stderr, "Error: Cannot use both -p and -P options.\n");
                    return -1;
                }
                single_port = atoi(optarg);
                break;

            case 'P': // List of Destination Ports
                if (single_port != -1) {
                    fprintf(stderr, "Error: Cannot use both -p and -P options.\n");
                    return -1;
                }
                port_list = optarg; // Store the raw string for later processing
                break;

            case 's': // Buffer Size
                *buffer_size = (size_t)atol(optarg);
                break;

            case 'i': // Destination IP
                *dst_ip = optarg;
                break;

            case 'q': // Number of Queue Pairs
                *number_of_qp = atoi(optarg);
                break;

            case 'v': // Enable validation
                *validate = 1;
                break;

            case 'h': // Help message
            default:
                fprintf(stderr, "Usage: %s -d <dst-port> | -D <port1,port2,...> -s <buffer-size> -i <dst-ip> -q <number of QPs> -v -h\n", argv[0]);
                return -1;
        }
    }

    // Process the ports
    if (single_port != -1) {
        // Use single port mode
        *dst_port = single_port;
        *dst_ports = NULL; // Ensure list pointer is NULL
    } else if (port_list) {
        // Parse comma-separated list of ports
        char *token;
        int count = 0;
        int *ports = malloc((*number_of_qp) * sizeof(int));
        if (!ports) {
            perror("malloc failed");
            return -1;
        }

        token = strtok(port_list, ",");
        while (token != NULL) {
            if (count >= *number_of_qp) {
                fprintf(stderr, "Error: Number of ports in -D does not match number_of_qp.\n");
                free(ports);
                return -1;
            }
            ports[count++] = atoi(token);
            token = strtok(NULL, ",");
        }

        if (count != *number_of_qp) {
            fprintf(stderr, "Error: Expected %d ports, but got %d.\n", *number_of_qp, count);
            free(ports);
            return -1;
        }

        *dst_ports = ports; // Assign parsed ports array
        *dst_port = -1; // Indicate that -d was not used
    } else {
        fprintf(stderr, "Error: Either -d or -D must be specified.\n");
        return -1;
    }

    return 0;
}

int create_qp(int dst_port, size_t buffer_size, char *server_ip, int validate){
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
    mr = ibv_reg_mr(pd, buffer, buffer_size, IBV_ACCESS_LOCAL_WRITE |
                                IBV_ACCESS_REMOTE_WRITE |
                                IBV_ACCESS_REMOTE_READ);
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
    while (1){
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
    }
    // to complete
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

int main(int argc, char *argv[]) {
    size_t buffer_size = 1024 * 1024 * 100; // Default buffer size
    char *server_ip = "127.0.0.1";        // Default to localhost
    int dst_port = 0;                   // Base port
    int* dst_ports_list;
    int validate = -1;                       // Default to feature disabled
    int number_of_qp = 3;
    int ret;

    // Parse arguments
    if (parse_arguments(argc, argv, &dst_port, &dst_ports_list, &buffer_size, &server_ip, &validate, &number_of_qp) < 0) {
        return 1;
    }

    pthread_t threads[number_of_qp];
    qp_thread_args args[number_of_qp];

    // Create threads for each call to create_qp
    for (int i = 0; i < number_of_qp; i++) {
        if (dst_port != -1){
            args[i].dst_port = dst_port + i;
        } else {
            args[i].dst_port = dst_ports_list[i];
        }
        args[i].buffer_size = buffer_size;
        args[i].server_ip = server_ip;
        args[i].validate = validate;

        ret = pthread_create(&threads[i], NULL, create_qp_thread, &args[i]);
        if (ret != 0) {
            fprintf(stderr, "Failed to create thread for port %d\n", args[i].dst_port);
            return 1;
        }
        fprintf(stdout, "QP is listening on port %d\n", args[i].dst_port);
    }

    // Wait for all threads to finish
    for (int i = 0; i < 3; i++) {
        pthread_join(threads[i], NULL);
    }

    printf("All QPs created successfully.\n");
    return 0;
}
