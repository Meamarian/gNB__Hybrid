/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright(c) 2010-2016 Intel Corporation
 */
#include <stdio.h>
#include <stdint.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <inttypes.h>
#include <sys/types.h>
#include <sys/queue.h>
#include <netinet/in.h>
#include <setjmp.h>
#include <stdarg.h>
#include <ctype.h>
#include <errno.h>
#include <getopt.h>
#include <signal.h>
#include <stdbool.h>
#include <rte_common.h>
#include <rte_log.h>
#include <rte_malloc.h>
#include <rte_memory.h>
#include <rte_memcpy.h>
#include <rte_eal.h>
#include <rte_launch.h>
#include <rte_atomic.h>
#include <rte_cycles.h>
#include <rte_prefetch.h>
#include <rte_lcore.h>
#include <rte_per_lcore.h>
#include <rte_branch_prediction.h>
#include <rte_interrupts.h>
#include <rte_random.h>
#include <rte_debug.h>
#include <rte_ether.h>
#include <rte_ethdev.h>
#include <rte_mempool.h>
#include <rte_mbuf.h>
#include <rte_hash.h>
#include <rte_jhash.h>
#include <unistd.h>

//########################################################################################
//#define DEBUG_BUILD 1
#ifdef DEBUG_BUILD
#define DEBUG(...) fprintf(stderr, __VA_ARGS__)
#else
#define DEBUG(...) \
    do {           \
    } while(0)
#endif

#ifndef BAAS_UDP_PORT
# define BAAS_UDP_PORT 12345
#endif

#ifndef BAAS_RSS_HF
# define BAAS_RSS_HF ETH_RSS_IPV4 | ETH_RSS_NONFRAG_IPV4_TCP | ETH_RSS_NONFRAG_IPV4_UDP | ETH_RSS_IPV6 | ETH_RSS_NONFRAG_IPV6_TCP | ETH_RSS_NONFRAG_IPV6_UDP | ETH_RSS_IPV6_EX | ETH_RSS_IPV6_TCP_EX | ETH_RSS_IPV6_UDP_EX
#endif

#ifndef BAAS_RX_OFFLOADS
# define BAAS_RX_OFFLOADS DEV_RX_OFFLOAD_CHECKSUM
#endif

#ifndef BAAS_TIMER_RESOLUTION
# define BAAS_TIMER_RESOLUTION 10000.0
#endif

static struct rte_hash_parameters ut_params = {
    .name = "BufferTable",
    .entries = 1024*256,
    .key_len = sizeof(uint64_t),
    .hash_func = rte_jhash,
    .extra_flag=RTE_HASH_EXTRA_FLAGS_EXT_TABLE,
    //.extra_flag=RTE_HASH_EXTRA_FLAGS_RW_CONCURRENCY,
    .socket_id = 0,
};

static struct rte_hash_parameters ut_params_teid = {
    .name = "BufferTableTeid",
    .entries = 1024*1,
    .key_len = sizeof(uint32_t),
    .hash_func = rte_jhash,
    .hash_func_init_val = 0,
    .socket_id = 0,
};


typedef struct rte_hash lookup_struct_t;
static lookup_struct_t *buffer_table;
static lookup_struct_t *buffer_table_teid;

//#define PKTSIZE 1152
#define USE_DIFFERENT_POOL_FOR_LCORES 0

static uint8_t mempool_num = 0;
static uint8_t resubmission_time = 10; //MS

struct packet_in_buffer_t {
    struct rte_mbuf * pkt;
    uint8_t portid;
    uint64_t pktid;
};

struct bucket_t {
    struct packet_in_buffer_t ** data;
    uint16_t last_element;
    uint16_t count;
};
#define NB_SOCKETS 8
//########################################################################################

static volatile bool force_quit;

/* MAC updating enabled by default */
static int mac_updating = 1;

#define RTE_LOGTYPE_L2FWD RTE_LOGTYPE_USER1

#define MAX_PKT_BURST 32
#define BURST_TX_DRAIN_US 1 /* TX drain every ~100us */ //100
#define MEMPOOL_CACHE_SIZE 512

/*
 * Configurable number of RX/TX ring descriptors
 */
#define RTE_TEST_RX_DESC_DEFAULT 1024
#define RTE_TEST_TX_DESC_DEFAULT 1024
static uint16_t nb_rxd = RTE_TEST_RX_DESC_DEFAULT;
static uint16_t nb_txd = RTE_TEST_TX_DESC_DEFAULT;

//########################################################################################
static const int bucket_size = 1024 * 512; 
//static const int bucket_size = 1024 * 1024;

//########################################################################################

/* ethernet addresses of ports */
static struct rte_ether_addr l2fwd_ports_eth_addr[RTE_MAX_ETHPORTS];

/* mask of enabled ports */
static uint32_t l2fwd_enabled_port_mask = 0;

/* list of enabled ports */
static uint32_t l2fwd_dst_ports[RTE_MAX_ETHPORTS];

struct port_pair_params {
#define NUM_PORTS	2
	uint16_t port[NUM_PORTS];
} __rte_cache_aligned;

static struct port_pair_params port_pair_params_array[RTE_MAX_ETHPORTS / 2];
static struct port_pair_params *port_pair_params;
static uint16_t nb_port_pair_params;

static unsigned int l2fwd_rx_queue_per_lcore = 1;

#define MAX_RX_QUEUE_PER_LCORE 16
#define MAX_TX_QUEUE_PER_PORT 16
struct lcore_queue_conf {
    unsigned n_rx_port;
    unsigned rx_port_list[MAX_RX_QUEUE_PER_LCORE];
} __rte_cache_aligned;
struct lcore_queue_conf lcore_queue_conf[RTE_MAX_LCORE];


//########################################################################################
struct bucket_t* global_buckets[RTE_MAX_LCORE];
static struct rte_eth_dev_tx_buffer *tx_buffer[RTE_MAX_ETHPORTS];
//static struct rte_eth_dev_tx_buffer * tx_buffer[RTE_MAX_ETHPORTS][RTE_MAX_LCORE];

// struct rte_eth_conf port_conf = {
//     .rxmode = {
//         .mq_mode = ETH_MQ_RX_RSS,
//         .max_rx_pkt_len = 128,//RTE_ETHER_MAX_LEN,
//         .split_hdr_size = 0,
//         .offloads = BAAS_RX_OFFLOADS,
//     },
//     .rx_adv_conf = {
//         .rss_conf = {
//             .rss_key = NULL,
//             .rss_hf = (BAAS_RSS_HF),
//         },
//     },
//     .txmode = {
//         .mq_mode = ETH_MQ_TX_NONE,
//     },
// };
//########################################################################################

static struct rte_eth_conf port_conf = {
	.rxmode = {
        //.mq_mode = ETH_MQ_RX_NONE,
        .max_rx_pkt_len = 450,//RTE_ETHER_MAX_LEN,
		.split_hdr_size = 0,
        //.offloads = BAAS_RX_OFFLOADS,
	},
	.txmode = {
		.mq_mode = ETH_MQ_TX_NONE,
	},
};

//########################################################################################
struct rte_mempool * l2fwd_pktmbuf_pool = NULL;
// struct rte_mempool ** l2fwd_pktmbuf_pool;
struct rte_mempool *  clone_pktmbuf_pool;

/* Per-port statistics struct */
struct l2fwd_port_statistics {
    uint64_t tx;
    uint64_t rx;
    uint64_t dropped;
    uint64_t deleted;
    uint64_t inserted;
} __rte_cache_aligned;
struct l2fwd_port_statistics port_statistics[RTE_MAX_ETHPORTS];

//########################################################################################
#define MAX_TIMER_PERIOD 86400 /* 1 day max */
/* A tsc-based timer responsible for triggering statistics printout */
static uint64_t timer_period = 3; /* default period is 3 seconds */

/* Print out statistics on packets dropped */
static void
print_stats(void) 
{
    uint64_t total_packets_dropped, total_packets_tx, total_packets_rx;
    unsigned portid;

    total_packets_dropped = 0;
    total_packets_tx = 0;
    total_packets_rx = 0;

	const char clr[] = { 27, '[', '2', 'J', '\0' };
	const char topLeft[] = { 27, '[', '1', ';', '1', 'H','\0' };

	/* Clear screen and move to top left */
	printf("%s%s", clr, topLeft);

    printf("\nPort statistics ====================================");

    for (portid = 0; portid < RTE_MAX_ETHPORTS; portid++) {
        /* skip disabled ports */
        if ((l2fwd_enabled_port_mask & (1 << portid)) == 0)
            continue;
        printf("\nStatistics for port %u ------------------------------"
            "\nPackets sent: %24"
            PRIu64 "\nPackets received: %20"
            PRIu64 "\nPackets dropped: %21"
            PRIu64 "\nPackets buffered: %20"
            PRIu64 "\nPackets deleted: %21"
            PRIu64,
            portid,
            port_statistics[portid].tx,
            port_statistics[portid].rx,
            port_statistics[portid].dropped,
            port_statistics[portid].inserted,
            port_statistics[portid].deleted);

        total_packets_dropped += port_statistics[portid].dropped;
        total_packets_tx += port_statistics[portid].tx;
        total_packets_rx += port_statistics[portid].rx;
    }
    printf("\nAggregate statistics ==============================="
        "\nTotal packets sent: %18"
        PRIu64 "\nTotal packets received: %14"
        PRIu64 "\nTotal packets dropped: %15"
        PRIu64,
        total_packets_tx,
        total_packets_rx,
        total_packets_dropped);
    printf("\n====================================================\n");
    fflush(stdout);
}

static void
l2fwd_mac_updating(struct rte_mbuf *m, unsigned dest_portid)
{
	struct rte_ether_hdr *eth;
	void *tmp;

	eth = rte_pktmbuf_mtod(m, struct rte_ether_hdr *);

	/* 02:00:00:00:00:xx */
	tmp = &eth->d_addr.addr_bytes[0];
	*((uint64_t *)tmp) = 0x000000000002 + ((uint64_t)dest_portid << 40);

	/* src addr */
	rte_ether_addr_copy(&l2fwd_ports_eth_addr[dest_portid], &eth->s_addr);
}


//########################################################################################

static struct packet_in_buffer_t** insert_packet_to_bucket(struct bucket_t * current_bucket, struct rte_mbuf * m, uint8_t portid, uint64_t pktid) {
    if (current_bucket -> last_element >= bucket_size){
        return NULL;
    }
    struct packet_in_buffer_t** ptr = &current_bucket -> data[current_bucket -> last_element];
    if(current_bucket -> data[current_bucket -> last_element] == 0x0)
        current_bucket -> data[current_bucket -> last_element] = rte_malloc(NULL,sizeof(struct packet_in_buffer_t),0);
    current_bucket -> data[current_bucket -> last_element] -> pkt = m;
    current_bucket -> data[current_bucket -> last_element] -> portid = portid;
    current_bucket -> data[current_bucket -> last_element] -> pktid = pktid;
    current_bucket -> last_element++;
    current_bucket -> count++;
    return ptr;
}

static void remove_packet_from_bucket(struct bucket_t * current_bucket, struct packet_in_buffer_t * pointer_to_packet_in_bucket) {
    // pointer_to_packet_in_bucket->pkt = 0x0;
    // pointer_to_packet_in_bucket->portid = 0x0;
    // pointer_to_packet_in_bucket->pktid = 0x0;
    if (pointer_to_packet_in_bucket->pkt != NULL) {
        rte_pktmbuf_free(pointer_to_packet_in_bucket->pkt);
    } else {
        // Print an error message
        fprintf(stderr, "Error: Attempting to free a NULL pointer\n");
    }
    //rte_pktmbuf_free(pointer_to_packet_in_bucket->pkt);
    if (current_bucket -> count > 0)
        current_bucket -> count--;
}

static void resend_packet_from_bucket(struct packet_in_buffer_t * pointer_to_packet_in_bucket) {
    struct rte_mbuf* cloned_pkt;
    if (unlikely ((cloned_pkt = rte_pktmbuf_clone(pointer_to_packet_in_bucket->pkt, clone_pktmbuf_pool)) == NULL)){
        return;
    }

    uint8_t portid = pointer_to_packet_in_bucket -> portid;
    unsigned dst_port = l2fwd_dst_ports[portid];
    //uint8_t lcoreindex = rte_lcore_index(rte_lcore_id());
    //struct rte_eth_dev_tx_buffer * buffer = tx_buffer[portid][lcoreindex];
    struct rte_eth_dev_tx_buffer * buffer = tx_buffer[portid];
    int sent = rte_eth_tx_buffer(portid, 0, buffer, cloned_pkt);
    if (sent)
        port_statistics[portid].tx += sent;
}

static void send_out_current_bucket_wo_removal(struct bucket_t * current_bucket, struct rte_hash* buffer_table, uint8_t lcoreindex) {
    DEBUG("NOTICE, SENDING OUT BUCKET\n");
    //RTE_LOG(INFO, L2FWD, "send_out_current_bucket_wo_removal( \n");
    uint8_t shift_packets_by_index = 0;
    if (current_bucket -> last_element == 0)
        current_bucket->count = 0;

    for (int i = 0; i < current_bucket -> last_element; i++) {
        struct rte_eth_dev_tx_buffer * buffer;
        int sent;
        if (current_bucket -> data[i] == 0x0)
	    current_bucket -> data[i] = rte_malloc(NULL,sizeof(struct packet_in_buffer_t),0);
        if (current_bucket -> data[i] -> pkt != 0x0) {
            uint8_t portid = current_bucket -> data[i] -> portid;
            unsigned dst_port = l2fwd_dst_ports[portid];
            //buffer = tx_buffer[portid][lcoreindex];
            buffer = tx_buffer[portid];

            if (shift_packets_by_index > 0){
                DEBUG("WARNING, - SHIFTING NEEDED %d\n", shift_packets_by_index);
                current_bucket -> data[i-shift_packets_by_index]->pkt = current_bucket -> data[i]->pkt;
                current_bucket -> data[i-shift_packets_by_index]->portid = current_bucket -> data[i]->portid;
                current_bucket -> data[i-shift_packets_by_index]->pktid = current_bucket -> data[i]->pktid;
                current_bucket -> data[i]->pkt = 0x0;
                current_bucket -> data[i]->portid = 0x0;
                current_bucket -> data[i]->pktid = 0x0;
                struct packet_in_buffer_t** ptr = &current_bucket -> data[i-shift_packets_by_index];

                //rte_hash_del_key(buffer_table, &current_bucket -> data[i-shift_packets_by_index]->pktid);
                int ret = rte_hash_add_key_data(buffer_table, &(current_bucket -> data[i-shift_packets_by_index]->pktid), *ptr);
                if (ret == 22){
                    DEBUG("ERROR INSERTION: %d WRONG PARAM\n", ret);
                    rte_exit(EXIT_FAILURE, "UNABLE TO STORE HASH ENTRY WRONG PARAM\n");
                }else if (ret == ENOSPC){
                    DEBUG("ERROR INSERTION: %d NO SPACE\n", ret);
                    rte_exit(EXIT_FAILURE, "UNABLE TO STORE HASH ENTRY NO SPACE\n");
                }else{
                    DEBUG("NOTICE, INSERTION %d OK\n", ret);
                }


            }
            //DEBUG("NOTICE, CLONING\n");
            /**
            * Create a "clone" of the given packet mbuf.
            *
            * Walks through all segments of the given packet mbuf, and for each of them:
            *  - Creates a new packet mbuf from the given pool.
            *  - Attaches newly created mbuf to the segment.
            * Then updates pkt_len and nb_segs of the "clone" packet mbuf to match values
            * from the original packet mbuf.
            *
            * @param md
            *   The packet mbuf to be cloned.
            * @param mp
            *   The mempool from which the "clone" mbufs are allocated.
            * @return
            *   - The pointer to the new "clone" mbuf on success.
            *   - NULL if allocation fails.
            */
            struct rte_mbuf* cloned_pkt;
            if (current_bucket -> data[i-shift_packets_by_index] -> pkt == 0x0 || unlikely ((cloned_pkt = rte_pktmbuf_clone(current_bucket -> data[i-shift_packets_by_index] -> pkt, clone_pktmbuf_pool)) == NULL)){
                rte_pktmbuf_free(cloned_pkt);
                continue;
            }
            //DEBUG("NOTICE, CLONING DONE\n");
            sent = rte_eth_tx_buffer(portid, 0, buffer, cloned_pkt);
            if (sent)
                port_statistics[portid].tx += sent;
        }else{
            shift_packets_by_index++;
        }
    }
    current_bucket -> last_element -= shift_packets_by_index;
    //current_bucket -> count = current_bucket -> last_element;
}


static void send_out_current_bucket(struct bucket_t * current_bucket, struct rte_hash* buffer_table, uint8_t lcoreindex) {
    for (int i = 0; i < current_bucket -> last_element; i++) {
        struct rte_eth_dev_tx_buffer * buffer;
        int sent;
        if (current_bucket -> data[i] -> pkt != 0x0) {
            uint8_t portid = current_bucket -> data[i] -> portid;
            unsigned dst_port = l2fwd_dst_ports[portid];
            //buffer = tx_buffer[portid][lcoreindex];
            buffer = tx_buffer[portid];

            /**
            * Buffer a single packet for future transmission on a port and queue
            *
            * This function takes a single mbuf/packet and buffers it for later
            * transmission on the particular port and queue specified. Once the buffer is
            * full of packets, an attempt will be made to transmit all the buffered
            * packets. In case of error, where not all packets can be transmitted, a
            * callback is called with the unsent packets as a parameter. If no callback
            * is explicitly set up, the unsent packets are just freed back to the owning
            * mempool. The function returns the number of packets actually sent i.e.
            * 0 if no buffer flush occurred, otherwise the number of packets successfully
            * flushed
            *
            * @param port_id
            *   The port identifier of the Ethernet device.
            * @param queue_id
            *   The index of the transmit queue through which output packets must be
            *   sent.
            *   The value must be in the range [0, nb_tx_queue - 1] previously supplied
            *   to rte_eth_dev_configure().
            * @param buffer
            *   Buffer used to collect packets to be sent.
            * @param tx_pkt
            *   Pointer to the packet mbuf to be sent.
            * @return
            *   0 = packet has been buffered for later transmission
            *   N > 0 = packet has been buffered, and the buffer was subsequently flushed,
            *     causing N packets to be sent, and the error callback to be called for
            *     the rest.
            */
            sent = rte_eth_tx_buffer(portid, 0, buffer, current_bucket -> data[i] -> pkt);

            rte_hash_del_key(buffer_table, &current_bucket -> data[i] -> pktid);

            current_bucket -> data[i] -> pkt = 0x0;
            current_bucket -> data[i] -> portid = 0x0;
            current_bucket -> data[i] -> pktid = 0x0;
            if (sent)
                port_statistics[portid].tx += sent;
        }
    }
    current_bucket -> last_element = 0;
    current_bucket -> count = 0;
}


static bool
array_contains(uint64_t element, uint64_t array[], uint32_t array_size){
    for (uint32_t i=0; i<array_size; i++)
        if (array[i] == element)
            return true;
    return false;
}

static void
handle_incoming_packet(struct rte_mbuf * m, struct bucket_t * current_bucket, struct rte_hash* buffer_table, struct rte_hash* buffer_table_teid, uint8_t portid) {
    uint8_t  nack_count = 0x55;
    unsigned dst_port;

    dst_port = l2fwd_dst_ports[portid];
    
    uint8_t * data_start = rte_pktmbuf_mtod(m, uint8_t * );
    if (unlikely(data_start == 0x0)) return;

    if (*(uint16_t *)(data_start + 12) != htobe16(0x800)){
        rte_pktmbuf_free(m);
        return;
    }


	if (mac_updating)
		l2fwd_mac_updating(m, dst_port);
    
    // //SWAP MAC ADDRESSES
    // struct rte_ether_hdr *eth;
    // struct rte_ether_addr tmp;
    // eth = rte_pktmbuf_mtod(m, struct rte_ether_hdr *);
    // rte_ether_addr_copy(&eth->d_addr, &tmp);
    // rte_ether_addr_copy(&eth->s_addr, &eth->d_addr);
    // rte_ether_addr_copy(&tmp, &eth->s_addr);

    uint8_t *ip = data_start + 14;
    // if (ip[0] != 0x45 || ip[9] != 17){ // ipv4+udp
    //     rte_pktmbuf_free(m);
    //     return;
    // }
    // //SWAP IP ADDRESSES
    // uint32_t tmp_ip;
    // tmp_ip = *(uint32_t*)(ip+12);
    // *(uint32_t*)(ip+12) = *(uint32_t*)(ip+16);
    // *(uint32_t*)(ip+16) = tmp_ip;


    uint8_t *udp = ip + 20;
    // if (
	// 	// *(uint16_t*)(udp + 0) != htobe16(BAAS_UDP_PORT) ||
    //     *(uint16_t*)(udp + 2) != htobe16(BAAS_UDP_PORT)){
    //     rte_pktmbuf_free(m);
    //     return;
    // }

    //SWAP UDP PORTS
    uint16_t udp_tmp;
    udp_tmp = *(uint16_t*)(udp + 0);
    //RTE_LOG(INFO, L2FWD,"src: %x \n", be16toh(udp_tmp));
    *(uint16_t*)(udp + 0) = *(uint16_t*)(udp + 2);
    *(uint16_t*)(udp + 2) = udp_tmp;

    uint8_t *buffheader = udp + 8;

    //uint8_t  nack_count = *buffheader;
    nack_count = *buffheader;
    //RTE_LOG(INFO, L2FWD,"******\n");
    //RTE_LOG(INFO, L2FWD,"nack count: %x \n", be16toh(nack_count));
    
    uint64_t endpoint_id = be32toh(*(uint32_t * )(buffheader + 1));
    DEBUG("NOTE, endpoint parsed 0x%.16" PRIX64"  \n",endpoint_id);
    uint16_t ack_sequence_number = be16toh(* (uint16_t * )(buffheader + 1+4));
    uint64_t packet_id = (endpoint_id<<32)+ack_sequence_number;
   
    uint8_t *rlcstatus = buffheader + 7; // you have rlc status header not rlc ack header, its only used inside the if
    //uint8_t *rlcstatus = buffheader + 9;

    uint8_t *nacks = rlcstatus + 8;
    uint8_t *check_e_for_nacks = rlcstatus + 3;
    uint8_t e = (*(uint8_t * )(check_e_for_nacks)) ;
    int efirstBit = (e & 0x80) ? 1 : 0;
    DEBUG("NOTE, E bit : %d \n", efirstBit);
    DEBUG("NOTICE, endpoint_id 0x%.16" PRIX64 " endpoint shifted 0x%.16" PRIX64" packet_id 0x%.16" PRIX64" \n", endpoint_id, endpoint_id<<32, packet_id);
    if (nack_count != 0xff) {
        //ack_sequence_number = be16toh(* (uint16_t * )(rlcstatus + 1));
        DEBUG("NOTE, ACK arrived 0x%.16" PRIX16 "\n", ack_sequence_number);
        
        nack_count=1;
		//DEBUG("NOTICE, nack count: %d \n", nack_count);
        uint64_t nack_list[nack_count];  // have 3 nack header means diff lost location in segments

        if (efirstBit==1){
        for (int nack_index=0; nack_index<nack_count; nack_index++){
            uint16_t nack_serial_number = be16toh(*(uint16_t * )(nacks + nack_index*2));
            uint64_t nack_pkt_id = (endpoint_id<<32)+nack_serial_number;
            DEBUG("NOTE, Prepring for INSTANT RESUBMIT FOR PACKET 0x%.16" PRIX64 " \n", nack_serial_number);
            struct packet_in_buffer_t* pointer_to_packet_in_bucket;
            int ret = rte_hash_lookup_data(buffer_table, &nack_pkt_id, (void**) &pointer_to_packet_in_bucket);
            if(ret >= 0) {
                DEBUG("NOTE, PACKET FOUND because of NACK , Don't delete it from RING 0x%.16" PRIX64"  \n", nack_serial_number);
                nack_list[nack_index] = nack_pkt_id;
                resend_packet_from_bucket(pointer_to_packet_in_bucket);
            }else{
                DEBUG("WARNING, PACKET NOT FOUND...because of NACK 0x%.16" PRIX64"  \n", nack_pkt_id);
            }
        }
        }


        // uint16_t* first_seq_to_ack_ptr;
        // DEBUG("NOTE, endpoint for searching 0x%.16" PRIX64"  \n",endpoint_id); 
        // int ret = rte_hash_lookup_data(buffer_table_teid, &endpoint_id, (void**)&first_seq_to_ack_ptr);
        // uint16_t first_seq_to_ack;
        // if(ret >= 0) {
        //     first_seq_to_ack = *first_seq_to_ack_ptr;
        //     DEBUG("NOTE, first_seq_to_ack found 0x%.16" PRIX64"  \n", first_seq_to_ack);
        // }else{
        //     first_seq_to_ack_ptr = rte_malloc(NULL,sizeof(uint16_t),0);
        //     if (unlikely(!first_seq_to_ack_ptr)) abort();
        //     DEBUG("NOTE, first_seq_to_ack NOT found 0x%.16" PRIX64"  \n", first_seq_to_ack);
        //     first_seq_to_ack = 0;
        // }
        // //if (ack_sequence_number < 65500) {
        // if (ack_sequence_number < 4) {
        // *first_seq_to_ack_ptr = ack_sequence_number+1;  // current seq number + 1 equal next seq number
        // DEBUG("NOTE, first_seq_to_ack ptr for ADDing 0x%.16" PRIX64"  \n", *first_seq_to_ack_ptr); 
        // DEBUG("NOTE, endpoint for ADDing 0x%.16" PRIX64"  \n",endpoint_id); 
        // //ack seq in bufffering header is ack seq in am rlc header
        // rte_hash_add_key_data(buffer_table_teid, &endpoint_id, first_seq_to_ack_ptr);
        //     //first_seq_to_ack = 0;
        // }

         
        // i am waiting for ack seq 2,ack is arrived for seq 1. i have all of this (endpointid<beade on teid>,current seq number+1) pair in table buffer_table_teid

        // uint64_t ack_pkt_id;
        // unsigned max_ack = ack_sequence_number;
        // if (first_seq_to_ack > ack_sequence_number){
        //    //max_ack = ack_sequence_number + 0x10000;
        //    max_ack = ack_sequence_number; // handle overflow
        // }
        //for (unsigned ack_index=first_seq_to_ack; ack_index<=max_ack; ack_index++){
            //ack_pkt_id = (endpoint_id<<32)+(ack_index & 0xffff);
            //ack_pkt_id = (endpoint_id<<32)+(ack_sequence_number & 0xffff);*
            //ack_pkt_id = (endpoint_id<<32)+(ack_sequence_number);
            // ack_pkt_id = (endpoint_id<<32)+ack_sequence_number;
        int del=1;
        if (efirstBit==1){
            if (array_contains(packet_id, nack_list, nack_count)){ // Skip the list of nacks    **** base of decision of being acked or not ****
                    del=0;
                    //continue;
            }
        }
        if (del==1){
            struct packet_in_buffer_t* pointer_to_packet_in_bucket1;
            //int ret1 = rte_hash_lookup_data(buffer_table, &ack_pkt_id, (void**)&pointer_to_packet_in_bucket1);
            int ret1 = rte_hash_lookup_data(buffer_table, &packet_id, (void**)&pointer_to_packet_in_bucket1);

            if(ret1 >= 0) {
                DEBUG("NOTE, PACKET FOUND 0x%.16" PRIX64"  \n", packet_id);
                remove_packet_from_bucket(current_bucket, pointer_to_packet_in_bucket1);
                //RTE_LOG(INFO, L2FWD, "NOTE, ACK arrived id/seq 0x%.16" PRIX64 " 0x%.16" PRIX16 "\n", packet_id, ack_sequence_number);
                DEBUG("NOTE, ACK arrived DELETING FROM 0x%.4" PRIX16 " 0x%.16" PRIX16 "\n", first_seq_to_ack, ack_sequence_number); 
                port_statistics[portid].deleted += 1;
                //rte_hash_del_key(buffer_table, &ack_pkt_id);
                rte_hash_del_key(buffer_table, &packet_id);
            }else{
                DEBUG("ACK DROP, PACKET NOT FOUND 0x%.16" PRIX64"  \n", packet_id);
                //RTE_LOG(INFO, L2FWD, "ACK DROP, PACKET NOT FOUND 0x%.16" PRIX64" \n" , packet_id);
                port_statistics[portid].dropped += 1;
            }
        }
        rte_pktmbuf_free(m);


    } else {
        //RTE_LOG(INFO, L2FWD, "handling func: pktid %u,teid %u,ack sn%u in lcore %u\n", packet_id,endpoint_id,ack_sequence_number,rte_lcore_id());		
        //you have rlc ack header , searching in hash tabels depends on buffering header not anything else
        struct packet_in_buffer_t* packet_in_bucket;
        DEBUG("NOTE, GTP arrived 0x%.16" PRIX16 "\n", ack_sequence_number);
        //RTE_LOG(INFO, L2FWD, "NOTE, GTP arrived 0x%.16" PRIX64" \n", packet_id);
        DEBUG("NOTICE, PKT pre storing: 0x%.16" PRIX64"\n", packet_id);
        /**
           Find a key-value pair in the hash table.
         * @param h
         *   Hash table to look in.
         * @param key
         *   Key to find.
         * @param data
         *   Output with pointer to data returned from the hash table.
         */
        int ret = rte_hash_lookup_data(buffer_table, &packet_id, (void**)&packet_in_bucket);
        if(ret >= 0) {
            DEBUG("DROP, PACKET IS ALREADY BUFFERED 0x%.16" PRIX64" \n", packet_id);
            RTE_LOG(INFO, L2FWD, "DROP, PACKET IS ALREADY BUFFERED 0x%.16" PRIX64" \n", packet_id);

            port_statistics[portid].dropped += 1;
            rte_pktmbuf_free(m);
            return;
        }

        DEBUG("NOTICE, PKT storing: 0x%.16" PRIX64" \n", packet_id);
        struct packet_in_buffer_t** pointer_to_packet_in_bucket;

        pointer_to_packet_in_bucket = insert_packet_to_bucket(current_bucket, m, portid, packet_id);
        if (pointer_to_packet_in_bucket == NULL){
            DEBUG("DROP, PACKET CANNOT BE STORED IN BUCKET, DROPPED 0x%.16" PRIX64 " \n", packet_id);
            port_statistics[portid].dropped += 1;
            rte_pktmbuf_free(m);
        }else{
            port_statistics[portid].inserted += 1;
            

            /**
            * Add a key-value pair to an existing hash table.
            * This operation is not multi-thread safe
            * @param h
            *   Hash table to add the key to.
            * @param key
            *   Key to add to the hash table.
            * @param data
            *   Data to add to the hash table.
            */
            // we save this key/value (pkt_id,pointer to the pkts in the bucket)
            int ret = rte_hash_add_key_data(buffer_table, &packet_id, *pointer_to_packet_in_bucket);
            //int ret = rte_hash_add_key_data(buffer_table, &packet_id, m);
            if (ret == 22){
                DEBUG("ERROR INSERTION: %d WRONG PARAM\n", ret);
                rte_exit(EXIT_FAILURE, "UNABLE TO STORE HASH ENTRY WRONG PARAM\n");
            }else if (ret == ENOSPC){
                DEBUG("ERROR INSERTION: %d NO SPACE\n", ret);
                rte_exit(EXIT_FAILURE, "UNABLE TO STORE HASH ENTRY NO SPACE\n");
            }else{
                DEBUG("NOTICE, INSERTION %d OK\n", ret);
            }
        }
    }
}

//########################################################################################

// static void
// l2fwd_simple_forward(struct rte_mbuf *m, unsigned portid)
// {
// 	unsigned dst_port;
// 	int sent;
// 	struct rte_eth_dev_tx_buffer *buffer;

// 	dst_port = l2fwd_dst_ports[portid];

// 	if (mac_updating)
// 		l2fwd_mac_updating(m, dst_port);

// 	buffer = tx_buffer[dst_port];
// 	sent = rte_eth_tx_buffer(dst_port, 0, buffer, m);
// 	if (sent)
// 		port_statistics[dst_port].tx += sent;
// }
/* main processing loop */
static void
l2fwd_main_loop(void) 
{
    struct rte_mbuf *pkts_burst[MAX_PKT_BURST];
    struct rte_mbuf *m;
    int sent;
    unsigned lcore_id;
    uint64_t prev_tsc, diff_tsc, cur_tsc, timer_tsc;
    cur_tsc = 0;
    unsigned i, j, portid, nb_rx;
    struct lcore_queue_conf *qconf;
    const uint64_t drain_tsc = (rte_get_tsc_hz() + US_PER_S - 1) / US_PER_S *
        BURST_TX_DRAIN_US;
    struct rte_eth_dev_tx_buffer *buffer;

    prev_tsc = 0;
    timer_tsc = 0;

    lcore_id = rte_lcore_id();
    qconf = &lcore_queue_conf[lcore_id];

    if (qconf->n_rx_port == 0) {
        RTE_LOG(INFO, L2FWD, "lcore %u has nothing to do\n", lcore_id);
        return;
    }

    RTE_LOG(INFO, L2FWD, "entering main loop on lcore %u\n", lcore_id);

    for (i = 0; i < qconf->n_rx_port; i++) {

        portid = qconf->rx_port_list[i];
        RTE_LOG(INFO, L2FWD, " -- lcoreid=%u portid=%u\n", lcore_id,
            portid);

    }
    unsigned dst_port = l2fwd_dst_ports[portid];
    ////Create Hash table
    // char tmp[50];
    // sprintf(tmp, "HashTable%d", lcore_id);
    // ut_params.name = tmp;
    // struct rte_hash* buffer_table = rte_hash_create(&ut_params);
    // if (buffer_table == NULL) rte_exit(EXIT_FAILURE, "UNABLE TO CREATE HASHTABLE\n");

    // sprintf(tmp, "HashTableTeid%d", lcore_id);
    // ut_params.name = tmp;
    // struct rte_hash* buffer_table_teid = rte_hash_create(&ut_params_teid);
    // if (buffer_table_teid == NULL) rte_exit(EXIT_FAILURE, "UNABLE TO CREATE HASHTABLE\n");

    uint8_t current_bucket = 0;
    //struct bucket_t* buckets = malloc(sizeof(struct bucket_t) * resubmission_time);
    struct bucket_t* buckets = rte_malloc(NULL, sizeof(struct bucket_t) * resubmission_time, 0);
    global_buckets[lcore_id] = buckets;
    //global_buckets[0] = buckets;

    for (int i = 0; i < resubmission_time; i++) {
        buckets[i].data = rte_malloc(NULL, bucket_size * sizeof(struct packet_in_buffer_t* ), 0);
        //buckets[i].data = malloc(bucket_size * sizeof(struct packet_in_buffer_t* ));
        buckets[i].last_element = 0;
        buckets[i].count = 0;
        for (int j = 0; j < bucket_size; j++) {
            buckets[i].data[j] = rte_malloc(NULL, sizeof(struct packet_in_buffer_t), 0);
            //buckets[i].data[j] = malloc(sizeof(struct packet_in_buffer_t));
            if (buckets[i].data[j] == NULL) rte_exit(EXIT_FAILURE, "UNABLE TO ALLOCATE BUCKET\n");
        }
    }
    int previous_bucket = -1;
    uint8_t lcoreindex = rte_lcore_index(lcore_id);

    //Get the number of cycles in one second for the default timer
    uint64_t proc_timer_hz = rte_get_timer_hz();
    //function named rte_rdtsc, which is used to read the timestamp counter (TSC) on x86-based systems
    uint64_t now = rte_rdtsc();

    while (!force_quit) {
        
        now = rte_rdtsc();
        if (now - (proc_timer_hz/10.0)*resubmission_time > cur_tsc) {
            cur_tsc = now;
            current_bucket = (current_bucket + 1) % resubmission_time;
        }
        //DEBUG("LCORE %d -- Current bucket:%d == prev %d => %d \n", lcore_id, current_bucket, previous_bucket, current_bucket == previous_bucket);
        if (previous_bucket != current_bucket) {
            //send_out_current_bucket( & (buckets[current_bucket]), buffer_table, lcoreindex );
            send_out_current_bucket_wo_removal( & (buckets[current_bucket]), buffer_table, lcoreindex );
            previous_bucket = current_bucket;
            /*
             * TX burst queue drain
             */
            diff_tsc = cur_tsc - prev_tsc;
            if (unlikely(diff_tsc > drain_tsc)) {

                for (i = 0; i < qconf->n_rx_port; i++) {

                    portid = l2fwd_dst_ports[qconf->rx_port_list[i]];
                    //buffer = tx_buffer[portid][lcoreindex];
                    buffer = tx_buffer[portid];

                /**
                * Send any packets queued up for transmission on a port and HW queue
                *
                * This causes an explicit flush of packets previously buffered via the
                * rte_eth_tx_buffer() function. It returns the number of packets successfully
                * sent to the NIC, and calls the error callback for any unsent packets. Unless
                * explicitly set up otherwise, the default callback simply frees the unsent
                * packets back to the owning mempool.
                *
                * @param port_id
                *   The port identifier of the Ethernet device.
                * @param queue_id
                *   The index of the transmit queue through which output packets must be
                *   sent.
                *   The value must be in the range [0, nb_tx_queue - 1] previously supplied
                *   to rte_eth_dev_configure().
                * @param buffer
                *   Buffer of packets to be transmit.
                * @return
                *   The number of packets successfully sent to the Ethernet device. The error
                *   callback is called for any packets which could not be sent.
                */

                    sent = rte_eth_tx_buffer_flush(portid, 0, buffer);
                    //RTE_LOG(INFO, L2FWD,"flush\n");
                    if (sent)
                        //port_statistics[portid].tx += sent;
                        port_statistics[portid].tx += sent;

                }

                /* if timer is enabled */
                if (timer_period > 0) {

                    /* advance the timer */
                    timer_tsc += diff_tsc;

                    /* if timer has reached its timeout */
                    if (unlikely(timer_tsc >= timer_period)) {

                        /* do this only on master core */
                        if (lcore_id == rte_get_master_lcore()) {
                            print_stats();
                            int sum_buckets = 0;
                            for (int lc=0; lc<RTE_MAX_LCORE; lc++){
                                if (!rte_lcore_is_enabled(lc)) continue;
                                // for (int i = 0; i < resubmission_time; i++) {
                                //     if(unlikely(port_statistics[portid].inserted == port_statistics[portid].deleted)){
                                //         global_buckets[lc][i].count = 0;
                                //         global_buckets[lc][i].last_element = 0;
                                //     }
                                //     printf("LCORE %d: Bucket %d has %d pkt\n", lc, i, global_buckets[lc][i].count);
                                //     sum_buckets += global_buckets[lc][i].count;
                                //}
                }
                            //printf("Bucket SUM has %d pkt\n", sum_buckets);
                            /* reset the timer */
                            timer_tsc = 0;
                        }
                    }
                }

                prev_tsc = cur_tsc;
            }
        }

        /*
         * Read packet from RX queues
         */
        for (i = 0; i < qconf->n_rx_port; i++) {

            portid = qconf->rx_port_list[i];
            /**
            *
            * Retrieve a burst of input packets from a receive queue of an Ethernet
            * device. The retrieved packets are stored in *rte_mbuf* structures whose
            * pointers are supplied in the *rx_pkts* array.
            *
            * The rte_eth_rx_burst() function loops, parsing the RX ring of the
            * receive queue, up to *nb_pkts* packets, and for each completed RX
            * descriptor in the ring
            * @param port_id
            *   The port identifier of the Ethernet device.
            * @param queue_id
            *   The index of the receive queue from which to retrieve input packets.
            *   The value must be in the range [0, nb_rx_queue - 1] previously supplied
            *   to rte_eth_dev_configure().
            * @param rx_pkts
            *   The address of an array of pointers to *rte_mbuf* structures that
            *   must be large enough to store *nb_pkts* pointers in it.
            * @param nb_pkts
            *   The maximum number of packets to retrieve.
            * @return
            *   The number of packets actually retrieved, which is the number
            *   of pointers to *rte_mbuf* structures effectively supplied to the
            *   *rx_pkts* array.
            */
            
            nb_rx = rte_eth_rx_burst(portid, 0,
                pkts_burst, MAX_PKT_BURST);

            port_statistics[portid].rx += nb_rx;

            for (j = 0; j < nb_rx; j++) {
                m = pkts_burst[j];
                rte_prefetch0(rte_pktmbuf_mtod(m, void *));
                //l2fwd_simple_forward(m, portid);
                //this func works on one packet in m struct
                handle_incoming_packet(m, & buckets[current_bucket], buffer_table, buffer_table_teid, portid);
            }
        }
    }
}
//########################################################################################

static int
l2fwd_launch_one_lcore(__rte_unused void *dummy)
{
	l2fwd_main_loop();
	return 0;
}

/* display usage */
static void
l2fwd_usage(const char *prgname)
{
	printf("%s [EAL options] -- -p PORTMASK [-q NQ]\n"
	       "  -p PORTMASK: hexadecimal bitmask of ports to configure\n"
	       "  -q NQ: number of queue (=ports) per lcore (default is 1)\n"
	       "  -T PERIOD: statistics will be refreshed each PERIOD seconds (0 to disable, 10 default, 86400 maximum)\n"
	       "  --[no-]mac-updating: Enable or disable MAC addresses updating (enabled by default)\n"
	       "      When enabled:\n"
	       "       - The source MAC address is replaced by the TX port MAC address\n"
	       "       - The destination MAC address is replaced by 02:00:00:00:00:TX_PORT_ID\n"
	       "  --portmap: Configure forwarding port pair mapping\n"
	       "	      Default: alternate port pairs\n\n",
	       prgname);
}

static int
l2fwd_parse_portmask(const char *portmask)
{
	char *end = NULL;
	unsigned long pm;

	/* parse hexadecimal string */
	pm = strtoul(portmask, &end, 16);
	if ((portmask[0] == '\0') || (end == NULL) || (*end != '\0'))
		return 0;

	return pm;
}

static int
l2fwd_parse_port_pair_config(const char *q_arg)
{
	enum fieldnames {
		FLD_PORT1 = 0,
		FLD_PORT2,
		_NUM_FLD
	};
	unsigned long int_fld[_NUM_FLD];
	const char *p, *p0 = q_arg;
	char *str_fld[_NUM_FLD];
	unsigned int size;
	char s[256];
	char *end;
	int i;

	nb_port_pair_params = 0;

	while ((p = strchr(p0, '(')) != NULL) {
		++p;
		p0 = strchr(p, ')');
		if (p0 == NULL)
			return -1;

		size = p0 - p;
		if (size >= sizeof(s))
			return -1;

		memcpy(s, p, size);
		s[size] = '\0';
		if (rte_strsplit(s, sizeof(s), str_fld,
				 _NUM_FLD, ',') != _NUM_FLD)
			return -1;
		for (i = 0; i < _NUM_FLD; i++) {
			errno = 0;
			int_fld[i] = strtoul(str_fld[i], &end, 0);
			if (errno != 0 || end == str_fld[i] ||
			    int_fld[i] >= RTE_MAX_ETHPORTS)
				return -1;
		}
		if (nb_port_pair_params >= RTE_MAX_ETHPORTS/2) {
			printf("exceeded max number of port pair params: %hu\n",
				nb_port_pair_params);
			return -1;
		}
		port_pair_params_array[nb_port_pair_params].port[0] =
				(uint16_t)int_fld[FLD_PORT1];
		port_pair_params_array[nb_port_pair_params].port[1] =
				(uint16_t)int_fld[FLD_PORT2];
		++nb_port_pair_params;
	}
	port_pair_params = port_pair_params_array;
	return 0;
}

static unsigned int
l2fwd_parse_nqueue(const char *q_arg)
{
	char *end = NULL;
	unsigned long n;

	/* parse hexadecimal string */
	n = strtoul(q_arg, &end, 10);
	if ((q_arg[0] == '\0') || (end == NULL) || (*end != '\0'))
		return 0;
	if (n == 0)
		return 0;
	if (n >= MAX_RX_QUEUE_PER_LCORE)
		return 0;

	return n;
}

static int
l2fwd_parse_timer_period(const char *q_arg)
{
	char *end = NULL;
	int n;

	/* parse number string */
	n = strtol(q_arg, &end, 10);
	if ((q_arg[0] == '\0') || (end == NULL) || (*end != '\0'))
		return -1;
	if (n >= MAX_TIMER_PERIOD)
		return -1;

	return n;
}

static const char short_options[] =
	"p:"  /* portmask */
	"q:"  /* number of queues */
	"T:"  /* timer period */
	;

#define CMD_LINE_OPT_MAC_UPDATING "mac-updating"
#define CMD_LINE_OPT_NO_MAC_UPDATING "no-mac-updating"
#define CMD_LINE_OPT_PORTMAP_CONFIG "portmap"

enum {
	/* long options mapped to a short option */

	/* first long only option value must be >= 256, so that we won't
	 * conflict with short options */
	CMD_LINE_OPT_MIN_NUM = 256,
	CMD_LINE_OPT_PORTMAP_NUM,
};

static const struct option lgopts[] = {
	{ CMD_LINE_OPT_MAC_UPDATING, no_argument, &mac_updating, 1},
	{ CMD_LINE_OPT_NO_MAC_UPDATING, no_argument, &mac_updating, 0},
	{ CMD_LINE_OPT_PORTMAP_CONFIG, 1, 0, CMD_LINE_OPT_PORTMAP_NUM},
	{NULL, 0, 0, 0}
};

/* Parse the argument given in the command line of the application */
static int
l2fwd_parse_args(int argc, char **argv)
{
	int opt, ret, timer_secs;
	char **argvopt;
	int option_index;
	char *prgname = argv[0];

	argvopt = argv;
	port_pair_params = NULL;

	while ((opt = getopt_long(argc, argvopt, short_options,
				  lgopts, &option_index)) != EOF) {

		switch (opt) {
		/* portmask */
		case 'p':
			l2fwd_enabled_port_mask = l2fwd_parse_portmask(optarg);
			if (l2fwd_enabled_port_mask == 0) {
				printf("invalid portmask\n");
				l2fwd_usage(prgname);
				return -1;
			}
			break;

		/* nqueue */
		case 'q':
			l2fwd_rx_queue_per_lcore = l2fwd_parse_nqueue(optarg);
			if (l2fwd_rx_queue_per_lcore == 0) {
				printf("invalid queue number\n");
				l2fwd_usage(prgname);
				return -1;
			}
			break;

		/* timer period */
		case 'T':
			timer_secs = l2fwd_parse_timer_period(optarg);
			if (timer_secs < 0) {
				printf("invalid timer period\n");
				l2fwd_usage(prgname);
				return -1;
			}
			timer_period = timer_secs;
			break;

		/* long options */
		case CMD_LINE_OPT_PORTMAP_NUM:
			ret = l2fwd_parse_port_pair_config(optarg);
			if (ret) {
				fprintf(stderr, "Invalid config\n");
				l2fwd_usage(prgname);
				return -1;
			}
			break;

		default:
			l2fwd_usage(prgname);
			return -1;
		}
	}

	if (optind >= 0)
		argv[optind-1] = prgname;

	ret = optind-1;
	optind = 1; /* reset getopt lib */
	return ret;
}

/*
 * Check port pair config with enabled port mask,
 * and for valid port pair combinations.
 */
static int
check_port_pair_config(void)
{
	uint32_t port_pair_config_mask = 0;
	uint32_t port_pair_mask = 0;
	uint16_t index, i, portid;

	for (index = 0; index < nb_port_pair_params; index++) {
		port_pair_mask = 0;

		for (i = 0; i < NUM_PORTS; i++)  {
			portid = port_pair_params[index].port[i];
			if ((l2fwd_enabled_port_mask & (1 << portid)) == 0) {
				printf("port %u is not enabled in port mask\n",
				       portid);
				return -1;
			}
			if (!rte_eth_dev_is_valid_port(portid)) {
				printf("port %u is not present on the board\n",
				       portid);
				return -1;
			}

			port_pair_mask |= 1 << portid;
		}

		if (port_pair_config_mask & port_pair_mask) {
			printf("port %u is used in other port pairs\n", portid);
			return -1;
		}
		port_pair_config_mask |= port_pair_mask;
	}

	l2fwd_enabled_port_mask &= port_pair_config_mask;

	return 0;
}

/* Check the link status of all ports in up to 9s, and print them finally */
static void
check_all_ports_link_status(uint32_t port_mask) 
{
    #define CHECK_INTERVAL 100 /* 100ms */
    #define MAX_CHECK_TIME 90 /* 9s (90 * 100ms) in total */
    uint16_t portid;
    uint8_t count, all_ports_up, print_flag = 0;
    struct rte_eth_link link;
    int ret;

    printf("\nChecking link status");
    fflush(stdout);
    for (count = 0; count <= MAX_CHECK_TIME; count++) {
        if (force_quit)
            return;
        all_ports_up = 1;
        RTE_ETH_FOREACH_DEV(portid) {
            if (force_quit)
                return;
            if ((port_mask & (1 << portid)) == 0)
                continue;
            			memset(&link, 0, sizeof(link));
			ret = rte_eth_link_get_nowait(portid, &link);
			if (ret < 0) {
				all_ports_up = 0;
				if (print_flag == 1)
					printf("Port %u link get failed: %s\n",
						portid, rte_strerror(-ret));
				continue;
			}
            /* print link status if flag set */
            if (print_flag == 1) {
                if (link.link_status)
                    printf(
                        "Port%d Link Up. Speed %u Mbps - %s\n",
                        portid, link.link_speed,
                        (link.link_duplex == ETH_LINK_FULL_DUPLEX) ?
                        ("full-duplex") : ("half-duplex"));
                else
                    printf("Port %d Link Down\n", portid);
                continue;
            }
            /* clear all_ports_up flag if any link down */
            if (link.link_status == ETH_LINK_DOWN) {
                all_ports_up = 0;
                break;
            }
        }
        /* after finally printing all link status, get out */
    if (print_flag == 1)
            break;

        if (all_ports_up == 0) {
            printf(".");
            fflush(stdout);
            rte_delay_ms(CHECK_INTERVAL);
        }

        /* set the print_flag if all ports up or timeout */
        if (all_ports_up == 1 || count == (MAX_CHECK_TIME - 1)) {
            print_flag = 1;
            printf("done\n");
        }
    }
}

static void
signal_handler(int signum) 
{
    if (signum == SIGINT || signum == SIGTERM) {
        printf("\n\nSignal %d received, preparing to exit...\n",
            signum);
        force_quit = true;
    }
}

static int
init_mem(void)
{
    //Create Hash table
    char tmp[50];
    ut_params.name = tmp;
    //ut_params.socket_id = socketid;
				
    buffer_table = rte_hash_create(&ut_params);
    if (buffer_table == NULL) {
        printf("UNABLE TO CREATE HASHTABLE\n");
        rte_exit(EXIT_FAILURE, "UNABLE TO CREATE HASHTABLE\n");
    }
    buffer_table_teid = rte_hash_create(&ut_params_teid);
    if (buffer_table_teid == NULL) {
        printf("UNABLE TO CREATE HASHTABLE\n");
        rte_exit(EXIT_FAILURE, "UNABLE TO CREATE HASHTABLE\n");
        
    }

}

int
main(int argc, char **argv)
{
	struct lcore_queue_conf *qconf;
	int ret;
	uint16_t nb_ports;
	uint16_t nb_ports_available = 0;
	uint16_t portid, last_port;
	unsigned lcore_id, rx_lcore_id;
	unsigned nb_ports_in_mask = 0;
	unsigned int nb_lcores = 0;
	unsigned int nb_mbufs;

	/* init EAL */
	ret = rte_eal_init(argc, argv);
	if (ret < 0)
		rte_exit(EXIT_FAILURE, "Invalid EAL arguments\n");
	argc -= ret;
	argv += ret;

	force_quit = false;
	signal(SIGINT, signal_handler);
	signal(SIGTERM, signal_handler);

	/* parse application arguments (after the EAL ones) */
	ret = l2fwd_parse_args(argc, argv);
	if (ret < 0)
		rte_exit(EXIT_FAILURE, "Invalid L2FWD arguments\n");

	printf("MAC updating %s\n", mac_updating ? "enabled" : "disabled");

	/* convert to number of cycles */
	timer_period *= rte_get_timer_hz();

	nb_ports = rte_eth_dev_count_avail();
	if (nb_ports == 0)
		rte_exit(EXIT_FAILURE, "No Ethernet ports - bye\n");

	if (port_pair_params != NULL) {
		if (check_port_pair_config() < 0)
			rte_exit(EXIT_FAILURE, "Invalid port pair config\n");
	}

	/* check port mask to possible port mask */
	if (l2fwd_enabled_port_mask & ~((1 << nb_ports) - 1))
		rte_exit(EXIT_FAILURE, "Invalid portmask; possible (0x%x)\n",
			(1 << nb_ports) - 1);

	/* reset l2fwd_dst_ports */
	for (portid = 0; portid < RTE_MAX_ETHPORTS; portid++)
		l2fwd_dst_ports[portid] = 0;
	last_port = 0;

	/* populate destination port details */
	if (port_pair_params != NULL) {
		uint16_t idx, p;

		for (idx = 0; idx < (nb_port_pair_params << 1); idx++) {
			p = idx & 1;
			portid = port_pair_params[idx >> 1].port[p];
			l2fwd_dst_ports[portid] =
				port_pair_params[idx >> 1].port[p ^ 1];
		}
	} else {
		RTE_ETH_FOREACH_DEV(portid) {
			/* skip ports that are not enabled */
			if ((l2fwd_enabled_port_mask & (1 << portid)) == 0)
				continue;

			if (nb_ports_in_mask % 2) {
				l2fwd_dst_ports[portid] = last_port;
				l2fwd_dst_ports[last_port] = portid;
			} else {
				last_port = portid;
			}

			nb_ports_in_mask++;
		}
		if (nb_ports_in_mask % 2) {
			printf("Notice: odd number of ports in portmask.\n");
			l2fwd_dst_ports[last_port] = last_port;
		}
	}

	rx_lcore_id = 0;
	qconf = NULL;

	/* Initialize the port/queue configuration of each logical core */
	RTE_ETH_FOREACH_DEV(portid) {
		/* skip ports that are not enabled */
		if ((l2fwd_enabled_port_mask & (1 << portid)) == 0)
			continue;

		/* get the lcore_id for this port */
		while (rte_lcore_is_enabled(rx_lcore_id) == 0 ||
		       lcore_queue_conf[rx_lcore_id].n_rx_port ==
		       l2fwd_rx_queue_per_lcore) {
			rx_lcore_id++;
			if (rx_lcore_id >= RTE_MAX_LCORE)
				rte_exit(EXIT_FAILURE, "Not enough cores\n");
		}

		if (qconf != &lcore_queue_conf[rx_lcore_id]) {
			/* Assigned a new logical core in the loop above. */
			qconf = &lcore_queue_conf[rx_lcore_id];
			nb_lcores++;
		}

		qconf->rx_port_list[qconf->n_rx_port] = portid;
		qconf->n_rx_port++;
		printf("Lcore %u: RX port %u TX port %u\n", rx_lcore_id,
		       portid, portid);
	}

	// nb_mbufs = RTE_MAX(nb_ports * (nb_rxd + nb_txd + MAX_PKT_BURST +
	// 	nb_lcores * MEMPOOL_CACHE_SIZE), 8192U);

    nb_mbufs = 1024U * 2048U * 1U - 1;
    // nb_mbufs = 8192U;
    RTE_LOG(INFO, L2FWD,"nb_mbufs: %u",nb_mbufs);

	// /* create the mbuf pool */
	// l2fwd_pktmbuf_pool = rte_pktmbuf_pool_create("mbuf_pool", nb_mbufs,
	// 	MEMPOOL_CACHE_SIZE, 0, RTE_MBUF_DEFAULT_BUF_SIZE,
	// 	rte_socket_id());
	// if (l2fwd_pktmbuf_pool == NULL)
	// 	rte_exit(EXIT_FAILURE, "Cannot init mbuf pool\n");
    
        /**
    * Create a mbuf pool.
    *
    * This function creates and initializes a packet mbuf pool. It is
    * a wrapper to rte_mempool functions.
    *
    * @param name
    *   The name of the mbuf pool.
    * @param n
    *   The number of elements in the mbuf pool. The optimum size (in terms
    *   of memory usage) for a mempool is when n is a power of two minus one:
    *   n = (2^q - 1).
    * @param cache_size
    *   Size of the per-core object cache. See rte_mempool_create() for
    *   details.
    * @param priv_size
    *   Size of application private are between the rte_mbuf structure
    *   and the data buffer. This value must be aligned to RTE_MBUF_PRIV_ALIGN.
    * @param data_room_size
    *   Size of data buffer in each mbuf, including RTE_PKTMBUF_HEADROOM.
    * @param socket_id
    *   The socket identifier where the memory should be allocated. The
    *   value can be *SOCKET_ID_ANY* if there is no NUMA constraint for the
    *   reserved zone.
    * @return
    *   The pointer to the new allocated mempool, on success. NULL on error
    *   with rte_errno set appropriately. Possible rte_errno values include:
    *    - E_RTE_NO_CONFIG - function could not get pointer to rte_config structure
    *    - E_RTE_SECONDARY - function was called from a secondary process instance
    *    - EINVAL - cache size provided is too large, or priv_size is not aligned.
    *    - ENOSPC - the maximum number of memzones has already been allocated
    *    - EEXIST - a memzone with the same name already exists
    *    - ENOMEM - no appropriate memory area found in which to create memzone
    */

    clone_pktmbuf_pool = rte_pktmbuf_pool_create("CLONEPOOL", 20000, 32, 0, 0, rte_socket_id());
    if (clone_pktmbuf_pool == NULL){
        rte_exit(EXIT_FAILURE, "Cannot init clone mbuf pool\n");
    }else {
        printf("init clone_pktmbuf_pool\n");
    }
        

    /* create the mbuf pool */
    if (USE_DIFFERENT_POOL_FOR_LCORES == 1){
        mempool_num=rte_lcore_count();
        printf("Using different mempool for all the %d lcores\n", mempool_num);
    }else{
        mempool_num=1;
        printf("Using the same mempool for all lcores\n");
    }

    //l2fwd_pktmbuf_pool = malloc(rte_lcore_count()*sizeof(struct rte_mempool*));
	l2fwd_pktmbuf_pool = rte_pktmbuf_pool_create("mbuf_pool", nb_mbufs,
		MEMPOOL_CACHE_SIZE, 0, RTE_MBUF_DEFAULT_BUF_SIZE,
		rte_socket_id());
	if (l2fwd_pktmbuf_pool == NULL)
		rte_exit(EXIT_FAILURE, "Cannot init mbuf pool\n");

	/* Initialise each port */
	RTE_ETH_FOREACH_DEV(portid) {
		struct rte_eth_rxconf rxq_conf;
		struct rte_eth_txconf txq_conf;
		struct rte_eth_conf local_port_conf = port_conf;
		struct rte_eth_dev_info dev_info;

		/* skip ports that are not enabled */
		if ((l2fwd_enabled_port_mask & (1 << portid)) == 0) {
			printf("Skipping disabled port %u\n", portid);
			continue;
		}
		nb_ports_available++;

		/* init port */
		printf("Initializing port %u... ", portid);
		fflush(stdout);

		ret = rte_eth_dev_info_get(portid, &dev_info);
		if (ret != 0)
			rte_exit(EXIT_FAILURE,
				"Error during getting device (port %u) info: %s\n",
				portid, strerror(-ret));

		if (dev_info.tx_offload_capa & DEV_TX_OFFLOAD_MBUF_FAST_FREE)
			local_port_conf.txmode.offloads |=
				DEV_TX_OFFLOAD_MBUF_FAST_FREE;

        /**
        * Configure an Ethernet device.
        * This function must be invoked first before any other function in the
        * Ethernet API. This function can also be re-invoked when a device is in the
        * stopped state.
        *
        * @param port_id
        *   The port identifier of the Ethernet device to configure.
        * @param nb_rx_queue
        *   The number of receive queues to set up for the Ethernet device.
        * @param nb_tx_queue
        *   The number of transmit queues to set up for the Ethernet device.
        * @param eth_conf
        *   The pointer to the configuration data to be used for the Ethernet device.
        *   The *rte_eth_conf* structure includes:
        *     -  the hardware offload features to activate, with dedicated fields for
        *        each statically configurable offload hardware feature provided by
        *        Ethernet devices, such as IP checksum or VLAN tag stripping for
        *        example.
        *        The Rx offload bitfield API is obsolete and will be deprecated.
        *        Applications should set the ignore_bitfield_offloads bit on *rxmode*
        *        structure and use offloads field to set per-port offloads instead.
        *     -  Any offloading set in eth_conf->[rt]xmode.offloads must be within
        *        the [rt]x_offload_capa returned from rte_eth_dev_info_get().
        *        Any type of device supported offloading set in the input argument
        *        eth_conf->[rt]xmode.offloads to rte_eth_dev_configure() is enabled
        *        on all queues and it can't be disabled in rte_eth_[rt]x_queue_setup()
        *     -  the Receive Side Scaling (RSS) configuration when using multiple RX
        *        queues per port. Any RSS hash function set in eth_conf->rss_conf.rss_hf
        *        must be within the flow_type_rss_offloads provided by drivers via
        *        rte_eth_dev_info_get() API.
        *
        *   Embedding all configuration information in a single data structure
        *   is the more flexible method that allows the addition of new features
        *   without changing the syntax of the API.
        * @return
        *   - 0: Success, device configured.
        *   - <0: Error code returned by the driver configuration function.
        **/

        ////////////////////////
		ret = rte_eth_dev_configure(portid, 1, 1, &local_port_conf);
		if (ret < 0)
			rte_exit(EXIT_FAILURE, "Cannot configure device: err=%d, port=%u\n",
				  ret, portid);
        ////////////////////

		ret = rte_eth_dev_adjust_nb_rx_tx_desc(portid, &nb_rxd,
						       &nb_txd);
		if (ret < 0)
			rte_exit(EXIT_FAILURE,
				 "Cannot adjust number of descriptors: err=%d, port=%u\n",
				 ret, portid);

		ret = rte_eth_macaddr_get(portid,
					  &l2fwd_ports_eth_addr[portid]);
		if (ret < 0)
			rte_exit(EXIT_FAILURE,
				 "Cannot get MAC address: err=%d, port=%u\n",
				 ret, portid);

		/* init one RX queue */
		fflush(stdout);
		rxq_conf = dev_info.default_rxconf;
		rxq_conf.offloads = local_port_conf.rxmode.offloads;
		ret = rte_eth_rx_queue_setup(portid, 0, nb_rxd,
					     rte_eth_dev_socket_id(portid),
					     &rxq_conf,
					     l2fwd_pktmbuf_pool);
		if (ret < 0)
			rte_exit(EXIT_FAILURE, "rte_eth_rx_queue_setup:err=%d, port=%u\n",
				  ret, portid);


        /**
        * Allocate and set up a transmit queue for an Ethernet device.
        *
        * @param port_id
        *   The port identifier of the Ethernet device.
        * @param tx_queue_id
        *   The index of the transmit queue to set up.
        *   The value must be in the range [0, nb_tx_queue - 1] previously supplied
        *   to rte_eth_dev_configure().
        * @param nb_tx_desc
        *   The number of transmit descriptors to allocate for the transmit ring.
        * @param socket_id
        *   The *socket_id* argument is the socket identifier in case of NUMA.
        *   Its value can be *SOCKET_ID_ANY* if there is no NUMA constraint for
        *   the DMA memory allocated for the transmit descriptors of the ring.
        * @param tx_conf
        *   The pointer to the configuration data to be used for the transmit queue.
        *   NULL value is allowed, in which case default TX configuration
        *   will be used.
        *   The *tx_conf* structure contains the following data:
        *   - The *tx_thresh* structure with the values of the Prefetch, Host, and
        *     Write-Back threshold registers of the transmit ring.
        *     When setting Write-Back threshold to the value greater then zero,
        *     *tx_rs_thresh* value should be explicitly set to one.
        *   - The *tx_free_thresh* value indicates the [minimum] number of network
        *     buffers that must be pending in the transmit ring to trigger their
        *     [implicit] freeing by the driver transmit function.
        *   - The *tx_rs_thresh* value indicates the [minimum] number of transmit
        *     descriptors that must be pending in the transmit ring before setting the
        *     RS bit on a descriptor by the driver transmit function.
        *     The *tx_rs_thresh* value should be less or equal then
        *     *tx_free_thresh* value, and both of them should be less then
        *     *nb_tx_desc* - 3.
        *   - The *offloads* member contains Tx offloads to be enabled.
        *     If an offloading set in tx_conf->offloads
        *     hasn't been set in the input argument eth_conf->txmode.offloads
        *     to rte_eth_dev_configure(), it is a new added offloading, it must be
        *     per-queue type and it is enabled for the queue.
        *     No need to repeat any bit in tx_conf->offloads which has already been
        *     enabled in rte_eth_dev_configure() at port level. An offloading enabled
        *     at port level can't be disabled at queue level.
        *
        *     Note that setting *tx_free_thresh* or *tx_rs_thresh* value to 0 forces
        *     the transmit function to use default values.
        * @return
        *   - 0: Success, the transmit queue is correctly set up.
        *   - -ENOMEM: Unable to allocate the transmit ring descriptors.
        */
 
		/* init one TX queue on each port */
		fflush(stdout);
		txq_conf = dev_info.default_txconf;
		txq_conf.offloads = local_port_conf.txmode.offloads;
		ret = rte_eth_tx_queue_setup(portid, 0, nb_txd,
				rte_eth_dev_socket_id(portid),
				&txq_conf);
		if (ret < 0)
			rte_exit(EXIT_FAILURE, "rte_eth_tx_queue_setup:err=%d, port=%u\n",
				ret, portid);

		/* Initialize TX buffers */
		tx_buffer[portid] = rte_zmalloc_socket("tx_buffer",
				RTE_ETH_TX_BUFFER_SIZE(MAX_PKT_BURST), 0,
				rte_eth_dev_socket_id(portid));
		if (tx_buffer[portid] == NULL)
			rte_exit(EXIT_FAILURE, "Cannot allocate buffer for tx on port %u\n",
					portid);

		rte_eth_tx_buffer_init(tx_buffer[portid], MAX_PKT_BURST);

		ret = rte_eth_tx_buffer_set_err_callback(tx_buffer[portid],
				rte_eth_tx_buffer_count_callback,
				&port_statistics[portid].dropped);
		if (ret < 0)
			rte_exit(EXIT_FAILURE,
			"Cannot set error callback for tx buffer on port %u\n",
				 portid);

		ret = rte_eth_dev_set_ptypes(portid, RTE_PTYPE_UNKNOWN, NULL,
					     0);
		if (ret < 0)
			printf("Port %u, Failed to disable Ptype parsing\n",
					portid);
		/* Start device */
		ret = rte_eth_dev_start(portid);
		if (ret < 0)
			rte_exit(EXIT_FAILURE, "rte_eth_dev_start:err=%d, port=%u\n",
				  ret, portid);

		printf("done: \n");

		ret = rte_eth_promiscuous_enable(portid);
		if (ret != 0)
			rte_exit(EXIT_FAILURE,
				 "rte_eth_promiscuous_enable:err=%s, port=%u\n",
				 rte_strerror(-ret), portid);

		printf("Port %u, MAC address: %02X:%02X:%02X:%02X:%02X:%02X\n\n",
				portid,
				l2fwd_ports_eth_addr[portid].addr_bytes[0],
				l2fwd_ports_eth_addr[portid].addr_bytes[1],
				l2fwd_ports_eth_addr[portid].addr_bytes[2],
				l2fwd_ports_eth_addr[portid].addr_bytes[3],
				l2fwd_ports_eth_addr[portid].addr_bytes[4],
				l2fwd_ports_eth_addr[portid].addr_bytes[5]);

		/* initialize port stats */
		memset(&port_statistics, 0, sizeof(port_statistics));
	}

	if (!nb_ports_available) {
		rte_exit(EXIT_FAILURE,
			"All available ports are disabled. Please set portmask.\n");
	}


    // //Create Hash table
    // char tmp[50];
    // ut_params.name = tmp;
    // //ut_params.socket_id = socketid;
				
    // buffer_table = rte_hash_create(&ut_params);
    // if (buffer_table == NULL) {
    //     printf("UNABLE TO CREATE HASHTABLE\n");
    //     rte_exit(EXIT_FAILURE, "UNABLE TO CREATE HASHTABLE\n");
    // }
    // buffer_table_teid = rte_hash_create(&ut_params_teid);
    // if (buffer_table_teid == NULL) {
    //     printf("UNABLE TO CREATE HASHTABLE\n");
    //     rte_exit(EXIT_FAILURE, "UNABLE TO CREATE HASHTABLE\n");
        
    //}

	/* init memory */
	ret = init_mem();
	if (ret < 0)
		rte_exit(EXIT_FAILURE, "init_mem failed :err=%d\n", ret);


	check_all_ports_link_status(l2fwd_enabled_port_mask);
	ret = 0;
	/* launch per-lcore init on every lcore */
	rte_eal_mp_remote_launch(l2fwd_launch_one_lcore, NULL, CALL_MASTER);
	RTE_LCORE_FOREACH_SLAVE(lcore_id) {
		if (rte_eal_wait_lcore(lcore_id) < 0) {
			ret = -1;
			break;
		}
	}

	RTE_ETH_FOREACH_DEV(portid) {
		if ((l2fwd_enabled_port_mask & (1 << portid)) == 0)
			continue;
		printf("Closing port %d...", portid);
		rte_eth_dev_stop(portid);
		rte_eth_dev_close(portid);
		printf(" Done\n");
	}
	printf("Bye...\n");

	return ret;
}
