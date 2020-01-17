/*
 * phy_common.c
 *
 *  Created on: Nov 12, 2019
 *      Author: lukas
 */

#include "phy_common.h"


// Init the PHY instance
PhyCommon phy_init_common()
{
	PhyCommon phy = malloc(sizeof(PhyCommon_s));

	// TX buffer has 2 entries for even/uneven subframes
	phy->txdata_f = malloc(sizeof(float complex**)*2);

    // alloc buffer for one subframe of symbols in frequency domain
	phy->txdata_f[0] = malloc(sizeof(float complex*)*SUBFRAME_LEN);
	phy->txdata_f[1] = malloc(sizeof(float complex*)*SUBFRAME_LEN);
	phy->rxdata_f    = malloc(sizeof(float complex*)*SUBFRAME_LEN);
    for (int i=0; i<SUBFRAME_LEN; i++) {
    	phy->txdata_f[0][i] = malloc(sizeof(float complex)*(NFFT));
    	phy->txdata_f[1][i] = malloc(sizeof(float complex)*(NFFT));
    	phy->rxdata_f[i]    = malloc(sizeof(float complex)*(NFFT));
    }

    // alloc buffer for subcarrier definitions
    phy->pilot_sc = malloc(NFFT);
    phy->pilot_symbols = malloc(SUBFRAME_LEN);
    gen_sc_alloc(phy);

    // init modulator objects
    phy->mcs_modem[0] = modem_create(LIQUID_MODEM_QPSK);
    phy->mcs_modem[1] = modem_create(LIQUID_MODEM_QPSK);
    phy->mcs_modem[2] = modem_create(LIQUID_MODEM_QAM16);
    phy->mcs_modem[3] = modem_create(LIQUID_MODEM_QAM64);
    phy->mcs_modem[4] = modem_create(LIQUID_MODEM_QAM64);

    // init FEC modules
    phy->mcs_fec[0] = fec_create(LIQUID_FEC_CONV_V27, NULL);
    phy->mcs_fec[1] = fec_create(LIQUID_FEC_CONV_V27P34, NULL);
    phy->mcs_fec[2] = fec_create(LIQUID_FEC_CONV_V27, NULL);
    phy->mcs_fec[3] = fec_create(LIQUID_FEC_CONV_V27, NULL);
    phy->mcs_fec[4] = fec_create(LIQUID_FEC_CONV_V27P34, NULL);

    phy->mcs_fec_scheme[0] = LIQUID_FEC_CONV_V27;
    phy->mcs_fec_scheme[1] = LIQUID_FEC_CONV_V27P34;
    phy->mcs_fec_scheme[2] = LIQUID_FEC_CONV_V27;
    phy->mcs_fec_scheme[3] = LIQUID_FEC_CONV_V27;
    phy->mcs_fec_scheme[4] = LIQUID_FEC_CONV_V27P34;


    // init subframe number and rx symbol nr
    phy->rx_subframe = 0;
    phy->rx_symbol = 0;
    phy->tx_subframe = 0;
    phy->tx_symbol = 0;

    // init the interleaver
    phy->mcs_interlvr[0] = interleaver_create(get_tbs_size(phy,0)/8);
    phy->mcs_interlvr[1] = interleaver_create(get_tbs_size(phy,1)/8);
    phy->mcs_interlvr[2] = interleaver_create(get_tbs_size(phy,2)/8);
    phy->mcs_interlvr[3] = interleaver_create(get_tbs_size(phy,3)/8);
    phy->mcs_interlvr[4] = interleaver_create(get_tbs_size(phy,4)/8);

    return phy;
}

void phy_destroy_common(PhyCommon phy)
{
    // free buffer for symbols in frequency domain
    for (int i=0; i<SUBFRAME_LEN; i++) {
    	free(phy->txdata_f[i]);
    	free(phy->rxdata_f[i]);
    }
	free(phy->txdata_f);
	free(phy->rxdata_f);

    // free buffer for subcarrier definitions
    free(phy->pilot_sc);
    free(phy->pilot_symbols);

    // delete modulator objects
    modem_destroy(phy->mcs_modem[0]);
    modem_destroy(phy->mcs_modem[1]);
    modem_destroy(phy->mcs_modem[2]);

    // delete FEC modules
    fec_destroy(phy->mcs_fec[0]);
    fec_destroy(phy->mcs_fec[1]);
    fec_destroy(phy->mcs_fec[2]);

    // delete the interleaver
    interleaver_destroy(phy->mcs_interlvr[0]);
    interleaver_destroy(phy->mcs_interlvr[1]);
    interleaver_destroy(phy->mcs_interlvr[2]);
    interleaver_destroy(phy->mcs_interlvr[3]);
}


// returns the Transport Block size of a UL/DL data slot in bits
int get_tbs_size(PhyCommon phy, uint mcs)
{
	if (NFFT==64) {
		uint symbols = (SLOT_LEN-1)*(NUM_DATA_SC+NUM_PILOT)+NUM_DATA_SC;
		uint enc_bits = symbols*modem_get_bps(phy->mcs_modem[mcs]); //number of encoded bits
		return (enc_bits-16)*fec_get_rate(phy->mcs_fec_scheme[mcs]); // real tbs size. Subtract 16bit for conv encoding
	} else {
		printf("[PHY common] fft size not yet implemented\n");
		return -1;
	}
}

// returns the size of the ULCTRL slots in bits
int get_ulctrl_slot_size(PhyCommon phy)
{
	uint symbols = NUM_DATA_SC;
	uint enc_bits = symbols*modem_get_bps(phy->mcs_modem[0]); //number of encoded bits
	return (enc_bits-16)*fec_get_rate(phy->mcs_fec_scheme[0]); // real tbs size. Subtract 16bit for conv encoding

}
// Modulate the given data to the frequency domain data of the Phy object
// returns the number of symbols that have been generated
void phy_mod(PhyCommon common, uint first_sc, uint last_sc, uint first_symb, uint last_symb,
			 uint mcs, uint8_t* data, uint buf_len, uint* written_samps)
{
	uint sfn = common->tx_subframe % 2;
	*written_samps = 0;
	for (int sym_idx=first_symb; sym_idx<=last_symb; sym_idx++) {
		for (int i=first_sc; i<=last_sc; i++) {
			if ((common->pilot_symbols[sym_idx] == NO_PILOT && !(common->pilot_sc[i] == OFDMFRAME_SCTYPE_NULL)) ||
			    (common->pilot_sc[i] == OFDMFRAME_SCTYPE_DATA)) {
				modem_modulate(common->mcs_modem[mcs],(uint)data[(*written_samps)++], &common->txdata_f[sfn][sym_idx][i]);
				if (*written_samps >= buf_len) {
					return;
				}
			}
		}
	}
}

// Symbol demapper with soft decision
// returns an array with n llr values for each demapped symbol and the number of demapped bits
void phy_demod_soft(PhyCommon common, uint first_sc, uint last_sc, uint first_symb, uint last_symb,
					uint mcs, uint8_t* llr, uint num_llr, uint* written_samps)
{
	*written_samps = 0;
	uint bps = modem_get_bps(common->mcs_modem[mcs]);

	// demodulate signal
	uint symbol = 0;
	for (int sym_idx=first_symb; sym_idx<=last_symb; sym_idx++) {
		for (int i=first_sc; i<=last_sc; i++) {
			if ((common->pilot_symbols[sym_idx] == NO_PILOT && !(common->pilot_sc[i] == OFDMFRAME_SCTYPE_NULL)) ||
			    (common->pilot_sc[i] == OFDMFRAME_SCTYPE_DATA)) {
				modem_demodulate_soft(common->mcs_modem[mcs], common->rxdata_f[sym_idx][i], &symbol, &llr[*written_samps]);
				*written_samps+=bps;
				if (*written_samps+bps >= num_llr) {
					return;
				}
			}
		}
	}
}

// generate the allocation info for the subcarriers and OFDM symbols within a slot
void gen_sc_alloc(PhyCommon phy)
{
    // initialize as NULL
    for (int i=0; i<NFFT; i++)
        phy->pilot_sc[i] = OFDMFRAME_SCTYPE_NULL;

    // upper band
    for (int i=0; i<(NUM_DATA_SC+NUM_PILOT)/2; i++) {
        phy->pilot_sc[i] = OFDMFRAME_SCTYPE_DATA;
    }

    // lower band
    for (int i=0; i<(NUM_DATA_SC+NUM_PILOT)/2; i++) {
        phy->pilot_sc[NFFT-1-i] = OFDMFRAME_SCTYPE_DATA;
    }

    // set pilots
    phy->pilot_sc[2] = OFDMFRAME_SCTYPE_PILOT;
    phy->pilot_sc[7] = OFDMFRAME_SCTYPE_PILOT;
    phy->pilot_sc[12] = OFDMFRAME_SCTYPE_PILOT;
    phy->pilot_sc[17] = OFDMFRAME_SCTYPE_PILOT;
    phy->pilot_sc[NFFT-1-2] = OFDMFRAME_SCTYPE_PILOT;
    phy->pilot_sc[NFFT-1-7] = OFDMFRAME_SCTYPE_PILOT;
    phy->pilot_sc[NFFT-1-12] = OFDMFRAME_SCTYPE_PILOT;
    phy->pilot_sc[NFFT-1-17] = OFDMFRAME_SCTYPE_PILOT;

    // Pilot definition in time domain:
    for (int i=0; i<SUBFRAME_LEN; i++) {
    	phy->pilot_symbols[i] = NO_PILOT;
    }
    // first OFDM symbol of each data slot shall be pilot
    phy->pilot_symbols[0] = PILOT;
    for (int i=0; i<NUM_SLOT; i++) {
    	phy->pilot_symbols[DLCTRL_LEN+2+(SLOT_LEN+1)*i] = PILOT;
    }
}
