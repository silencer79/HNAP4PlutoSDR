/*
 * HNAP4PlutoSDR - HAMNET Access Protocol implementation for the Adalm Pluto SDR
 *
 * Copyright (C) 2020 Lukas Ostendorf <lukas.ostendorf@gmail.com>
 *                    and the project contributors
 *
 * This library is free software; you can redistribute it and/or modify it under the terms of the
 * GNU Lesser General Public License as published by the Free Software Foundation; version 3.0.
 *
 * This library is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY;
 * without even the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.
 * See the GNU Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public License along with this library;
 * if not, write to the Free Software Foundation, Inc., 51 Franklin Street, Fifth Floor,
 * Boston, MA 02110-1301 USA
 */

#define _GNU_SOURCE


#include "../mac/mac_bs.h"
#include "../phy/phy_bs.h"
#include "../phy/phy_config.h"
#include "../platform/pluto.h"
#include "../platform/platform_simulation.h"
#include "../util/log.h"

#include <pthread.h>
#include <time.h>
#include <sched.h>
#include <unistd.h>
#include <getopt.h>

// Set to 1 in order to use the simulated platform
#define BS_USE_PLATFORM_SIM 0

// set to one if the BS shall send random MAC data frames
#define BS_SEND_ENABLE 0

// FPGA sample buffers will contain multiple ofdm symbols. We use 2 symbols per buffer
#define SYMBOLS_PER_BUF 2
int buflen;             // size of the fpga transfer buffers

// compensate for offset within a symbol in samples
// is used to properly align UL and DL over the air and compensate for FIR delays
#define INTER_SYMB_OFFSET 0

// Configure CPU core affinities for the threads
#define BS_RX_SLOT_CPUID 0
#define BS_RX_CPUID 1
#define BS_TX_CPUID 1
#define BS_MAC_CPUID 0
#define BS_TAP_CPUID 0

// program options
struct option Options[] = {
  {"rxgain",required_argument,NULL,'g'},
  {"txgain",required_argument,NULL,'t'},
  {"frequency",required_argument,NULL,'f'},
  {"config",required_argument,NULL,'c'},
  {"log",required_argument,NULL,'l'},
  {"help",no_argument,NULL,'h'},
  {NULL},
};
char* helpstring = "Basestation for 70cm Waveform.\n\n \
Options:\n \
   --rxgain -g:    fix the rxgain to a value [-1 73]\n \
   --txgain -t:    fix the txgain to a value [-89 0]\n \
   --frequency -f: tune to a specific (DL) frequency\n \
   --config -c     specify a configuration file\n \
   --log -l        specify the log level. Default: 2.\n \
                   0=TRACE 1=DEBUG 2=INFO 3=WARN 4=ERR 5=NONE\n";

extern char *optarg;
int rxgain = 70;
int txgain = 0;
long long int frequency = -1;
char* config_file=NULL;

// struct holds arguments for RX thread
struct rx_th_data_s {
	PhyBS phy;
	platform hw;
	pthread_barrier_t* thread_sync;
};

// struct holds arguments for TX thread
struct tx_th_data_s {
	PhyBS phy;
	platform hw;
	pthread_cond_t* scheduler_signal;
	pthread_barrier_t* thread_sync;
};

// struct holds arguments for MAC thread
struct mac_th_data_s {
	pthread_cond_t* scheduler_signal;
	pthread_mutex_t* scheduler_mutex;
	MacBS mac;
};

// struct with args for RX slot thread
struct rx_slot_th_data_s {
    PhyBS phy;
    pthread_cond_t* rx_slot_signal;
    pthread_mutex_t* rx_slot_mutex;
};

// Main Thread for BS receive
void* thread_phy_bs_rx(void* arg)
{
	platform hw = ((struct rx_th_data_s*)arg)->hw;
	PhyBS phy = ((struct rx_th_data_s*)arg)->phy;
	pthread_barrier_t* rx_tx_sync = ((struct rx_th_data_s*)arg)->thread_sync;
    TIMECHECK_CREATE(timecheck_bs_rx);
    TIMECHECK_INIT(timecheck_bs_rx,"bs.rx_buffer",10000);

	float complex* rxbuf_time = calloc(sizeof(float complex),buflen);

	// read some rxbuffer objects in order to empty rxbuffer queue
	pthread_barrier_wait(rx_tx_sync);
	sleep(1); // wait until buffer filled
	for (int i=0; i<KERNEL_BUF_RX+1; i++)
		hw->platform_rx(hw, rxbuf_time);

	pthread_barrier_wait(rx_tx_sync);
	LOG(INFO,"RX thread started: RX symbol %d. TX symbol %d\n",phy->common->rx_symbol,phy->common->tx_symbol);
	while (1)
	{
		hw->platform_rx(hw, rxbuf_time);
		TIMECHECK_START(timecheck_bs_rx);
		phy_bs_rx_symbol(phy, rxbuf_time);
		phy_bs_rx_symbol(phy, rxbuf_time+(nfft+cp_len));
		TIMECHECK_STOP_CHECK(timecheck_bs_rx,530);
		//TIMECHECK_INFO(timecheck_bs_rx);
	}
	return NULL;
}

void* thread_phy_bs_rx_slot(void* arg)
{
    PhyBS phy = ((struct rx_slot_th_data_s*)arg)->phy;
    pthread_cond_t* cond_signal = ((struct rx_slot_th_data_s*)arg)->rx_slot_signal;
    pthread_mutex_t* mutex = ((struct rx_slot_th_data_s*)arg)->rx_slot_mutex;
    TIMECHECK_CREATE(timecheck_bs_rx_slot);
    TIMECHECK_INIT(timecheck_bs_rx_slot,"bs.rx_slot",1000);

    while (1) {
        // Wait for signal from UE rx thread
        pthread_mutex_lock(mutex);
        pthread_cond_wait(cond_signal, mutex);
        TIMECHECK_START(timecheck_bs_rx_slot);

        phy_bs_proc_slot(phy,phy->rx_slot_nr);

        pthread_mutex_unlock(mutex);
        TIMECHECK_STOP_CHECK(timecheck_bs_rx_slot,3500);
        TIMECHECK_INFO(timecheck_bs_rx_slot);
    }
    return NULL;
}

// Main Thread for BS transmit
void* thread_phy_bs_tx(void* arg)
{
	PhyBS phy = ((struct tx_th_data_s*)arg)->phy;
	platform bs = ((struct tx_th_data_s*)arg)->hw;
	pthread_cond_t* scheduler_signal = ((struct tx_th_data_s*)arg)->scheduler_signal;
	pthread_barrier_t* tx_rx_sync = ((struct tx_th_data_s*)arg)->thread_sync;
	uint subframe_cnt = 0;
    TIMECHECK_CREATE(timecheck_bs_tx);
    TIMECHECK_INIT(timecheck_bs_tx,"bs.tx_buffer",10000);

	float complex* txbuf_time = calloc(sizeof(float complex),buflen);

	// generate some txbuffers in order to keep the txbuffer queue full
	bs->platform_tx_prep(bs, txbuf_time, 0, buflen);
	pthread_barrier_wait(tx_rx_sync);
	sleep(1); // wait until buffer emptied
	for (int i=0; i<KERNEL_BUF_TX+1; i++)
		bs->platform_tx_push(bs);

	pthread_barrier_wait(tx_rx_sync);
	LOG(INFO,"TX thread started: RX symbol %d. TX symbol %d\n",phy->common->rx_symbol,phy->common->tx_symbol);
	while (1)
	{
	    LOG(TRACE,"[TX Thread] start subframe %d\n",subframe_cnt);
		for (int symbol=0; symbol<SUBFRAME_LEN/2; symbol++) {
			bs->platform_tx_push(bs);
			bs->platform_tx_prep(bs, txbuf_time+buflen-INTER_SYMB_OFFSET, 0, INTER_SYMB_OFFSET);
            TIMECHECK_START(timecheck_bs_tx);
			phy_bs_write_symbol(phy, txbuf_time);
			phy_bs_write_symbol(phy, txbuf_time+1*(nfft+cp_len));

			bs->platform_tx_prep(bs, txbuf_time, INTER_SYMB_OFFSET, buflen-INTER_SYMB_OFFSET);
            // run scheduler. TODO tweak signaling time: after ULCTRL is received, but early enough to finish
            if (symbol==23) {
				pthread_cond_signal(scheduler_signal);
			}
            TIMECHECK_STOP_CHECK(timecheck_bs_tx,530);
            //TIMECHECK_INFO(timecheck_bs_tx);

		} // end{for}
		subframe_cnt++;
	}
	return NULL;
}

void* thread_mac_bs_scheduler(void* arg)
{
	MacBS mac = ((struct mac_th_data_s*)arg)->mac;
	pthread_cond_t* cond_signal = ((struct mac_th_data_s*)arg)->scheduler_signal;
	pthread_mutex_t* mutex = ((struct mac_th_data_s*)arg)->scheduler_mutex;
    TIMECHECK_CREATE(timecheck_mac_bs);
	TIMECHECK_INIT(timecheck_mac_bs,"bs.mac_scheduler",1000);
	uint subframe_cnt = 0;

	while (1) {
		// Wait for signal from BS tx thread
		pthread_mutex_lock(mutex);
		pthread_cond_wait(cond_signal, mutex);
        TIMECHECK_START(timecheck_mac_bs);
		// add some data to send
#if BS_SEND_ENABLE
		uint payload_len = 200;
		MacDataFrame dl_frame = dataframe_create(payload_len);
		for (int i=0; i<payload_len; i++)
			dl_frame->data[i] = rand() & 0xFF;
		memcpy(dl_frame->data,&subframe_cnt,sizeof(uint));
		if(!mac_bs_add_txdata(mac, 2, dl_frame)) {
			dataframe_destroy(dl_frame);
		}
#endif
		mac_bs_run_scheduler(mac);
		subframe_cnt++;
        TIMECHECK_STOP_CHECK(timecheck_mac_bs,3500);
        TIMECHECK_INFO(timecheck_mac_bs);
        pthread_mutex_unlock(mutex);
    }
	return NULL;
}

int main(int argc,char *argv[])
{
	pthread_t bs_phy_rx_slot_th, bs_phy_rx_th, bs_phy_tx_th, bs_mac_th, bs_tap_th;

	// load default configuration
	phy_config_default_64();

    // parse program args
    int d;
    while((d = getopt_long(argc,argv,"g:t:f:c:l:h",Options,NULL)) != EOF){
        switch(d){
        case 'g':
            rxgain = atoi(optarg);
            if (rxgain < RXGAIN_MIN || rxgain > RXGAIN_MAX) {
                printf ("Error: rxgain %d out of range [-1 73]!\n",rxgain);
                exit(EXIT_FAILURE);
            }
            break;
        case 't':
            txgain = atoi(optarg);
            if (txgain < TXGAIN_MIN || txgain > TXGAIN_MAX) {
                printf ("Error: txgain %d out of range [-89 0]!\n",txgain);
                exit(EXIT_FAILURE);
            }
            break;
        case 'f':
            frequency = atoi(optarg);
            break;
        case 'c':
            phy_config_load_file(optarg);
            config_file = calloc(strlen(optarg),1);
            strcpy(config_file,optarg);
            break;
        case 'l':
            global_log_level = atoi(optarg);
            if (global_log_level<TRACE || global_log_level>NONE) {
                printf("ERROR: log level %d undefined!\n",global_log_level);
                exit(EXIT_FAILURE);
            }
            break;
        case 'h':
            printf("%s",helpstring);
            exit(0);
            break;
        default:
            printf("%s",helpstring);
            exit(0);
        }
    }
    // set buffer size
    buflen = SYMBOLS_PER_BUF*(nfft+cp_len);

    // configure frequency, if user specified parameter
    if (frequency>0) {
        ul_lo = ul_lo + frequency - dl_lo;
        dl_lo = frequency;
    }
    // print system config
    phy_config_print();

	// Init platform
#if BS_USE_PLATFORM_SIM
	platform pluto = platform_init_simulation(BUFLEN);
#else
	platform pluto = init_pluto_platform(buflen, config_file);
    pluto_set_rxgain(pluto, rxgain);
    pluto_set_txgain(pluto, txgain);
    pluto_set_tx_freq(pluto, dl_lo);
    pluto_set_rx_freq(pluto, ul_lo);
#endif
    printf("Pluto config: rxgain %d txgain %d DL_LO %lldHz UL_LO %lldHz\n",rxgain,txgain,dl_lo,ul_lo);

    // Init phy and mac layer
	PhyBS phy = phy_bs_init();
	MacBS mac = mac_bs_init();
	phy->rxgain = rxgain;
	phy->txgain = txgain;

	phy_bs_set_mac_interface(phy, mac);
	mac_bs_set_phy_interface(mac, phy);

	//rx and tx threads will be synchronized by a barrier
	pthread_barrier_t sync_barrier;
	pthread_barrier_init(&sync_barrier, NULL, 2);

	// create arguments for MAC scheduler thread
	pthread_mutex_t mutex = PTHREAD_MUTEX_INITIALIZER;
	pthread_cond_t cond = PTHREAD_COND_INITIALIZER;
	struct mac_th_data_s mac_th_data;
	mac_th_data.mac = mac;
	mac_th_data.scheduler_mutex = &mutex;
	mac_th_data.scheduler_signal = &cond;

	// create arguments for RX thread
	struct rx_th_data_s rx_th_data;
	rx_th_data.hw = pluto;
	rx_th_data.phy = phy;
	rx_th_data.thread_sync = &sync_barrier;


	// create arguments for TX thread
	struct tx_th_data_s tx_th_data;
	tx_th_data.hw = pluto;
	tx_th_data.phy = phy;
	tx_th_data.scheduler_signal = &cond;
	tx_th_data.thread_sync = &sync_barrier;

    // create arguments for RX slot thread
    pthread_cond_t rx_slot_cond = PTHREAD_COND_INITIALIZER;
    pthread_mutex_t rx_slot_mutex = PTHREAD_MUTEX_INITIALIZER;
    struct rx_slot_th_data_s rx_slot_th_data;
    rx_slot_th_data.phy = phy;
    rx_slot_th_data.rx_slot_mutex = &rx_slot_mutex;
    rx_slot_th_data.rx_slot_signal = &rx_slot_cond;
    phy_bs_set_rx_slot_th_signal(phy,&rx_slot_cond);

    // start RX slot thread
    if (pthread_create(&bs_phy_rx_slot_th, NULL, thread_phy_bs_rx_slot, &rx_slot_th_data) != 0) {
        LOG(ERR,"could not create RX slot processing thread. Abort!\n");
        exit(EXIT_FAILURE);
    } else {
        LOG(INFO,"created RX slot processing thread.\n");
    }
    cpu_set_t cpu_set;
    CPU_ZERO(&cpu_set);
    CPU_SET(BS_RX_SLOT_CPUID,&cpu_set);
    struct sched_param prio_rt_high, prio_rt_normal;
    prio_rt_high.sched_priority = 2;
    prio_rt_normal.sched_priority = 1;
    pthread_setaffinity_np(bs_phy_rx_slot_th,sizeof(cpu_set_t),&cpu_set);
    pthread_setschedparam(bs_phy_rx_slot_th, SCHED_FIFO, &prio_rt_normal);

    // start RX thread
	if (pthread_create(&bs_phy_rx_th, NULL, thread_phy_bs_rx, &rx_th_data) !=0) {
		LOG(ERR,"could not create RX thread. Abort!\n");
		exit(EXIT_FAILURE);
	} else {
		LOG(INFO,"created RX thread.\n");
	}
	CPU_ZERO(&cpu_set);
	CPU_SET(BS_RX_CPUID,&cpu_set);
	pthread_setaffinity_np(bs_phy_rx_th,sizeof(cpu_set_t),&cpu_set);
    pthread_setschedparam(bs_phy_rx_th, SCHED_FIFO, &prio_rt_high);

	// start TX thread
	if (pthread_create(&bs_phy_tx_th, NULL, thread_phy_bs_tx, &tx_th_data) !=0) {
		LOG(ERR,"could not create TX thread. Abort!\n");
		exit(EXIT_FAILURE);
	} else {
		LOG(INFO,"created TX thread.\n");
	}
	CPU_ZERO(&cpu_set);
	CPU_SET(BS_TX_CPUID,&cpu_set);
	pthread_setaffinity_np(bs_phy_tx_th,sizeof(cpu_set_t),&cpu_set);
    pthread_setschedparam(bs_phy_tx_th, SCHED_FIFO, &prio_rt_high);

	// start MAC thread
	if (pthread_create(&bs_mac_th, NULL, thread_mac_bs_scheduler, &mac_th_data) !=0) {
		LOG(ERR,"could not create MAC thread. Abort!\n");
		exit(EXIT_FAILURE);
	} else {
		LOG(INFO,"created MAC thread.\n");
	}
	CPU_ZERO(&cpu_set);
	CPU_SET(BS_MAC_CPUID,&cpu_set);
	pthread_setaffinity_np(bs_mac_th,sizeof(cpu_set_t),&cpu_set);
    pthread_setschedparam(bs_mac_th, SCHED_FIFO, &prio_rt_high);

	// start TAP receiver thread
	if (pthread_create(&bs_tap_th, NULL, mac_bs_tap_rx_th, mac) !=0) {
		LOG(ERR,"could not create TAP receive thread. Abort!\n");
		exit(EXIT_FAILURE);
	} else {
		LOG(INFO,"created TAP thread.\n");
	}
	CPU_ZERO(&cpu_set);
	CPU_SET(BS_TAP_CPUID,&cpu_set);
    pthread_setaffinity_np(bs_tap_th,sizeof(cpu_set_t),&cpu_set);
    //pthread_setschedparam(bs_tap_th, SCHED_FIFO, &prio_rt_normal);

	// printf affinities
	pthread_getaffinity_np(bs_phy_rx_th,sizeof(cpu_set_t),&cpu_set);
	printf("RX Thread CPU mask: ");
	for (int i=0; i<4; i++)
		printf("%d ",CPU_ISSET(i, &cpu_set));

	pthread_getaffinity_np(bs_phy_tx_th,sizeof(cpu_set_t),&cpu_set);
	printf("\nTX Thread CPU mask: ");
	for (int i=0; i<4; i++)
		printf("%d ",CPU_ISSET(i, &cpu_set));

	pthread_getaffinity_np(bs_mac_th,sizeof(cpu_set_t),&cpu_set);
	printf("\nMAC Thread CPU mask: ");
	for (int i=0; i<4; i++)
		printf("%d ",CPU_ISSET(i, &cpu_set));

    pthread_getaffinity_np(bs_phy_rx_slot_th,sizeof(cpu_set_t),&cpu_set);
    printf("\nRX slot Thread CPU mask: ");
    for (int i=0; i<4; i++)
        printf("%d ",CPU_ISSET(i, &cpu_set));
	printf("\n");

    // main thread: regularly show statistics:
    char stats_buf[512];
    while (1) {
        sleep(60);
        int num_user = 0;
        for (int userid=0; userid<MAX_USER;userid++) {
            if (mac->UE[userid] != NULL) {
                num_user++;
                LOG(INFO, "User %2d stats:\n", userid);
                SYSLOG(LOG_INFO, "User %2d stats:\n", userid);
                mac_stats_print(stats_buf, 512, &mac->UE[userid]->stats);
                LOG(INFO, "%s", stats_buf);
                SYSLOG(LOG_INFO, "%s", stats_buf);
                LOG(INFO, "UL mcs %d DL mcs %d\n", mac->UE[userid]->ul_mcs, mac->UE[userid]->dl_mcs);
                SYSLOG(LOG_INFO, "UL mcs %d DL mcs %d\n", mac->UE[userid]->ul_mcs, mac->UE[userid]->dl_mcs);
            }
        }
        LOG(INFO,"Num connected users: %d\n",num_user);
        SYSLOG(LOG_INFO,"Num connected users: %d\n",num_user);
    }

	static void* ret[4];
	pthread_join(bs_phy_rx_th, (void*)&ret[0]);
    pthread_join(bs_phy_tx_th, (void*)&ret[1]);
    pthread_join(bs_phy_rx_slot_th, (void*)&ret[2]);
	pthread_join(bs_mac_th, &ret[3]);

}
