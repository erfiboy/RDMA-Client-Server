#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <arpa/inet.h>
#include <rdma/rdma_cma.h>
#include <openssl/sha.h>
#include <string.h>
#include <stdio.h>

struct flow {
    int dst_port;
    size_t buffer_size;
    int src_port;
    char *dst_ip;
    char *src_ip;
    struct timespec timestamp; 
};

struct queue_pair{
    struct rdma_event_channel *ec;
    struct rdma_cm_id *id;
    struct rdma_cm_event *event;
    struct rdma_conn_param cm_params;
    struct ibv_pd *pd;
    struct ibv_mr *mr;
    struct ibv_comp_channel *comp_chan;
    struct ibv_cq *cq;
    struct ibv_qp_init_attr qp_attr;
    char *buffer;
    char *probe;
};

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

int parse_arguments(int argc, char *argv[], int *dst_port, size_t *buffer_size, int *src_port, char **dst_ip,  char **src_ip, char **filename, int *validate, int *fill_memory) {
    int opt;
    while ((opt = getopt(argc, argv, "F:d:s:p:r:i:vfh")) != -1) { // Added 'i' for IP address
        switch (opt) {
            case 'F':
                *filename = optarg;
                return 1;
                break;
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
                fprintf(stderr, "Usage: %s -F <config file> -d <dst-port> -s <buffer-size> -p <source-port> -i <src-ip> -r <remote-ip> -v <validate> -f <fill memory with random byte> -h <help>\n", argv[0]);
                return -1;
            default:
                fprintf(stderr, "Usage: %s -F <config file> -d <dst-port> -s <buffer-size> -p <source-port> -i <src-ip> -r <remote-ip> -v <validate> -f <fill memory with random byte> -h <help>\n", argv[0]);
                return -1;
        }
    }
    return 0;
}

int parse_config(const char *filename, int *flow_count, struct flow **flows) {
    FILE *file = fopen(filename, "r");
    if (file == NULL) {
        perror("Could not open file");
        return -1;  // Return error code if file cannot be opened
    }

    // Read the number of flows from the first line
    fscanf(file, "%d", flow_count);

    // Allocate memory for the array of flows
    *flows = malloc(sizeof(struct flow) * (*flow_count));
    if (*flows == NULL) {
        perror("Memory allocation failed");
        fclose(file);
        return -1;  // Return error code if memory allocation fails
    }

    // Read the flow information
    for (int i = 0; i < *flow_count; i++) {
        (*flows)[i].dst_ip = malloc(16 * sizeof(char));  // Allocate space for IP address
        (*flows)[i].src_ip = malloc(16 * sizeof(char));  // Allocate space for IP address
        
        // Read the details for each flow
        fscanf(file, "%s %s %d %d %zu %ld", 
                (*flows)[i].dst_ip, (*flows)[i].src_ip, 
                &(*flows)[i].src_port, &(*flows)[i].dst_port, 
                &(*flows)[i].buffer_size, &(*flows)[i].timestamp.tv_sec);

        // Convert the timestamp to timespec structure
        (*flows)[i].timestamp.tv_nsec = (long)(((*flows)[i].timestamp.tv_sec - (long)(*flows)[i].timestamp.tv_sec) * 1000000000);
    }

    fclose(file);
    return 0;  // Return success code
}

int connect_to_a_server(int dst_port, size_t buffer_size, int src_port, char *dst_ip, char *src_ip, int validate, int fill_memory, struct queue_pair *qp_info) {
    int ret;

    // Bind the socket to the specific source port
    struct sockaddr_in src_addr;
    memset(&src_addr, 0, sizeof(src_addr));
    src_addr.sin_family = AF_INET;
    src_addr.sin_port = htons(src_port);  // Set the desired source port
    inet_pton(AF_INET, src_ip, &src_addr.sin_addr);

    qp_info->ec = rdma_create_event_channel();
    if (!qp_info->ec) {
        perror("rdma_create_event_channel failed");
        return 1;
    }

    ret = rdma_create_id(qp_info->ec, &qp_info->id, NULL, RDMA_PS_TCP);
    if (ret) {
        perror("rdma_create_id failed");
        return 1;
    }

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons(dst_port);
    inet_pton(AF_INET, dst_ip, &addr.sin_addr);

    ret = rdma_resolve_addr(qp_info->id, (struct sockaddr *)&src_addr, (struct sockaddr *)&addr, 2000);
    if (ret) {
        perror("rdma_resolve_addr failed");
        return 1;
    }

    ret = rdma_get_cm_event(qp_info->ec, &qp_info->event);
    if (ret) {
        perror("rdma_get_cm_event failed");
        return 1;
    }

    if (qp_info->event->event != RDMA_CM_EVENT_ADDR_RESOLVED) {
        fprintf(stderr, "Unexpected event: %d\n", qp_info->event->event);
        return 1;
    }
    rdma_ack_cm_event(qp_info->event);

    ret = rdma_resolve_route(qp_info->id, 2000);
    if (ret) {
        perror("rdma_resolve_route failed");
        return 1;
    }

    ret = rdma_get_cm_event(qp_info->ec, &qp_info->event);
    if (ret) {
        perror("rdma_get_cm_event failed");
        return 1;
    }

    if (qp_info->event->event != RDMA_CM_EVENT_ROUTE_RESOLVED) {
        fprintf(stderr, "Unexpected event: %d\n", qp_info->event->event);
        return 1;
    }
    rdma_ack_cm_event(qp_info->event);

    qp_info->pd = ibv_alloc_pd(qp_info->id->verbs);
    if (!qp_info->pd) {
        perror("ibv_alloc_pd failed");
        return 1;
    }

    qp_info->comp_chan = ibv_create_comp_channel(qp_info->id->verbs);
    if (!qp_info->comp_chan) {
        perror("ibv_create_comp_channel failed");
        return 1;
    }

    qp_info->cq = ibv_create_cq(qp_info->id->verbs, 10, NULL, qp_info->comp_chan, 0);
    if (!qp_info->cq) {
        perror("ibv_create_cq failed");
        return 1;
    }

    memset(&qp_info->qp_attr, 0, sizeof(qp_info->qp_attr));
    qp_info->qp_attr.cap.max_send_wr = 10;
    qp_info->qp_attr.cap.max_recv_wr = 10;
    qp_info->qp_attr.cap.max_send_sge = 1;
    qp_info->qp_attr.cap.max_recv_sge = 1;
    qp_info->qp_attr.send_cq = qp_info->cq;
    qp_info->qp_attr.recv_cq = qp_info->cq;
    qp_info->qp_attr.qp_type = IBV_QPT_RC;

    ret = rdma_create_qp(qp_info->id, qp_info->pd, &qp_info->qp_attr);
    if (ret) {
        perror("rdma_create_qp failed");
        return 1;
    }

    qp_info->buffer = malloc(buffer_size);
    strcpy(qp_info->buffer, "Hello from client");

    qp_info->mr = ibv_reg_mr(qp_info->pd, qp_info->buffer, buffer_size, IBV_ACCESS_LOCAL_WRITE | IBV_ACCESS_REMOTE_WRITE);
    if (!qp_info->mr) {
        perror("ibv_reg_mr failed");
        return 1;
    }

    if (fill_memory) {
        initialize_mr_random(qp_info->mr);
    }
    if (validate) {
        print_mr_hashes(qp_info->mr);
    }

    memset(&qp_info->cm_params, 0, sizeof(qp_info->cm_params));
    qp_info->cm_params.initiator_depth = 1;
    qp_info->cm_params.responder_resources = 1;
    qp_info->cm_params.rnr_retry_count = 7;

    ret = rdma_connect(qp_info->id, &qp_info->cm_params);
    if (ret) {
        perror("rdma_connect failed");
        return 1;
    }

    ret = rdma_get_cm_event(qp_info->ec, &qp_info->event);
    if (ret) {
        perror("rdma_get_cm_event failed");
        return 1;
    }

    if (qp_info->event->event != RDMA_CM_EVENT_ESTABLISHED) {
        fprintf(stderr, "Unexpected event: %d\n", qp_info->event->event);
        return 1;
    }
    rdma_ack_cm_event(qp_info->event);

    printf("Connected to server!\n");

    struct ibv_send_wr wr, *bad_wr = NULL;
    struct ibv_sge sge;

    memset(&wr, 0, sizeof(wr));
    wr.wr_id = (uintptr_t)qp_info->buffer;
    wr.opcode = IBV_WR_SEND;
    wr.sg_list = &sge;
    wr.num_sge = 1;
    wr.send_flags = IBV_SEND_SIGNALED;

    sge.addr = (uintptr_t)qp_info->buffer;
    sge.length = buffer_size;
    sge.lkey = qp_info->mr->lkey;

    while (1) {
        char c = getchar();  // Waits for user input
        if (c != '\n')  // Ignores Enter key presses
            break;

        struct timespec start, end;

        // Record the start time
        clock_gettime(CLOCK_MONOTONIC, &start);
        ret = ibv_post_send(qp_info->id->qp, &wr, &bad_wr);
        if (ret) {
            perror("ibv_post_send failed");
            return 1;
        }

        struct ibv_wc wc;
        while ((ret = ibv_poll_cq(qp_info->cq, 1, &wc)) == 0);

        if (ret < 0) {
            perror("ibv_poll_cq failed");
            return 1;
        }

        if (wc.status != IBV_WC_SUCCESS) {
            fprintf(stderr, "Failed status %s (%d) for wr_id %d\n", ibv_wc_status_str(wc.status), wc.status, (int)wc.wr_id);
            return 1;
        }

        // Record the end time
        clock_gettime(CLOCK_MONOTONIC, &end);

        // Calculate elapsed time in nanoseconds
        long seconds = end.tv_sec - start.tv_sec;
        long nanoseconds = end.tv_nsec - start.tv_nsec;
        long elapsed_ns = seconds * 1e9 + nanoseconds;  // Total time in nanoseconds

        // Convert to milliseconds
        double elapsed_ms = (double)elapsed_ns / 1e6;

        // Print the elapsed time in nanoseconds and milliseconds
        printf("Elapsed time: %ld nanoseconds (%.3f milliseconds)\n", elapsed_ns, elapsed_ms);
    }

    printf("Message sent to server!\n");

    rdma_disconnect(qp_info->id);
    rdma_destroy_qp(qp_info->id);
    ibv_dereg_mr(qp_info->mr);
    free(qp_info->buffer);
    rdma_destroy_id(qp_info->id);
    ibv_destroy_cq(qp_info->cq);
    ibv_destroy_comp_channel(qp_info->comp_chan);
    ibv_dealloc_pd(qp_info->pd);
    rdma_destroy_event_channel(qp_info->ec);

    return 0;
}


int main(int argc, char *argv[]) {
    int dst_port = 23456;
    size_t buffer_size = 1024 * 1024 * 100;
    int src_port = 12345;
    char *dst_ip = "127.0.0.1"; // Default to localhost
    char *src_ip = "127.0.0.1"; // Default to localhost
    char *flow_file;
    int validate = 0, fill_memory = 0;
    int flow_count = 0;
    struct flow *flows = NULL;
    struct queue_pair qp_info = {0};
    int result = parse_arguments(argc, argv, &dst_port, &buffer_size, &src_port, &dst_ip, &src_ip, &flow_file, &validate, &fill_memory);
    if (result < 0) {
        return 1; // Exit if parsing failed
    } else if (result == 1) {
        if (parse_config("config.txt", &flow_count, &flows) != 0) {
            return 1;  // Return error if parsing failed
        }
        dst_port = flows[0].dst_port;
        buffer_size = flows[0].buffer_size;
        src_port = flows[0].src_port;
        dst_ip = flows[0].dst_ip;
        src_ip = flows[0].src_ip;

        fprintf(stdout ,"Connecting to server...\n");
        fprintf(stdout ,"Destination IP: %s\n", dst_ip);
        fprintf(stdout ,"Source IP: %s\n", src_ip);
        fprintf(stdout ,"Source Port: %d\n", src_port);
        fprintf(stdout ,"Destination Port: %d\n", dst_port);
        fprintf(stdout ,"Buffer Size: %zu\n", buffer_size);
    }

    connect_to_a_server(dst_port, buffer_size, src_port, dst_ip, src_ip, validate, fill_memory, &qp_info);

    if (flow_count > 0){
        for (int i = 0; i < flow_count; i++) {
            free(flows[i].dst_ip);
            free(flows[i].src_ip);
        }
        free(flows);
    }

    return 0;
}
