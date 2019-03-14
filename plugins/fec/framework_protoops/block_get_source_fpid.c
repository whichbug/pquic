#include "../framework/block_framework_sender.h"
#include "../framework/block_framework_receiver.c"


protoop_arg_t block_get_source_fpid(picoquic_cnx_t *cnx)
{
    block_fec_framework_t *bff = (block_fec_framework_t *) get_cnx(cnx, CNX_AK_INPUT, 0);
    return (protoop_arg_t) get_source_fpid(bff).raw;
}