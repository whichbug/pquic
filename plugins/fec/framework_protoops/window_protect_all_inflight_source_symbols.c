#include "../framework/window_framework_sender.c"
#include "../framework/window_framework_receiver.c"


protoop_arg_t window_select_symbols_to_protect(picoquic_cnx_t *cnx)
{
    fec_block_t *fb = (fec_block_t *) get_cnx(cnx, CNX_AK_INPUT, 0);
    window_fec_framework_t *wff = (window_fec_framework_t *) get_cnx(cnx, CNX_AK_INPUT, 1);
    fb->current_source_symbols = 0;
    PROTOOP_PRINTF(cnx, "SELECT, SMALLEST = %u, HIGHEST = %u\n", wff->smallest_in_transit, wff->highest_in_transit);
    for (int i = MAX(wff->smallest_in_transit, wff->highest_in_transit - MIN(RECEIVE_BUFFER_MAX_LENGTH, wff->highest_in_transit)) ; i <= wff->highest_in_transit ; i++) {
        source_symbol_t *ss = wff->fec_window[((uint32_t) i) % RECEIVE_BUFFER_MAX_LENGTH];
        if (ss && ss->source_fec_payload_id.raw == i) {
            fb->source_symbols[fb->current_source_symbols++] = ss;
        }
    }
    fb->total_source_symbols = fb->current_source_symbols;
    fb->total_repair_symbols = MIN(wff->n-wff->k, fb->total_source_symbols);

    return 0;
}