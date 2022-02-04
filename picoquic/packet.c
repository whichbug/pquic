/*
* Author: Christian Huitema
* Copyright (c) 2017, Private Octopus, Inc.
* All rights reserved.
*
* Permission to use, copy, modify, and distribute this software for any
* purpose with or without fee is hereby granted, provided that the above
* copyright notice and this permission notice appear in all copies.
*
* THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS" AND
* ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED
* WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
* DISCLAIMED. IN NO EVENT SHALL Private Octopus, Inc. BE LIABLE FOR ANY
* DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES
* (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES;
* LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND
* ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
* (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS
* SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
*/

/*
 * Processing of an incoming packet.
 * - Has to find the proper context, based on either the 64 bit context ID
 *   or a combination of source address, source port and partial context value.
 * - Has to find the sequence number, based on partial values and windows.
 * - For initial packets, has to perform version checks.
 */

#include "fnv1a.h"
#include "picoquic_internal.h"
#include "tls_api.h"
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include "plugin.h"
#include "memory.h"
#include "logger.h"

/*
 * The new packet header parsing is version dependent
 */

int picoquic_parse_packet_header(
    picoquic_quic_t* quic,
    uint8_t* bytes,
    uint32_t length,
    struct sockaddr* addr_from,
    picoquic_packet_header* ph,
    picoquic_cnx_t** pcnx,
    int receiving)
{
    int ret = 0;
    uint8_t local_ctx_length = (quic == NULL) ? 0 : quic->local_ctx_length;

    /* Initialize the PH structure to zero, but version index to -1 (error) */
    memset(ph, 0, sizeof(picoquic_packet_header));
    ph->version_index = -1;
    
    if (length == 0) {
        return -1;
    }

    if ((bytes[0] & 0x40) != 0x40) {
        ph->ptype = picoquic_packet_error;
        ph->offset = length;
        ph->payload_length = 0;
        return ret;
    }

    /* Is this a long header of a short header? -- in any case, we need at least 17 bytes */
    if ((bytes[0] & 0x80) == 0x80) {
        if (length < 7) {
            ret = -1;
        } else {
            uint8_t l_dest_id, l_srce_id;
            /* The bytes at position 1..4 describe the version */
            picoquic_long_packet_type type = (bytes[0] >> 4) & 3;
            ph->vn = PICOPARSE_32(bytes + 1);
            l_dest_id = bytes[5];

            if (6 + l_dest_id + 1 > (int) length) {
                /* malformed packet */
                ret = -1;
            } else {
                ph->offset = 6;
                ph->offset += picoquic_parse_connection_id(bytes + ph->offset, l_dest_id, &ph->dest_cnx_id);
                l_srce_id = bytes[ph->offset++];
                ph->offset += picoquic_parse_connection_id(bytes + ph->offset, l_srce_id, &ph->srce_cnx_id);

                /* Not applicable for long packets. */
                ph->has_spin_bit = 0;
                ph->spin = 0;

                if (ph->vn == 0) {
                    /* VN = zero identifies a version negotiation packet */
                    ph->ptype = picoquic_packet_version_negotiation;
                    ph->pc = picoquic_packet_context_initial;
                    ph->payload_length = (uint16_t) ((length > ph->offset) ? length - ph->offset : 0);

                    if (*pcnx == NULL && quic != NULL) {
                        /* The version negotiation should always include the cnx-id sent by the client */
                        if (ph->dest_cnx_id.id_len > 0) {
                            *pcnx = picoquic_cnx_by_id(quic, ph->dest_cnx_id);
                        }
                        else {
                            *pcnx = picoquic_cnx_by_net(quic, addr_from);

                            if (*pcnx != NULL && (*pcnx)->path[0]->local_cnxid.id_len != 0) {
                                *pcnx = NULL;
                            }
                        }
                    }
                } else {
                    char context_by_addr = 0;
                    uint64_t payload_length = 0;
                    uint64_t pn_length_clear = 0;
                    uint32_t var_length = 0;

                    ph->version_index = picoquic_get_version_index(ph->vn);

                    if (ph->version_index >= 0) {
                        /* If the version is supported now, the format field in the version table
                        * describes the encoding. */
                        switch (picoquic_supported_versions[ph->version_index].version_header_encoding) {
                        case picoquic_version_header_29:
                            switch (type) {
                            case picoquic_long_packet_type_initial:
                            {
                                /* special case of the initial packets. They contain a retry token between the header
                                * and the encrypted payload */
                                uint64_t tok_len = 0;
                                size_t l_tok_len = picoquic_varint_decode(bytes + ph->offset, length - ph->offset, &tok_len);

                                ph->ptype = picoquic_packet_initial;
                                ph->pc = picoquic_packet_context_initial;
                                ph->epoch = 0;
                                if (l_tok_len == 0) {
                                    /* packet is malformed */
                                    ph->offset = length;
                                    ph->ptype = picoquic_packet_error;
                                    ph->pc = 0;
                                }
                                else {
                                    ph->token_length = (uint32_t)tok_len;
                                    ph->token_offset = ph->offset + (uint32_t)l_tok_len;
                                    ph->offset += (uint32_t)(l_tok_len + (size_t)tok_len);
                                }

                                break;
                            }
                            case picoquic_long_packet_type_0rtt:
                                ph->ptype = picoquic_packet_0rtt_protected;
                                ph->pc = picoquic_packet_context_application;
                                ph->epoch = 1;
                                break;
                            case picoquic_long_packet_type_handshake:
                                ph->ptype = picoquic_packet_handshake;
                                ph->pc = picoquic_packet_context_handshake;
                                ph->epoch = 2;
                                break;
                            case picoquic_long_packet_type_retry:
                                ph->ptype = picoquic_packet_retry;
                                ph->pc = picoquic_packet_context_initial;
                                ph->epoch = 0;
                                break;
                            default:
                                ph->offset = length;
                                ph->ptype = picoquic_packet_error;
                                ph->pc = 0;
                                break;
                            }
                            break;
                        default:
                            /* version is not supported */
                            DBG_PRINTF("Version (%x) is recognized but encoding not supported\n", ph->vn);
                            ph->ptype = picoquic_packet_error;
                            ph->version_index = -1;
                            ph->pc = 0;
                            break;
                        }
                    }

                    if (ph->ptype == picoquic_packet_retry) {
                        /* No segment length or sequence number in retry packets */
                        if (length > ph->offset) {
                            payload_length = (uint16_t)length - ph->offset;
                        }
                        else {
                            payload_length = 0;
                            ph->ptype = picoquic_packet_error;
                        }
                    } else {
                        if (ph->offset < length) {
                            var_length = (uint32_t)picoquic_varint_decode(bytes + ph->offset,
                                length - ph->offset, &payload_length);
                        }

                        if (var_length <= 0 || ph->offset + var_length + pn_length_clear + payload_length > length ||
                            ph->version_index < 0) {
                            ph->ptype = picoquic_packet_error;
                            ph->payload_length = (uint16_t)((length > ph->offset) ? length - ph->offset : 0);
                        }
                    }

                    if (ph->ptype != picoquic_packet_error)
                    {
                        ph->payload_length = (uint16_t)payload_length;
                        ph->offset += var_length;
                        ph->pn_offset = ph->offset;

                        /* Retrieve the connection context */
                        if (*pcnx == NULL && quic != NULL) {
                            *pcnx = picoquic_cnx_by_id(quic, ph->dest_cnx_id);

                            /* TODO: something for the case of client initial, e.g. source IP + initial CNX_ID */
                            if (*pcnx == NULL) {
                                *pcnx = picoquic_cnx_by_net(quic, addr_from);

                                if (*pcnx != NULL)
                                {
                                    context_by_addr = 1;
                                }
                            }
                        }

                        /* If the context was found by using `addr_from`, but the packet type
                         * does not allow that, reset the context to NULL. */
                        if (context_by_addr)
                        {
                            if (ph->ptype == picoquic_packet_initial || ph->ptype == picoquic_packet_0rtt_protected)
                            {
                                if (picoquic_compare_connection_id(&(*pcnx)->initial_cnxid, &ph->dest_cnx_id) != 0) {
                                    *pcnx = NULL;
                                }
                            } else {
                                *pcnx = NULL;
                            }
                        }
                    }
                }
            }
        }
    } else {
        /* If this is a short header, it should be possible to retrieve the connection
         * context. This depends on whether the quic context requires cnx_id or not.
         */
         uint8_t cnxid_length = (receiving == 0 && *pcnx != NULL) ? (*pcnx)->path[0]->remote_cnxid.id_len : local_ctx_length;
         ph->pc = picoquic_packet_context_application;

         if ((int)length >= 1 + cnxid_length) {
             /* We can identify the connection by its ID */
             ph->offset = (uint32_t)( 1 + picoquic_parse_connection_id(bytes + 1, cnxid_length, &ph->dest_cnx_id));
             /* TODO: should consider using combination of CNX ID and ADDR_FROM */
             if (*pcnx == NULL && quic != NULL) {
                 if (local_ctx_length > 0) {
                     *pcnx = picoquic_cnx_by_id(quic, ph->dest_cnx_id);
                 }
                 else {
                     *pcnx = picoquic_cnx_by_net(quic, addr_from);
                 }
             }
         } else {
             ph->ptype = picoquic_packet_error;
             ph->offset = length;
             ph->payload_length = 0;
             return ret;
         }

         if (*pcnx != NULL) {
             ph->epoch = 3;
             ph->version_index = (*pcnx)->version_index;
             /* If the connection is identified, decode the short header per version ID */
             switch (picoquic_supported_versions[ph->version_index].version_header_encoding) {
             case picoquic_version_header_29:
                 ph->has_spin_bit = 1;
                 ph->ptype = picoquic_packet_1rtt_protected_phi0;
                 ph->spin = (bytes[0] >> 5) & 1;
                 ph->pn_offset = ph->offset;
                 ph->pn = 0;
                 ph->pnmask = 0;
                 break;
             }

             if (length < ph->offset) {
                 ret = -1;
                 ph->payload_length = 0;
             } else {
                 ph->payload_length = (uint16_t)(length - ph->offset);
             }
         } else {
             /* This may be a packet to a forgotten connection */
             ph->payload_length = (uint16_t)((length > ph->offset)?length - ph->offset:0);
         }
    }

    return ret;
}

/* The packet number logic */
uint64_t picoquic_get_packet_number64(uint64_t highest, uint64_t mask, uint32_t pn)
{
    uint64_t expected = highest + 1;
    uint64_t not_mask_plus_one = (~mask) + 1;
    uint64_t pn64 = (expected & mask) | pn;

    if (pn64 < expected) {
        uint64_t delta1 = expected - pn64;
        uint64_t delta2 = not_mask_plus_one - delta1;
        if (delta2 < delta1) {
            pn64 += not_mask_plus_one;
        }
    } else {
        uint64_t delta1 = pn64 - expected;
        uint64_t delta2 = not_mask_plus_one - delta1;

        if (delta2 <= delta1 && (pn64 & mask) > 0) {
            /* Out of sequence packet from previous roll */
            pn64 -= not_mask_plus_one;
        }
    }

    return pn64;
}

/*
 * Decrypt the incoming packet.
 * Apply packet number decryption. This may require updating the
 * sequence number and the offset
 */
size_t  picoquic_decrypt_packet(picoquic_cnx_t* cnx,
    uint8_t* bytes, size_t packet_length, picoquic_packet_header* ph,
    void * hp_enc, void* aead_context, int * already_received, picoquic_path_t* path_from)
{
    size_t decoded;
    size_t length = ph->offset + ph->payload_length; /* this may change after decrypting the PN */

    /* Might happen if the CID is not the one expected */
    if (!path_from) {
        path_from = cnx->path[0];
    }

    if (already_received != NULL) {
        *already_received = 0;
    }

    if (hp_enc != NULL)
    {
        /* The header length is not yet known, will only be known after the sequence number is decrypted */
        size_t sample_offset = ph->pn_offset + 4;
        size_t sample_size = picoquic_hp_iv_size(hp_enc);
        uint8_t mask[5] = { 0, 0, 0, 0, 0 };

        if (sample_offset + sample_size > length)
        {
            /* Invalid packet format. Avoid crash! */
            ph->pn = 0xFFFFFFFF;
            ph->pnmask = 0xFFFFFFFF00000000ull;
            ph->offset = ph->pn_offset;

            DBG_PRINTF("Invalid packet format, type: %d, epoch: %d, pc: %d, pn: %d\n",
                       ph->ptype, ph->epoch, ph->pc, (int)ph->pn);
        } else {
            /* Decode */
            uint8_t first_byte = bytes[0];
            uint8_t first_mask = ((first_byte & 0x80) == 0x80) ? 0x0F : 0x1F;
            uint8_t pn_l;
            uint32_t pn_val = 0;

            picoquic_hp_encrypt(hp_enc, bytes + sample_offset, mask, mask, sizeof(mask));
            /* Decode the first byte */
            first_byte ^= (mask[0] & first_mask);
            pn_l = (first_byte & 3) + 1;
            ph->pnmask = (0xFFFFFFFFFFFFFFFFull);
            bytes[0] = first_byte;

            /* Packet encoding is 1 to 4 bytes */
            for (uint8_t i = 1; i <= pn_l; i++) {
                pn_val <<= 8;
                bytes[ph->offset] ^= mask[i];
                pn_val += bytes[ph->offset++];
                ph->pnmask <<= 8;
            }

            ph->pn = pn_val;
            ph->payload_length -= pn_l;
            /* Only set the key phase byte if short header */
            if (ph->ptype == picoquic_packet_1rtt_protected_phi0) {
                ph->ptype = ((first_byte >> 2) & 1) == 1 ? picoquic_packet_1rtt_protected_phi1 : picoquic_packet_1rtt_protected_phi0;
            }
        }
    } else {
        /* The hp_enc algorithm was not initialized. Avoid crash! */
        ph->pn = 0xFFFFFFFF;
        ph->pnmask = 0xFFFFFFFF00000000ull;
        ph->offset = ph->pn_offset;

        DBG_PRINTF("PN dec not ready, type: %d, epoch: %d, pc: %d, pn: %d\n",
            ph->ptype, ph->epoch, ph->pc, (int)ph->pn);
    }

    /* Build a packet number to 64 bits */
    ph->pn64 = picoquic_get_packet_number64(
        (already_received==NULL)?path_from->pkt_ctx[ph->pc].send_sequence:
        path_from->pkt_ctx[ph->pc].first_sack_item.end_of_sack_range, ph->pnmask, ph->pn);

    /* verify that the packet is new */
    if (already_received != NULL && picoquic_is_pn_already_received(path_from, ph->pc, ph->pn64) != 0) {
        /* Set error type: already received */
        *already_received = 1;
    }

    /* by conventions, values larger than input indicate error */
    decoded = picoquic_aead_decrypt_generic(bytes + ph->offset,
                bytes + ph->offset, ph->payload_length, ph->pn64, bytes, ph->offset, aead_context);

    return decoded;
}

/**
 * See PROTOOP_NOPARAM_GET_INCOMING_PATH
 */
protoop_arg_t get_incoming_path(picoquic_cnx_t* cnx)
{
    picoquic_packet_header* ph = (picoquic_packet_header*) cnx->protoop_inputv[0];
    picoquic_path_t* path_from = NULL;

    if (picoquic_compare_connection_id(&ph->dest_cnx_id, &cnx->initial_cnxid) == 0 ||
        picoquic_compare_connection_id(&ph->dest_cnx_id, &cnx->path[0]->local_cnxid) == 0) {
        path_from = cnx->path[0];
    }

    return (protoop_arg_t) path_from;
}

picoquic_path_t* picoquic_get_incoming_path(
    picoquic_cnx_t* cnx,
    picoquic_packet_header* ph)
{
    return (picoquic_path_t*) protoop_prepare_and_run_noparam(cnx, &PROTOOP_NOPARAM_GET_INCOMING_PATH, NULL,
        ph);
}

int picoquic_parse_header_and_decrypt(
    picoquic_quic_t* quic,
    uint8_t* bytes,
    uint32_t length,
    uint32_t packet_length,
    struct sockaddr* addr_from,
    uint64_t current_time,
    picoquic_packet_header* ph,
    picoquic_cnx_t** pcnx,
    uint32_t * consumed,
    int * new_context_created)
{
    /* Parse the clear text header. Ret == 0 means an incorrect packet that could not be parsed */
    int already_received = 0;
    size_t decoded_length = 0;
    int ret = picoquic_parse_packet_header(quic, bytes, length, addr_from, ph, pcnx, 1);

    if (ret == 0) {
        /* TODO: clarify length, payload length, packet length -- special case of initial packet */
        length = ph->offset + ph->payload_length;
        *consumed = length;


        if (ph->ptype == picoquic_packet_initial) {
            if ((*pcnx == NULL || !(*pcnx)->client_mode)) {
                /* Create a connection context if the CI is acceptable */
                if (packet_length < PICOQUIC_ENFORCED_INITIAL_MTU) {
                    /* Unexpected packet. Reject, drop and log. */
                    ret = PICOQUIC_ERROR_INITIAL_TOO_SHORT;
                }
            }
            if (ret == 0 && *pcnx == NULL) {
                /* if listening is OK, listen */
                *pcnx = picoquic_create_cnx(quic, ph->dest_cnx_id, ph->srce_cnx_id, addr_from, current_time, ph->vn,
                                            NULL, NULL, 0);
                *new_context_created = (*pcnx == NULL) ? 0 : 1;
            }
        }

        /* TODO: replace switch by reference to epoch */

        if (*pcnx != NULL) {
            picoquic_path_t* path_from = picoquic_get_incoming_path(*pcnx, ph);
            switch (ph->ptype) {
            case picoquic_packet_version_negotiation:
                /* Packet is not encrypted */
                break;
            case picoquic_packet_initial:
                decoded_length = picoquic_decrypt_packet(*pcnx, bytes, packet_length, ph,
                    (*pcnx)->crypto_context[0].hp_dec,
                    (*pcnx)->crypto_context[0].aead_decrypt, &already_received, path_from);
                length = ph->offset + ph->payload_length;
                *consumed = length;
                break;
            case picoquic_packet_retry:
                /* packet is not encrypted, no sequence number. */
                ph->pn = 0;
                ph->pn64 = 0;
                ph->pnmask = 0;
                decoded_length = ph->payload_length;
                break;
            case picoquic_packet_handshake:
                decoded_length = picoquic_decrypt_packet(*pcnx, bytes, length, ph,
                    (*pcnx)->crypto_context[2].hp_dec,
                    (*pcnx)->crypto_context[2].aead_decrypt, &already_received, path_from);
                break;
            case picoquic_packet_0rtt_protected:
                decoded_length = picoquic_decrypt_packet(*pcnx, bytes, length, ph,
                    (*pcnx)->crypto_context[1].hp_dec,
                    (*pcnx)->crypto_context[1].aead_decrypt, &already_received, path_from);
                break;
            case picoquic_packet_1rtt_protected_phi0:
            case picoquic_packet_1rtt_protected_phi1:
                /* TODO : roll key based on PHI */
                /* AEAD Decrypt, in place */
                decoded_length = picoquic_decrypt_packet(*pcnx, bytes, length, ph,
                    (*pcnx)->crypto_context[3].hp_dec,
                    (*pcnx)->crypto_context[3].aead_decrypt, &already_received, path_from);
                break;
            default:
                /* Packet type error. Log and ignore */
                ret = PICOQUIC_ERROR_DETECTED;
                break;
            }

            /* TODO: consider the error "too soon" */
            if (decoded_length > (length - ph->offset)) {
                ret = PICOQUIC_ERROR_AEAD_CHECK;
                if (*new_context_created) {
                    picoquic_delete_cnx(*pcnx);
                    *pcnx = NULL;
                    *new_context_created = 0;
                }
            }
            else if (already_received != 0) {
                ret = PICOQUIC_ERROR_DUPLICATE;
            }
            else {
                ph->payload_length = (uint16_t)decoded_length;
            }
        }
        else if (ph->ptype == picoquic_packet_1rtt_protected_phi0 ||
            ph->ptype == picoquic_packet_1rtt_protected_phi1)
        {
            /* This may be a stateles reset */
            *pcnx = picoquic_cnx_by_net(quic, addr_from);

            if (*pcnx != NULL && length >= PICOQUIC_RESET_PACKET_MIN_SIZE &&
                memcmp(bytes + length - PICOQUIC_RESET_SECRET_SIZE,
                (*pcnx)->path[0]->reset_secret, PICOQUIC_RESET_SECRET_SIZE) == 0) {
                ret = PICOQUIC_ERROR_STATELESS_RESET;
            }
            else {
                *pcnx = NULL;
            }
        }
    }

    return ret;
}

/*
 * Processing of a version renegotiation packet.
 *
 * From the specification: When the client receives a Version Negotiation packet
 * from the server, it should select an acceptable protocol version. If the server
 * lists an acceptable version, the client selects that version and reattempts to
 * create a connection using that version. Though the contents of a packet might
 * not change in response to version negotiation, a client MUST increase the packet
 * number it uses on every packet it sends. Packets MUST continue to use long headers
 * and MUST include the new negotiated protocol version.
 */
int picoquic_incoming_version_negotiation(
    picoquic_cnx_t* cnx,
    uint8_t* bytes,
    uint32_t length,
    struct sockaddr* addr_from,
    picoquic_packet_header* ph,
    uint64_t current_time)
{
    /* Parse the content */
    int ret = -1;
#ifdef _WINDOWS
    UNREFERENCED_PARAMETER(addr_from);
#endif

    if (picoquic_compare_connection_id(&ph->dest_cnx_id, &cnx->path[0]->local_cnxid) != 0 || ph->vn != 0) {
        /* Packet that do not match the "echo" checks should be logged and ignored */
        ret = 0;
    } else {
        /* Trying to renegotiate the version, just ignore the packet if not good. */
        ret = picoquic_reset_cnx_version(cnx, bytes + ph->offset, length - ph->offset, current_time);
    }

    return ret;
}

/*
 * Send a version negotiation packet in response to an incoming packet
 * sporting the wrong version number.
 */

int picoquic_prepare_version_negotiation(
    picoquic_quic_t* quic,
    struct sockaddr* addr_from,
    struct sockaddr* addr_to,
    unsigned long if_index_to,
    picoquic_packet_header* ph)
{
    int ret = -1;
    picoquic_stateless_packet_t* sp = picoquic_create_stateless_packet(quic);

    if (sp != NULL) {
        uint8_t* bytes = sp->bytes;
        size_t byte_index = 0;

        /* Packet type set to random value for version negotiation */
        picoquic_public_random(bytes + byte_index, 1);
        bytes[byte_index++] |= 0x80;
        /* Set the version number to zero */
        picoformat_32(bytes + byte_index, 0);
        byte_index += 4;
        /* Encode the CIDs, in reverse of the incoming packet */
        bytes[byte_index++] = ph->srce_cnx_id.id_len;
        byte_index += picoquic_format_connection_id(bytes + byte_index, PICOQUIC_MAX_PACKET_SIZE - byte_index, ph->srce_cnx_id);
        bytes[byte_index++] = ph->dest_cnx_id.id_len;
        byte_index += picoquic_format_connection_id(bytes + byte_index, PICOQUIC_MAX_PACKET_SIZE - byte_index, ph->dest_cnx_id);

        /* Set the payload to the list of versions */
        for (size_t i = 0; i < picoquic_nb_supported_versions; i++) {
            picoformat_32(bytes + byte_index, picoquic_supported_versions[i].version);
            byte_index += 4;
        }
        /* Set length and addresses, and queue. */
        sp->length = byte_index;
        memset(&sp->addr_to, 0, sizeof(sp->addr_to));
        memcpy(&sp->addr_to, addr_from,
            (addr_from->sa_family == AF_INET) ? sizeof(struct sockaddr_in) : sizeof(struct sockaddr_in6));
        memset(&sp->addr_local, 0, sizeof(sp->addr_local));
        memcpy(&sp->addr_local, addr_to,
            (addr_to->sa_family == AF_INET) ? sizeof(struct sockaddr_in) : sizeof(struct sockaddr_in6));
        sp->if_index_local = if_index_to;
        picoquic_queue_stateless_packet(quic, sp);
    }

    return ret;
}

/*
 * Process an unexpected connection ID. This could be an old packet from a
 * previous connection. If the packet type correspond to an encrypted value,
 * the server can respond with a public reset.
 *
 * Per draft 14, the stateless reset starts with the packet code 0K110000.
 * The packet has after the first byte at least 20 random bytes, and then
 * the 16 bytes reset token.
 */
void picoquic_process_unexpected_cnxid(
    picoquic_quic_t* quic,
    uint32_t length,
    struct sockaddr* addr_from,
    struct sockaddr* addr_to,
    unsigned long if_index_to,
    picoquic_packet_header* ph)
{
    if (length > PICOQUIC_RESET_PACKET_MIN_SIZE &&
        (ph->ptype == picoquic_packet_1rtt_protected_phi0 ||
            ph->ptype == picoquic_packet_1rtt_protected_phi1)) {
        picoquic_stateless_packet_t* sp = picoquic_create_stateless_packet(quic);
        if (sp != NULL) {
            uint32_t pad_size = length - 17;
            uint8_t* bytes = sp->bytes;
            size_t byte_index = 0;

            if (pad_size > 20) {
                pad_size = (uint32_t)picoquic_public_uniform_random(pad_size - 20) + 20;
            }
            else {
                pad_size = 20;
            }

            /* Packet type set to short header */
            bytes[byte_index++] = (ph->ptype == picoquic_packet_1rtt_protected_phi0) ? 0x30 : 0x70;
            /* Add the random bytes */
            picoquic_public_random(bytes + byte_index, pad_size);
            byte_index += pad_size;
            /* Add the public reset secret */
            (void)picoquic_create_cnxid_reset_secret(quic, &ph->dest_cnx_id, bytes + byte_index);
            byte_index += PICOQUIC_RESET_SECRET_SIZE;
            sp->length = byte_index;
            memset(&sp->addr_to, 0, sizeof(sp->addr_to));
            memcpy(&sp->addr_to, addr_from,
                (addr_from->sa_family == AF_INET) ? sizeof(struct sockaddr_in) : sizeof(struct sockaddr_in6));
            memset(&sp->addr_local, 0, sizeof(sp->addr_local));
            memcpy(&sp->addr_local, addr_to,
                (addr_to->sa_family == AF_INET) ? sizeof(struct sockaddr_in) : sizeof(struct sockaddr_in6));
            sp->if_index_local = if_index_to;
            picoquic_queue_stateless_packet(quic, sp);
        }
    }
}

/*
 * Queue a stateless retry packet
 */

void picoquic_queue_stateless_retry(picoquic_cnx_t* cnx,
    picoquic_packet_header* ph, struct sockaddr* addr_from,
    struct sockaddr* addr_to,
    unsigned long if_index_to,
    uint8_t * token,
    size_t token_length)
{
    picoquic_stateless_packet_t* sp = picoquic_create_stateless_packet(cnx->quic);
    size_t checksum_length = picoquic_get_checksum_length(cnx, 1);

    if (sp != NULL) {
        uint8_t* bytes = sp->bytes;
        uint32_t byte_index = 0;
        size_t data_bytes = 0;
        uint32_t header_length = 0;
        uint32_t pn_offset;
        uint32_t pn_length;
        uint8_t odcil_random = ((uint8_t)picoquic_public_uniform_random(256))&0xF0;

        cnx->path[0]->remote_cnxid = ph->srce_cnx_id;

        byte_index = header_length = picoquic_create_packet_header(cnx, picoquic_packet_retry,
            cnx->path[0],
            0, bytes, &pn_offset, &pn_length);


        /* use same encoding as packet header */
        bytes[byte_index++] = odcil_random|picoquic_create_packet_header_cnxid_lengths(0, cnx->initial_cnxid.id_len);

        byte_index += picoquic_format_connection_id(bytes + byte_index,
            PICOQUIC_MAX_PACKET_SIZE - byte_index - checksum_length, cnx->initial_cnxid);
        byte_index += (uint32_t)data_bytes;
        memcpy(&bytes[byte_index], token, token_length);
        byte_index += (uint32_t)token_length;

        sp->length = byte_index;


        memset(&sp->addr_to, 0, sizeof(sp->addr_to));
        memcpy(&sp->addr_to, addr_from,
            (addr_from->sa_family == AF_INET) ? sizeof(struct sockaddr_in) : sizeof(struct sockaddr_in6));
        memset(&sp->addr_local, 0, sizeof(sp->addr_local));
        memcpy(&sp->addr_local, addr_to,
            (addr_to->sa_family == AF_INET) ? sizeof(struct sockaddr_in) : sizeof(struct sockaddr_in6));
        sp->if_index_local = if_index_to;
        picoquic_queue_stateless_packet(cnx->quic, sp);
    }
}

/*
 * Processing of an incoming client initial packet,
 * on an unknown connection context.
 */

int picoquic_incoming_initial(
    picoquic_cnx_t** pcnx,
    uint8_t* bytes,
    struct sockaddr* addr_from,
    struct sockaddr* addr_to,
    unsigned long if_index_to,
    picoquic_packet_header* ph,
    uint64_t current_time,
    int new_context_created)
{
    int ret = 0;
    size_t extra_offset = 0;

    /* Logic to test the retry token.
     * TODO: this should probably be implemented as a callback */
    if ((*pcnx)->quic->flags&picoquic_context_check_token) {
        uint8_t * base;
        size_t len;
        uint8_t token[16];

        if (addr_from->sa_family == AF_INET) {
            struct sockaddr_in * a4 = (struct sockaddr_in *)addr_from;
            len = 4;
            base = (uint8_t *)&a4->sin_addr;
        }
        else {
            struct sockaddr_in6 * a6 = (struct sockaddr_in6 *)addr_from;
            len = 16;
            base = (uint8_t *)&a6->sin6_addr;
        }

        if (picoquic_get_retry_token((*pcnx)->quic, base, len,
            token, sizeof(token)) != 0)
        {
            ret = PICOQUIC_ERROR_MEMORY;
        }
        else {
            if (ph->token_length != sizeof(token) ||
                memcmp(token, bytes + ph->token_offset, sizeof(token)) != 0)
            {
                picoquic_queue_stateless_retry(*pcnx, ph,
                    addr_from, addr_to, if_index_to, token, sizeof(token));
                ret = PICOQUIC_ERROR_RETRY;
            }
        }
    }

    /* decode the incoming frames */
    if (ret == 0) {
        picoquic_path_t* path_x = (*pcnx)->path[0];
        ret = picoquic_decode_frames(*pcnx,
            bytes + ph->offset + extra_offset, ph->payload_length - extra_offset, ph->epoch, current_time, path_x);
    }

    /* processing of client initial packet */
    if (ret == 0) {
        /* initialization of context & creation of data */
        /* TODO: find path to send data produced by TLS. */
        ret = picoquic_tls_stream_process(*pcnx);
    }

    if (ret != 0 || (*pcnx)->cnx_state == picoquic_state_disconnected) {
        /* This is bad. If this is an initial attempt, delete the connection */
        if (new_context_created) {
            picoquic_delete_cnx(*pcnx);
            *pcnx = NULL;
            ret = PICOQUIC_ERROR_CONNECTION_DELETED;
        }
    }
    else {
        /* remember the local address on which the initial packet arrived. */
        (*pcnx)->path[0]->if_index_local = if_index_to;
        (*pcnx)->path[0]->local_addr_len = (addr_to->sa_family == AF_INET) ? sizeof(struct sockaddr_in) : sizeof(struct sockaddr_in6);
        memcpy(&(*pcnx)->path[0]->local_addr, addr_to, (*pcnx)->path[0]->local_addr_len);
    }

    return ret;
}

/*
 * Processing of a server retry
 *
 * The packet number and connection ID fields echo the corresponding fields from the
 * triggering client packet. This allows a client to verify that the server received its packet.
 *
 * A Server Stateless Retry packet is never explicitly acknowledged in an ACK frame by a client.
 * Receiving another Client Initial packet implicitly acknowledges a Server Stateless Retry packet.
 *
 * After receiving a Server Stateless Retry packet, the client uses a new Client Initial packet
 * containing the next token. In effect, the next cryptographic
 * handshake message is sent on a new connection.
 */

int picoquic_incoming_retry(
    picoquic_cnx_t* cnx,
    uint8_t* bytes,
    picoquic_packet_header* ph,
    uint64_t current_time)
{
    int ret = 0;
    size_t token_length = 0;
    uint8_t * token = NULL;

    if (cnx->cnx_state != picoquic_state_client_init_sent && cnx->cnx_state != picoquic_state_client_init_resent) {
        ret = PICOQUIC_ERROR_UNEXPECTED_PACKET;
    } else {
        /* Verify that the header is a proper echo of what was sent */
        if (ph->vn != picoquic_supported_versions[cnx->version_index].version) {
            /* Packet that do not match the "echo" checks should be logged and ignored */
            ret = PICOQUIC_ERROR_UNEXPECTED_PACKET;
        } else if (ph->pn64 != 0) {
            /* after draft-12, PN is required to be 0 */
            ret = PICOQUIC_ERROR_UNEXPECTED_PACKET;
        }
    }

    if (ret == 0) {
        /* Parse the retry frame */
        size_t byte_index = ph->offset;
        uint8_t odcil;
        uint8_t unused_cil;

        picoquic_parse_packet_header_cnxid_lengths(bytes[byte_index++], &unused_cil, &odcil);


        if (odcil != cnx->initial_cnxid.id_len || odcil + 1 > ph->payload_length ||
            memcmp(cnx->initial_cnxid.id, &bytes[byte_index], odcil) != 0) {
            /* malformed ODCIL, or does not match initial cid; ignore */
            ret = PICOQUIC_ERROR_UNEXPECTED_PACKET;
        } else {
            byte_index += odcil;
            token_length = ph->offset + ph->payload_length - byte_index;

            if (token_length > 0) {
                token = malloc(token_length);
                if (token == NULL) {
                    ret = PICOQUIC_ERROR_MEMORY;
                } else {
                    memcpy(token, &bytes[byte_index], token_length);
                }
            }
        }
    }

    if (ret == 0) {
        /* reset the initial CNX_ID to the version sent by the server */
        cnx->initial_cnxid = ph->srce_cnx_id;

        /* keep a copy of the retry token */
        if (cnx->retry_token != NULL) {
            free(cnx->retry_token);
        }
        cnx->retry_token = token;
        cnx->retry_token_length = (uint32_t)token_length;

        picoquic_reset_cnx(cnx, current_time);
    }

    if (ret == 0) {
        /* Mark the packet as not required for ack */
        ret = PICOQUIC_ERROR_RETRY;
    }

    return ret;
}

/*
 * Processing of a server clear text packet.
 */

int picoquic_incoming_server_cleartext(
    picoquic_cnx_t* cnx,
    uint8_t* bytes,
    struct sockaddr* addr_to,
    unsigned long if_index_to,
    picoquic_packet_header* ph,
    uint64_t current_time)
{
    int ret = 0;
#ifdef _WINDOWS
    UNREFERENCED_PARAMETER(addr_to);
    UNREFERENCED_PARAMETER(if_index_to);
#endif

    if (cnx->cnx_state == picoquic_state_client_init_sent || cnx->cnx_state == picoquic_state_client_init_resent) {
        picoquic_set_cnx_state(cnx, picoquic_state_client_handshake_start);
    }

    int restricted = cnx->cnx_state != picoquic_state_client_handshake_start && cnx->cnx_state != picoquic_state_client_handshake_progress;

    /* Check the server cnx id */
    if (picoquic_is_connection_id_null(cnx->path[0]->remote_cnxid) && restricted == 0) {
        /* On first response from the server, copy the cnx ID and the incoming address */
        cnx->path[0]->remote_cnxid = ph->srce_cnx_id;
        cnx->path[0]->local_addr_len = (addr_to->sa_family == AF_INET) ? sizeof(struct sockaddr_in) : sizeof(struct sockaddr_in6);
        memcpy(&cnx->path[0]->local_addr, addr_to, cnx->path[0]->local_addr_len);
    }
    else if (picoquic_compare_connection_id(&cnx->path[0]->remote_cnxid, &ph->srce_cnx_id) != 0) {
        ret = PICOQUIC_ERROR_CNXID_CHECK; /* protocol error */
    }


    if (ret == 0) {
        /* Accept the incoming frames */
        picoquic_path_t* path_x = cnx->path[0];
        ret = picoquic_decode_frames(cnx,
            bytes + ph->offset, ph->payload_length, ph->epoch, current_time, path_x);
    }

    /* processing of initial packet */
    if (ret == 0 && restricted == 0) {
        ret = picoquic_tls_stream_process(cnx);

        /* If the handshake keys have been received there is no need to
         * repeat the initial packet any more */

        if (ret == 0 && cnx->crypto_context[2].aead_decrypt != NULL &&
            cnx->crypto_context[2].aead_encrypt != NULL)
        {
            picoquic_implicit_handshake_ack(cnx, cnx->path[0], picoquic_packet_context_initial, current_time);
        }
    }

    if (ret != 0) {
        /* This is bad. should just delete the context, log the packet, etc */
    }


    return ret;
}

/*
 * Processing of client clear text packet.
 */
int picoquic_incoming_client_cleartext(
    picoquic_cnx_t* cnx,
    uint8_t* bytes,
    picoquic_packet_header* ph,
    uint64_t current_time)
{
    int ret = 0;

    if (cnx->cnx_state == picoquic_state_server_init
        || cnx->cnx_state == picoquic_state_server_handshake
        || cnx->cnx_state == picoquic_state_server_almost_ready
        || cnx->cnx_state == picoquic_state_server_ready) {
        if (picoquic_compare_connection_id(&ph->srce_cnx_id, &cnx->path[0]->remote_cnxid) != 0) {
            ret = PICOQUIC_ERROR_CNXID_CHECK;
        } else {
            /* Accept the incoming frames */
            picoquic_path_t* path_x = cnx->path[0];
            ret = picoquic_decode_frames(cnx,
                bytes + ph->offset, ph->payload_length, ph->epoch, current_time, path_x);

            /* processing of client clear text packet */
            if (ret == 0) {
                /* initialization of context & creation of data */
                /* TODO: find path to send data produced by TLS. */
                ret = picoquic_tls_stream_process(cnx);
            }

            /* If ClientFinished has been received, the server side handshake is done */
            if (ret == 0 && !cnx->client_mode && !cnx->handshake_done &&
                cnx->cnx_state == picoquic_state_server_ready && picoquic_is_tls_handshake_complete(cnx)) {
                cnx->handshake_done = 1;
                for (int i = 0; i < cnx->nb_paths; i++) {
                    picoquic_path_t *path = cnx->path[i];
                    picoquic_implicit_handshake_ack(cnx, path, picoquic_packet_context_initial, current_time);
                    picoquic_implicit_handshake_ack(cnx, path, picoquic_packet_context_handshake, current_time);
                }
                if (cnx->callback_fn != NULL) {
                    if (cnx->callback_fn(cnx, 0, NULL, 0, picoquic_callback_ready, cnx->callback_ctx, NULL) != 0) {
                        picoquic_connection_error(cnx, PICOQUIC_TRANSPORT_INTERNAL_ERROR, 0);
                    }
                }
            }
        }
    } else {
        /* Not expected. Log and ignore. */
        ret = PICOQUIC_ERROR_UNEXPECTED_PACKET;
    }

    return ret;
}

/*
* Processing of stateless reset packet.
*/
int picoquic_incoming_stateless_reset(
    picoquic_cnx_t* cnx)
{
    /* Stateless reset. The connection should be abandonned */
    picoquic_set_cnx_state(cnx, picoquic_state_disconnected);

    if (cnx->callback_fn) {
        (void)(cnx->callback_fn)(cnx, 0, NULL, 0, picoquic_callback_stateless_reset, cnx->callback_ctx, NULL);
    }

    return PICOQUIC_ERROR_AEAD_CHECK;
}

/*
 * Processing of 0-RTT packet
 */

int picoquic_incoming_0rtt(
    picoquic_cnx_t* cnx,
    uint8_t* bytes,
    picoquic_packet_header* ph,
    uint64_t current_time)
{
    int ret = 0;

    if (!(picoquic_compare_connection_id(&ph->dest_cnx_id , &cnx->initial_cnxid)==0 ||
        picoquic_compare_connection_id(&ph->dest_cnx_id, &cnx->path[0]->local_cnxid) == 0) ||
        picoquic_compare_connection_id(&ph->srce_cnx_id, &cnx->path[0]->remote_cnxid) != 0 ) {
        ret = PICOQUIC_ERROR_CNXID_CHECK;
    } else if (cnx->cnx_state == picoquic_state_server_almost_ready || cnx->cnx_state == picoquic_state_server_ready) {
        if (ph->vn != picoquic_supported_versions[cnx->version_index].version) {
            ret = picoquic_connection_error(cnx, PICOQUIC_TRANSPORT_PROTOCOL_VIOLATION, 0);
        } else {
            /* Accept the incoming frames */
            picoquic_path_t* path_x = cnx->path[0];
            ret = picoquic_decode_frames(cnx,
                bytes + ph->offset, ph->payload_length, ph->epoch, current_time, path_x);

            if (ret == 0) {
                /* Processing of TLS messages -- EOED */
                ret = picoquic_tls_stream_process(cnx);
            }
        }
    } else {
        /* Not expected. Log and ignore. */
        ret = PICOQUIC_ERROR_UNEXPECTED_PACKET;
    }

    return ret;
}


/**
 * See PROTOOP_NOPARAM_INCOMING_ENCRYPTED
 */
protoop_arg_t incoming_encrypted(picoquic_cnx_t *cnx)
{
    /* Is argc at the right value? */
    if (cnx->protoop_inputc != 4) {
        printf("Not matching number of arguments: %d != %d\n", cnx->protoop_inputc, 4);
        return PICOQUIC_ERROR_PROTOCOL_OPERATION_UNEXEPECTED_ARGC;
    }

    uint8_t* bytes = (uint8_t *) cnx->protoop_inputv[0];
    picoquic_packet_header* ph = (picoquic_packet_header *) cnx->protoop_inputv[1];
    struct sockaddr* addr_from = (struct sockaddr *) cnx->protoop_inputv[2];
    uint64_t current_time = (uint64_t) cnx->protoop_inputv[3];

    int ret = 0;
    picoquic_packet_context_enum pc = ph->pc;
    picoquic_path_t* path_x = picoquic_get_incoming_path(cnx, ph);

    if (!path_x) {
        ret = PICOQUIC_ERROR_CNXID_CHECK;
    } else if (cnx->cnx_state < picoquic_state_client_almost_ready) {
        /* handshake is not complete. Just ignore the packet */
        ret = PICOQUIC_ERROR_UNEXPECTED_PACKET;
    } else if (cnx->cnx_state == picoquic_state_disconnected) {
        /* Connection is disconnected. Just ignore the packet */
        ret = PICOQUIC_ERROR_UNEXPECTED_PACKET;
    }
    else {
        /* Packet is correct */
        if (ph->pn64 > path_x->pkt_ctx[pc].first_sack_item.end_of_sack_range) {
            cnx->current_spin = ph->spin ^ cnx->client_mode;
            if (ph->has_spin_bit && cnx->current_spin != cnx->prev_spin) {
                // got an edge
                cnx->prev_spin = cnx->current_spin;
                cnx->spin_edge = 1;
                cnx->spin_vec = (ph->spin_vec == 3) ? 3 : (ph->spin_vec + 1);
                cnx->spin_last_trigger = picoquic_get_quic_time(cnx->quic);
            }
        }

        /* Do not process data in closing or draining modes */
        if (cnx->cnx_state >= picoquic_state_closing_received) {
            /* only look for closing frames in closing modes */
            if (cnx->cnx_state == picoquic_state_closing) {
                int closing_received = 0;

                ret = picoquic_decode_closing_frames(cnx,
                    bytes + ph->offset, ph->payload_length, &closing_received);

                if (ret == 0) {
                    if (closing_received) {
                        if (cnx->client_mode) {
                            picoquic_set_cnx_state(cnx, picoquic_state_disconnected);
                        }
                        else {
                            picoquic_set_cnx_state(cnx, picoquic_state_draining);
                        }
                    }
                    else {
                        path_x->pkt_ctx[ph->pc].ack_needed = 1;
                    }
                }
            }
            else {
                /* Just ignore the packets in closing received or draining mode */
                ret = PICOQUIC_ERROR_UNEXPECTED_PACKET;
            }
        }
        else {
            /* Compare the packet address to the current path value */
            if (picoquic_compare_addr((struct sockaddr *)&path_x->peer_addr,
                (struct sockaddr *)addr_from) != 0 &&
                (((addr_from->sa_family != AF_INET) || ((struct sockaddr_in *) addr_from)->sin_addr.s_addr != 0))) /* This line is a pure hotfix for UDP src address being 0.0.0.0 */
            { // TODO: Handle equivalent IPv4 encoded in IPv6
                /* uint8_t buffer[16]; */ // Unused
                /* size_t challenge_length; */ // Unused

                /* Address origin different than expected. Update */
                LOG { ;
                    char old[INET6_ADDRSTRLEN] = {0};
                    char new[INET6_ADDRSTRLEN] = {0};
                    struct sockaddr *old_addr = (struct sockaddr *) &path_x->peer_addr;
                    struct sockaddr *new_addr = addr_from;
                    inet_ntop(old_addr->sa_family, old_addr->sa_family == AF_INET ? &((struct sockaddr_in *)old_addr)->sin_addr : &((struct sockaddr_in6 *)old_addr)->sin6_addr, old, path_x->peer_addr_len);
                    inet_ntop(new_addr->sa_family, new_addr->sa_family == AF_INET ? &((struct sockaddr_in *)new_addr)->sin_addr : &((struct sockaddr_in6 *)new_addr)->sin6_addr, new, path_x->peer_addr_len);
                    LOG_EVENT(cnx, "connectivity", "peer_address_changed", "incoming_encrypted",
                              "{\"path\": \"%p\", \"old_address\": \"%s\", \"new_address\": \"%s\"}", path_x, old, new);
                }
                path_x->peer_addr_len = (addr_from->sa_family == AF_INET) ? sizeof(struct sockaddr_in) : sizeof(struct sockaddr_in6);
                memcpy(&path_x->peer_addr, addr_from, path_x->peer_addr_len);
                /* Reset the path challenge */
                path_x->challenge = picoquic_public_random_64();
                path_x->challenge_verified = 0;
                path_x->challenge_time = current_time + path_x->retransmit_timer;
                path_x->challenge_repeat_count = 0;
                protoop_prepare_and_run_noparam(cnx, &PROTOOP_NOPARAM_PEER_ADDRESS_CHANGED, NULL, path_x);
            }

            /* Compute receive bandwidth */
            path_x->received += ph->offset + ph->payload_length + picoquic_get_checksum_length(cnx, 0);
            if (path_x->receive_rate_epoch == 0) {
                path_x->received_prior = path_x->received;
                path_x->receive_rate_epoch = current_time;
            } else {
                uint64_t delta = current_time - path_x->receive_rate_epoch;
                if (delta > path_x->smoothed_rtt && delta > PICOQUIC_BANDWIDTH_TIME_INTERVAL_MIN) {
                    path_x->receive_rate_estimate = ((path_x->received - path_x->received_prior)*1000000) / delta;
                    path_x->received_prior = path_x->received;
                    path_x->receive_rate_epoch = current_time;
                    if (path_x->receive_rate_estimate > path_x->receive_rate_max) {
                        path_x->receive_rate_max = path_x->receive_rate_estimate;
                    }
                }
            }

            /* Accept the incoming frames */
            ret = picoquic_decode_frames(cnx,
                bytes + ph->offset, ph->payload_length, ph->epoch, current_time, path_x);
        }

        if (ret == 0) {
            /* Processing of TLS messages  */
            ret = picoquic_tls_stream_process(cnx);
        }
    }

    return (protoop_arg_t) ret;
}

/*
 * Processing of client encrypted packet.
 */
int picoquic_incoming_encrypted(
    picoquic_cnx_t* cnx,
    uint8_t* bytes,
    picoquic_packet_header* ph,
    struct sockaddr* addr_from,
    uint64_t current_time)
{
    return (int) protoop_prepare_and_run_noparam(cnx, &PROTOOP_NOPARAM_INCOMING_ENCRYPTED, NULL,
        bytes, ph, addr_from, current_time);
}

/*
* Processing of the packet that was just received from the network.
*/

int picoquic_incoming_segment(
    picoquic_quic_t* quic,
    uint8_t* bytes,
    uint32_t length,
    uint32_t packet_length,
    uint32_t * consumed,
    struct sockaddr* addr_from,
    struct sockaddr* addr_to,
    int if_index_to,
    uint64_t current_time,
    picoquic_connection_id_t * previous_dest_id,
    int *new_context_created)
{
    int ret = 0;
    picoquic_cnx_t* cnx = NULL;
    picoquic_packet_header ph;
    *new_context_created = 0;

    /* Parse the header and decrypt the packet */
    ret = picoquic_parse_header_and_decrypt(quic, bytes, length, packet_length, addr_from,
        current_time, &ph, &cnx, consumed, new_context_created);

    if (cnx != NULL) LOG {
        PUSH_LOG_CTX(cnx, "\"packet_type\": \"%s\", \"pn\": %" PRIu64, picoquic_log_ptype_name(ph.ptype), ph.pn64);
    }

    /* Verify that the segment coalescing is for the same destination ID */
    if (ret == 0) {
        if (picoquic_is_connection_id_null(*previous_dest_id)) {
            *previous_dest_id = ph.dest_cnx_id;
        }
        /* This is commented out because with multipath, we can have coalescing of several destination IDs without having issues... */
        /*
        else if (picoquic_compare_connection_id(previous_dest_id, &ph.dest_cnx_id) != 0) {
            ret = PICOQUIC_ERROR_CNXID_SEGMENT;

        }
        */
    }

    /* Log the incoming packet */
    picoquic_log_decrypted_segment(quic->F_log, 1, cnx, 1, &ph, bytes, (uint32_t)*consumed, ret);

    if (ret == 0) {
        if (cnx == NULL) {
            if (ph.version_index < 0 && ph.vn != 0) {
                /* use the result of parsing to consider version negotiation */
                picoquic_prepare_version_negotiation(quic, addr_from, addr_to, if_index_to, &ph);
            }
            else {
                /* Unexpected packet. Reject, drop and log. */
                if (!picoquic_is_connection_id_null(ph.dest_cnx_id)) {
                    picoquic_process_unexpected_cnxid(quic, length, addr_from, addr_to, if_index_to, &ph);
                }
                ret = PICOQUIC_ERROR_DETECTED;
            }
        }
        else {
            /* TO DO: Find the incoming path */
            /* TO DO: update each of the incoming functions, since the packet is already decrypted. */
            /* Hook for performing action when connection received new packet */
            picoquic_received_packet(cnx, quic->rcv_socket, quic->rcv_tos);
            picoquic_path_t *path = (picoquic_path_t *) protoop_prepare_and_run_noparam(cnx, &PROTOOP_NOPARAM_GET_INCOMING_PATH, NULL, &ph);
            picoquic_header_parsed(cnx, &ph, path, *consumed);
            if (cnx != NULL) LOG {
                PUSH_LOG_CTX(cnx, "\"path\": \"%p\"", path);
            }

            switch (ph.ptype) {
            case picoquic_packet_version_negotiation:
                if (cnx->cnx_state == picoquic_state_client_init_sent) {
                    /* Proceed with version negotiation*/
                    ret = picoquic_incoming_version_negotiation(
                        cnx, bytes, length, addr_from, &ph, current_time);
                }
                else {
                    /* This is an unexpected packet. Log and drop.*/
                    DBG_PRINTF("Unexpected packet (%d), type: %d, epoch: %d, pc: %d, pn: %d\n",
                        cnx->client_mode, ph.ptype, ph.epoch, ph.pc, (int) ph.pn);
                    ret = PICOQUIC_ERROR_DETECTED;
                }
                break;
            case picoquic_packet_initial:
                /* Initial packet: either crypto handshakes or acks. */
                if (picoquic_compare_connection_id(&ph.dest_cnx_id, &cnx->initial_cnxid) == 0 ||
                    picoquic_compare_connection_id(&ph.dest_cnx_id, &cnx->path[0]->local_cnxid) == 0) {
                    /* Verify that the source CID matches expectation */
                    if (picoquic_is_connection_id_null(cnx->path[0]->remote_cnxid)) {
                        cnx->path[0]->remote_cnxid = ph.srce_cnx_id;
                        cnx->path[0]->local_addr_len = (addr_to->sa_family == AF_INET) ? sizeof(struct sockaddr_in) : sizeof(struct sockaddr_in6);
                        memcpy(&cnx->path[0]->local_addr, addr_to, cnx->path[0]->local_addr_len);
                    } else if (picoquic_compare_connection_id(&cnx->path[0]->remote_cnxid, &ph.srce_cnx_id) != 0) {
                        DBG_PRINTF("Error wrong srce cnxid (%d), type: %d, epoch: %d, pc: %d, pn: %d\n",
                            cnx->client_mode, ph.ptype, ph.epoch, ph.pc, (int)ph.pn);
                        ret = PICOQUIC_ERROR_UNEXPECTED_PACKET;
                    }
                    if (ret == 0) {
                        if (cnx->client_mode == 0) {
                            /* TODO: finish processing initial connection packet */
                            cnx->local_parameters.original_destination_connection_id = ph.dest_cnx_id;
                            ret = picoquic_incoming_initial(&cnx, bytes,
                                addr_from, addr_to, if_index_to, &ph, current_time, *new_context_created);
                        }
                        else {
                            /* TODO: this really depends on the current receive epoch */
                            ret = picoquic_incoming_server_cleartext(cnx, bytes, addr_to, if_index_to, &ph, current_time);
                        }
                    }
                } else {
                    DBG_PRINTF("Error detected (%d), type: %d, epoch: %d, pc: %d, pn: %d\n",
                        cnx->client_mode, ph.ptype, ph.epoch, ph.pc, (int)ph.pn);
                    ret = PICOQUIC_ERROR_DETECTED;
                }
                break;
            case picoquic_packet_retry:
                /* TODO: server retry is completely revised in the new version. */
                ret = picoquic_incoming_retry(cnx, bytes, &ph, current_time);
                break;
            case picoquic_packet_handshake:
                if (cnx->client_mode)
                {
                    ret = picoquic_incoming_server_cleartext(cnx, bytes, addr_to, if_index_to, &ph, current_time);
                }
                else
                {
                    ret = picoquic_incoming_client_cleartext(cnx, bytes, &ph, current_time);
                }
                break;
            case picoquic_packet_0rtt_protected:
                /* TODO : decrypt with 0RTT key */
                ret = picoquic_incoming_0rtt(cnx, bytes, &ph, current_time);
                break;
            case picoquic_packet_1rtt_protected_phi0:
            case picoquic_packet_1rtt_protected_phi1:
                ret = picoquic_incoming_encrypted(cnx, bytes, &ph, addr_from, current_time);
                /* TODO : roll key based on PHI */
                break;
            default:
                /* Packet type error. Log and ignore */
                DBG_PRINTF("Unexpected packet type (%d), type: %d, epoch: %d, pc: %d, pn: %d\n",
                    cnx->client_mode, ph.ptype, ph.epoch, ph.pc, (int) ph.pn);
                ret = PICOQUIC_ERROR_DETECTED;
                break;
            }
            if (cnx != NULL) LOG {
                POP_LOG_CTX(cnx);
            }
        }
    } else if (ret == PICOQUIC_ERROR_STATELESS_RESET) {
        ret = picoquic_incoming_stateless_reset(cnx);
    }

    if (ret == 0 || ret == PICOQUIC_ERROR_SPURIOUS_REPEAT) {
        if (cnx != NULL && cnx->cnx_state != picoquic_state_disconnected &&
            ph.ptype != picoquic_packet_version_negotiation) {
            /* Mark the sequence number as received */
            /* FIXME */
            picoquic_path_t* path_x = picoquic_get_incoming_path(cnx, &ph);
            ret = picoquic_record_pn_received(cnx, path_x, ph.pc, ph.pn64, current_time);
        }
        if (cnx != NULL) {
            picoquic_cnx_set_next_wake_time(cnx, current_time);
        }
    } else if (ret == PICOQUIC_ERROR_DUPLICATE) {
        /* Bad packets are dropped silently, but duplicates should be acknowledged */
        if (cnx != NULL) {
            /* FIXME */
            picoquic_path_t* path_x = picoquic_get_incoming_path(cnx, &ph);
            path_x->pkt_ctx[ph.pc].ack_needed = 1;
        }
        ret = -1;
    } else if (ret == PICOQUIC_ERROR_AEAD_CHECK || ret == PICOQUIC_ERROR_INITIAL_TOO_SHORT ||
        ret == PICOQUIC_ERROR_UNEXPECTED_PACKET || ret == PICOQUIC_ERROR_FNV1A_CHECK ||
        ret == PICOQUIC_ERROR_CNXID_CHECK ||
        ret == PICOQUIC_ERROR_RETRY || ret == PICOQUIC_ERROR_DETECTED ||
        ret == PICOQUIC_ERROR_CONNECTION_DELETED ||
        ret == PICOQUIC_ERROR_CNXID_SEGMENT) {
        /* Bad packets are dropped silently */

        DBG_PRINTF("Packet (%d) dropped, t: %d, e: %d, pc: %d, pn: %d, l: %d, ret : %x\n",
            (cnx == NULL) ? -1 : cnx->client_mode, ph.ptype, ph.epoch, ph.pc, (int)ph.pn,
            length, ret);
        ret = -1;
    } else if (ret == 1) {
        /* wonder what happened ! */
        DBG_PRINTF("Packet (%d) get ret=1, t: %d, e: %d, pc: %d, pn: %d, l: %d\n",
            (cnx == NULL) ? -1 : cnx->client_mode, ph.ptype, ph.epoch, ph.pc, (int)ph.pn, length);
        ret = -1;
    } else if (ret != 0) {
        DBG_PRINTF("Packet (%d) error, t: %d, e: %d, pc: %d, pn: %d, l: %d, ret : %x\n",
            (cnx == NULL) ? -1 : cnx->client_mode, ph.ptype, ph.epoch, ph.pc, (int)ph.pn, length, ret);
        ret = -1;
    }

    if (cnx != NULL) LOG {
        POP_LOG_CTX(cnx);
    }

    if (cnx != NULL) {
        if (!cnx->processed_transport_parameter && cnx->remote_parameters_received) {
            picoquic_handle_plugin_negotiation(cnx);
            cnx->processed_transport_parameter = 1;
        }
    }

    return ret;
}

int picoquic_incoming_packet(
    picoquic_quic_t* quic,
    uint8_t* bytes,
    uint32_t length,
    struct sockaddr* addr_from,
    struct sockaddr* addr_to,
    int if_index_to,
    uint64_t current_time,
    int* new_context_created)
{
    uint32_t consumed_index = 0;
    int ret = 0;
    picoquic_connection_id_t previous_destid = picoquic_null_connection_id;


    while (consumed_index < length) {
        uint32_t consumed = 0;

        ret = picoquic_incoming_segment(quic, bytes + consumed_index,
            length - consumed_index, length,
            &consumed, addr_from, addr_to, if_index_to, current_time, &previous_destid, new_context_created);

        if (ret == 0) {
            consumed_index += consumed;
        } else {
            ret = 0;
            break;
        }
    }

    return ret;
}

void packet_register_noparam_protoops(picoquic_cnx_t *cnx)
{
    register_noparam_protoop(cnx, &PROTOOP_NOPARAM_INCOMING_ENCRYPTED, &incoming_encrypted);
    register_noparam_protoop(cnx, &PROTOOP_NOPARAM_GET_INCOMING_PATH, &get_incoming_path);
}
