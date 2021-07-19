#include "../include/ike.h"


char src_addr[16];
char dst_addr[16];
char current_time[21];

int get_response_flag(struct rte_isakmp_hdr *hdr){
    //if 1, this packet is used to respond else if request
    return (hdr->flags >> 5) & 1;
}

int get_version_flag(struct rte_isakmp_hdr *hdr){
    //if 1, responder can use higher version
    return (hdr->flags >> 4) & 1;
}

int get_initiator_flag(struct rte_isakmp_hdr *hdr){
    //if 1, sender is initiator else responder
    return (hdr->flags >> 3) & 1;
}

/// Gets Exchange type of isakmp packet
/// Should only be used if logging the exchange type is needed
/// @param hdr isakmp header of packet
/// @return String containing Exchange type of packet
char *get_exchange_type (struct rte_isakmp_hdr *hdr){
    int index  = hdr->exchange_type - 34;
    if(index > 0 && index - 34 < 4){
        return exchange_types[index];
    }
    else return "";
}

/// Gets Payload Type from a isakmp header as a string
/// Should only be used if logging the payload type is needed
/// @param hdr isakmp header of packet
/// @returns a string containing the payload type
char *get_ike_payload_type(struct rte_isakmp_hdr *hdr){
    if(hdr->nxt_payload == NO){
        return "No Next Payload";
    }
    else if(hdr->nxt_payload == SKF){
        return "Encrypted and Authenticated Fragment";
    }
    else if(hdr->nxt_payload >= SA && hdr->nxt_payload <= EAP){
        return payload_types[hdr->nxt_payload - 33];
    }
    else{
        return NULL;
    }
}

/// Gets Payload Type from a isakmp payload header as a string
/// Should only be used if logging the payload type is needed
/// @param hdr isakmp header of packet
/// @returns a string containing the payload type
char *get_payload_nxt_payload(struct isakmp_payload_hdr *hdr){
    if(hdr->nxt_payload == NO){
        return "No Next Payload";
    }
    else if(hdr->nxt_payload == SKF){
        return "Encrypted and Authenticated Fragment";
    }
    else if(hdr->nxt_payload >= SA && hdr->nxt_payload <= EAP){
        return payload_types[hdr->nxt_payload - 33];
    }
    else{
        return NULL;
    }
}

void print_isakmp_headers_info(struct rte_isakmp_hdr *isakmp_hdr){
    //used to print important ike header instructions, can be converted to log into file if needed
    printf("Initiator SPI: %lx\n", rte_be_to_cpu_64(isakmp_hdr->initiator_spi));
    printf("Responder SPI: %lx\n", rte_be_to_cpu_64(isakmp_hdr->responder_spi));
    printf("Initiator: %d\n",get_initiator_flag(isakmp_hdr));
    printf("Response: %d\n",get_response_flag(isakmp_hdr));
    printf("Message ID: %04x\n\n",rte_be_to_cpu_32(isakmp_hdr->message_id));
    printf("Exchange type: %s", get_exchange_type(isakmp_hdr));
}

/**
 * Analyses a Security Association payload
 * @param pkt : pointer to packet used
 * @param offset: offset to paylaod header
 * @param isakmp_hdr pointer to isakmp headers
 * @param ipv4_hdr pointer to ipv4 headers
 * @returns 1 if there are no errors analyzing the packet, 0 if otherwise
 */
int analyse_SA(struct rte_mbuf *pkt,uint16_t offset,struct rte_isakmp_hdr *isakmp_hdr,struct rte_ipv4_hdr *ipv4_hdr){
    int check = 1;
    if(offset + sizeof(struct isakmp_payload_hdr) < rte_pktmbuf_data_len(pkt) ){
        struct isakmp_payload_hdr *payload;
        payload = rte_pktmbuf_mtod_offset(pkt,struct isakmp_payload_hdr *,offset); //get payload header
        char **proposals;
        
        int count = get_proposals(pkt,offset + sizeof(struct isakmp_payload_hdr),&proposals,&check); //get proposals and their respective transformations
        if(count == 0){
            printf("sad\n");
        }
        else{
            for(int i = 0;i<count;i++){
                char* proposal = proposals[i];
                char log[4096] = {0};
                if(get_initiator_flag(isakmp_hdr) == 0){
                    snprintf(log,4096,"%s;Proposals selected by %s: %s\n",current_time,src_addr,proposal);
                }
                else{
                    snprintf(log,4096,"%s;Proposals proposed by %s: %s\n",current_time,src_addr,proposal);
                    for(int i = 1;i <= tunnels->size; i++){
                        struct tunnel *tunnel = tunnels->array[i];
                        if(check_ike_spi(isakmp_hdr->initiator_spi,isakmp_hdr->responder_spi,ipv4_hdr->src_addr,ipv4_hdr->dst_addr,tunnel) == 1){
                            break;
                        }
                    }
                }
                write_log(ipsec_log,log,LOG_INFO);
                proposal = NULL;
                free(proposal);
            }
            proposals = NULL;
            free(proposals);
        }
        
        if(payload->nxt_payload !=0){
            check = analyse_isakmp_payload(pkt,isakmp_hdr,ipv4_hdr,offset + rte_be_to_cpu_16(payload->length),payload->nxt_payload); //continue analyzing packet
        }
        return check;
    }
    else{
        return 0;
    }
    
}

/**
 * Analyses a Key Exchange payload
 * @param pkt : pointer to packet used
 * @param offset: offset to paylaod header
 * @param isakmp_hdr pointer to isakmp headers
 * @param ipv4_hdr pointer to ipv4 headers
 * @returns 1 if there are no errors analyzing the packet, 0 if otherwise
 */
int analyse_KE(struct rte_mbuf *pkt,uint16_t offset,struct rte_isakmp_hdr *isakmp_hdr,struct rte_ipv4_hdr *ipv4_hdr){
    int check = 1;
    if(offset + sizeof(struct key_exchange) <= rte_pktmbuf_data_len(pkt)){
        struct key_exchange* payload;
        payload = rte_pktmbuf_mtod_offset(pkt,struct key_exchange *,offset);
        if(payload->hdr.nxt_payload !=0){
            check = analyse_isakmp_payload(pkt,isakmp_hdr,ipv4_hdr,offset + rte_be_to_cpu_16(payload->hdr.length),payload->hdr.nxt_payload);
        }
    }
    else{
        check = 0;
    }
    return check;
}

/**
 * Analyses a Authenticated and Encrypted payload. Note that whats inside cannot be analysed because it is encrypted
 * @param pkt : pointer to packet used
 * @param offset: offset to paylaod header
 * @param isakmp_hdr pointer to isakmp headers
 * @param ipv4_hdr pointer to ipv4 headers
 * @returns 1 if there are no errors analyzing the packet, 0 if otherwise
 */
int analyse_SK(struct rte_mbuf *pkt, uint16_t offset, struct rte_isakmp_hdr *isakmp_hdr,struct rte_ipv4_hdr *ipv4_hdr){
    if(offset + sizeof(struct isakmp_payload_hdr) <= rte_pktmbuf_data_len(pkt)){
        struct isakmp_payload_hdr *payload_hdr;
        payload_hdr = rte_pktmbuf_mtod_offset(pkt,struct isakmp_payload_hdr *,offset);
        
        for(int i = 1;i <= tunnels->size; i++){
            struct tunnel *tunnel = tunnels->array[i];
            char log[2048] = {0};
            if(check_ike_spi(isakmp_hdr->initiator_spi,isakmp_hdr->responder_spi,ipv4_hdr->src_addr,ipv4_hdr->dst_addr,tunnel) == 1){
                //nid to ensure spi is the same
                if(payload_hdr->nxt_payload == NO && isakmp_hdr->exchange_type == INFORMATIONAL){
                    //Dead peer detection
                    //responder will send the request and initiator has to respond within 6 requests
                    if(get_initiator_flag(isakmp_hdr) == 0 && get_response_flag(isakmp_hdr) == 0){
                        // DPD start/continue
                        tunnel->dpd_count += 1;
                        if(tunnel->dpd_count == 1){
                            tunnel->dpd = true;
                        }
                        if(tunnel->dpd_count == 6){
                            // Peer is dead and session should be removed

                            
                            snprintf(log,2048,"%s;Session ended btw %s and %s\n",current_time,src_addr,dst_addr);
                            write_log(ipsec_log,log,LOG_INFO);
                            // printf("Session ended btw SPI: %lx, %lx\n", rte_be_to_cpu_64(isakmp_hdr->initiator_spi), rte_be_to_cpu_64(isakmp_hdr->responder_spi));
                            removeIndex(tunnels,i);
                        }
                    }
                    else if(get_initiator_flag(isakmp_hdr) == 1 && get_response_flag(isakmp_hdr) == 1){
                        //Peer has responded and is not dead , hence refresh dpd is reset
                        tunnel->dpd_count = 0;
                        tunnel->dpd = false;
                    }
                }
                else if(payload_hdr->nxt_payload == D && isakmp_hdr->exchange_type == INFORMATIONAL){
                    //Either side ends connection, so delete tunnel
                    snprintf(log,2048,"%s;Session ended btw %s and %s\n",current_time,
                    src_addr,dst_addr);
                    write_log(ipsec_log,log,LOG_INFO);
                    delete_tunnel(isakmp_hdr->initiator_spi,isakmp_hdr->responder_spi,ipv4_hdr->src_addr,ipv4_hdr->dst_addr);
                }
                else if(payload_hdr->nxt_payload == AUTH && isakmp_hdr->exchange_type == IKE_AUTH){
                    //99.9% means authenticated once responder sends this payload unless server kena gon
                    if(get_response_flag(isakmp_hdr) == 1){
                        snprintf(log,2048,"%s;IKE Authentication between %s and %s succeeded\n",current_time,
                        src_addr,dst_addr);
                        write_log(ipsec_log,log,LOG_INFO);
                        tunnel->auth = true;
                    }
                }
                else if(payload_hdr->nxt_payload == N && isakmp_hdr->exchange_type == INFORMATIONAL && get_initiator_flag(isakmp_hdr) == 0 && get_response_flag(isakmp_hdr) == 1){
                    // for now it prob means smth went wrong
                    snprintf(log,2048,"%s;IKE Authentication between %s and %s failed\n",current_time,
                    src_addr, dst_addr);
                    delete_tunnel(isakmp_hdr->initiator_spi,isakmp_hdr->responder_spi,ipv4_hdr->src_addr,ipv4_hdr->dst_addr);
                }
                
            }
                
        }
        return 1;
    }
    else{
        return 0;
    }
    
}

/**
 * Analyses a Notify payload. If an error code is sent, should kill sesssion i think?
 * @param pkt : pointer to packet used
 * @param offset: offset to paylaod header
 * @param isakmp_hdr pointer to isakmp headers
 * @param ipv4_hdr pointer to ipv4 headers
 * @returns 1 if there are no errors analyzing the packet, 0 if otherwise
 */

int analyse_N(struct rte_mbuf *pkt, uint16_t offset,struct rte_isakmp_hdr *isakmp_hdr,struct rte_ipv4_hdr *ipv4_hdr){
    int check = 1;
    if(offset + sizeof(struct notify_hdr) + sizeof(struct isakmp_payload_hdr) <= rte_pktmbuf_data_len(pkt)){
        struct notify *payload;
        bool error = false;
        bool special_error = false;
        payload = malloc(sizeof(struct notify_hdr));
        char *msg = malloc(256);
        char failed_msg[128] = "";
        char log[2048];
        strcat(failed_msg,"IKE failed with error:");
        if(payload && msg){
            payload->payload_hdr = rte_pktmbuf_mtod_offset(pkt,struct isakmp_payload_hdr *,offset);
            payload->hdr = rte_pktmbuf_mtod_offset(pkt,struct notify_hdr *,offset + sizeof(struct isakmp_payload_hdr));
            if(rte_be_to_cpu_16(payload->hdr->msg_type) <= 44){
                //so far anything above 44 isnt done theres the 16k series of msg types;
                char * msg_type = notify_msg_type[rte_be_to_cpu_16(payload->hdr->msg_type) - 1];
                if(strcmp(msg_type, "\0") != 0 && rte_be_to_cpu_16(payload->hdr->msg_type) !=  INVALID_SPI && rte_be_to_cpu_16(payload->hdr->msg_type) !=  INVALID_MSG_ID){
                    //Error codes related to auth
                    error = true;
                    strcat(failed_msg,msg_type);
                }
                //other error codes
                else if(rte_be_to_cpu_16(payload->hdr->msg_type) ==  INVALID_SPI){
                    msg = "Invalid SPI detected by firewall\n";
                    special_error = true;
                }
                else if(rte_be_to_cpu_16(payload->hdr->msg_type) ==  INVALID_MSG_ID){
                    msg = "Invalid Message ID detected by firewall\n";
                    special_error = true;
                }
                if(error){
                    delete_tunnel(isakmp_hdr->initiator_spi,isakmp_hdr->responder_spi,ipv4_hdr->src_addr,ipv4_hdr->dst_addr);
                    
                    snprintf(log,2048,"%s;%s",current_time,
                    failed_msg);
                    write_log(ipsec_log,log,LOG_NOTICE);
                }
                else if(special_error){
                    snprintf(log,2048,"%s;%s",current_time,
                    msg);
                    write_log(ipsec_log,log,LOG_NOTICE);
                }
            }

        }
        if(payload->payload_hdr->nxt_payload != NO){
            check = analyse_isakmp_payload(pkt,isakmp_hdr,ipv4_hdr,offset + rte_be_to_cpu_16(payload->payload_hdr->length),payload->payload_hdr->nxt_payload);
        }   
        free(payload);
        free(msg);
    }
    else{
        check = 0;
    }
    return check;
}

/**
 * Analyses a Certificate/Certificate request payload
 * @param pkt : pointer to packet used
 * @param offset: offset to paylaod header
 * @param isakmp_hdr pointer to isakmp headers
 * @param ipv4_hdr pointer to ipv4 headers
 * @returns 1 if there are no errors analyzing the packet, 0 if otherwise
 */
int analyse_CERT(struct rte_mbuf *pkt, uint16_t offset,struct rte_isakmp_hdr *isakmp_hdr,struct rte_ipv4_hdr *ipv4_hdr){
    int check = 1;
    if(offset + sizeof(struct isakmp_payload_hdr) + sizeof(int8_t) <= rte_pktmbuf_data_len(pkt)){
        struct certificate *certificate =  malloc(sizeof(struct certificate));
        if(certificate){
            certificate->hdr = rte_pktmbuf_mtod_offset(pkt,struct isakmp_payload_hdr*,offset);
            certificate->type = rte_pktmbuf_mtod_offset(pkt,int8_t*,offset + sizeof(struct isakmp_payload_hdr));
            int encoding_length = rte_be_to_cpu_16(certificate->hdr->length) - sizeof(struct isakmp_payload_hdr) - sizeof(int8_t);
            uint8_t *buf = malloc(encoding_length);
            if(buf){
                uint8_t *encoding = rte_pktmbuf_read(pkt,offset+sizeof(struct isakmp_payload_hdr)+1,encoding_length,buf);
            }
            if(certificate->hdr->nxt_payload != NO){
                check = analyse_isakmp_payload(pkt,isakmp_hdr,ipv4_hdr,offset + rte_be_to_cpu_16(certificate->hdr->length),certificate->hdr->nxt_payload);
            }
        }
    }
    else{
        check = 0;
    }
    return check;
}

/**
 * Get proposals and transformations found in a SA payload
 * @param pkt pointer to packet being analyzed
 * @param offset offset to SA hdr
 * @param proposals string array containing proposals
 * @param check int to check whether if packet is malformed
 * @returns number of proposals found
 */
int get_proposals(struct rte_mbuf *pkt, uint16_t offset,char***proposals,int *check){
    struct proposal_struc *proposal;
    proposal = malloc(3 * __SIZEOF_POINTER__);
    int count = 0;
    *proposals = 0;
    do{
        if(offset + sizeof(struct proposal_hdr) <= rte_pktmbuf_data_len(pkt) ){
            if(*proposals){
                char **temp = reallocarray(*proposals,count+1,__SIZEOF_POINTER__);
                if(temp){
                    *proposals = temp;
                }
            }
            else{
                *proposals = calloc(1,__SIZEOF_POINTER__);
            }
            
            if(!*proposals){
                printf("Failed to allocate memory. Exiting\n");
                exit(1);
            }
            proposal->hdr = rte_pktmbuf_mtod_offset(pkt,struct proposal_hdr *, offset);
            
            uint16_t transformation_offset = offset + 8 + proposal->hdr->spi_size;
            (*proposals)[count] = calloc(4096,sizeof(char));
            if((*proposals)[count]){
                get_transformations(pkt,transformation_offset,proposal->hdr->num_transforms,(*proposals)[count],check);
            }
            else{
                printf("Failed to allocate memory. Exiting\n");
                exit(1);
            }
            
            offset += rte_be_to_cpu_16(proposal->hdr->len); //add offset for nxt proposal
            count++;
        }
        else{
            *check = 0;
            break;
        }
        
    }while(proposal->hdr->nxt_payload != (int8_t)0 && *proposals);
    
    return count;
    
}

/** 
 * Get Transformations found in a proposal and converts them into a string to log:
 * @param pkt pointer to packet to be analyzed
 * @param transformations pointer to Array contained in payload_struc object
 * @param offset Offset to transformation
 * @param size Number of transformations
 * @param buf string used to store transformations found
 * @param check int to check whether if packet is malformed
 */
void get_transformations(struct rte_mbuf *pkt, int offset,int size,char *buf,int* check){
    struct transform_struc *transform = malloc(2 * __SIZEOF_POINTER__);
    if(transform){
        int actual = 0;
        void *objects[] = {0};
        do{
            if(offset + sizeof(struct transform_hdr) <= rte_pktmbuf_data_len(pkt) ){
                transform->hdr = rte_pktmbuf_mtod_offset(pkt,struct transform_hdr *, offset);
                actual++;
                
                switch(transform->hdr->type){
                    case ENCR:
                        sprintf(buf + strlen(buf),"%s",encr_algo[rte_be_to_cpu_16(transform->hdr->transform_ID) - 1]);
                        if(transform->hdr->len != 8){
                            if(offset + sizeof(struct attr) <= rte_pktmbuf_data_len(pkt) ){
                                 //get attributes for transformation usually key length
                                struct attr *attribute  = rte_pktmbuf_mtod_offset(pkt,struct attr *,offset + 8);
                                sprintf(buf + strlen(buf),"_%d/",rte_be_to_cpu_16(attribute->value));
                            }
                            else{
                                *check = 0;
                            }
                        }
                        else{
                            sprintf(buf + strlen(buf),"/");
                        }
                        break;
                    case PRF:
                        sprintf(buf + strlen(buf),"%s/",pseudorandom_func[rte_be_to_cpu_16(transform->hdr->transform_ID) -1]);
                        break;
                    case INTEG:
                        sprintf(buf + strlen(buf),"%s/",integrity_func[rte_be_to_cpu_16(transform->hdr->transform_ID)]);
                        break;
                    case D_H:
                        sprintf(buf + strlen(buf),"%s/",DH[rte_be_to_cpu_16(transform->hdr->transform_ID)]);
                        break;
                    case ESN:
                        break;
                    default:
                        printf("Invalid Transform Type!\n");
                        *check = 0;
                        break;
                }
                if(actual > size){
                    printf("Too many transformations!\n");
                    *check = 0;
                }
                if(*check == 0){
                    break;
                }
                offset += rte_be_to_cpu_16(transform->hdr->len);
                
            }
            else{
                *check = 0;
            }
            
        }while(transform->hdr->nxt_payload != 0);
    }

}

/** deletes tunnel from authenticated tunnels once session ends
 */
void delete_tunnel(uint64_t initiator_spi,uint64_t responder_spi,int src_addr,int dst_addr){
    for(int i = 1;i <= tunnels->size; i++){
        struct tunnel *tunnel = tunnels->array[i];
        if(check_ike_spi(initiator_spi,responder_spi,src_addr,dst_addr,tunnel) == 1){
            remove_tunnel(tunnel);
            removeIndex(tunnels,i);
            break;
        }
    }
}

/** 
 * checks whether if ike information in tunnel matches provided spis and ip address
 * @returns 1 if information matches, 0 if otherwise
 */
int check_ike_spi(uint64_t initiator_spi,uint64_t responder_spi,int src_addr,int dst_addr,struct tunnel* tunnel){
    return (tunnel->client_spi == initiator_spi 
                && tunnel->host_spi == responder_spi) && ((tunnel->client_ip == src_addr && tunnel->host_ip == dst_addr) || 
                (tunnel->host_ip == src_addr && tunnel->client_ip == dst_addr)) ? 1 : 0;
}

/** 
 * checks whether if ike information in tunnel actually exists
 * @param isakmp_hdr isakmp header containing initiator and responder spis to check
 * @param ipv4_hdr IPV4 header containing respective ip addresses of client and host to check
 * @returns 1 if information matches, 0 if otherwise
 */
int check_if_tunnel_exists(struct rte_isakmp_hdr *isakmp_hdr,struct rte_ipv4_hdr *ipv4_hdr){
    for(int i = 1;i<=tunnels->size;i++){
        struct tunnel *tunnel = (struct tunnel *)tunnels->array[i];
        if(check_ike_spi(isakmp_hdr->initiator_spi,isakmp_hdr->responder_spi,ipv4_hdr->src_addr,ipv4_hdr->dst_addr,tunnel) == 1){
            tunnel->timeout = 0;
            return 1;
        }
    }
    return 0;
}

/** converts ipv4 address into strings and place them in ip
 * @param ip_address ipv4 address to convert 
 * @param ip char pointer to store the converted string
 */
void get_ip_address_string(rte_be32_t ip_address,char *ip){
    int bit4 = ip_address >> 24 & 0xFF;
    int bit3 = ip_address >> 16 & 0xFF;
    int bit2 = ip_address >> 8 & 0xFF;
    int bit1 = ip_address & 0xFF;
    sprintf(ip,"%u.%u.%u.%u",bit1,bit2,bit3,bit4);
}   

void add_tunnel(struct tunnel* add){
    char* bytes = malloc(serialize_size);
    if(bytes){
        memcpy(bytes,add,serialize_size);
        char* tunnel = b64_encode(bytes,serialize_size);
        FILE* fp = fopen(tunnel_log, "a+");
        fprintf(fp,"%s\n",tunnel);
        fclose(fp);
    }
    free(bytes);
}

void remove_tunnel(struct tunnel* remove){
    char* bytes = malloc(serialize_size);
    char* line;
    long file_size;
    size_t len = 0;
    size_t size = 0;
    size_t read = 0;
    int offset = 0;
    if(bytes){
        memcpy(bytes,remove,serialize_size);
        char* tunnel = b64_encode(bytes,serialize_size);
        FILE* fp = fopen(tunnel_log, "r+");
        fseek(fp,0, SEEK_END);
        file_size = ftell(fp);
        fseek(fp, 0L, SEEK_SET);
        char* og_file = calloc(file_size,sizeof(char));
        if(og_file){
            fread(og_file,sizeof(char),file_size,fp);
            fseek(fp, 0L, SEEK_SET);
            while(read = getline(&line,&len,fp) != -1){
                int line_len = strlen(line);
                line[line_len-1] = NULL;
                if(strcmp(line,tunnel) == 0){
                    
                    long move_size = file_size - offset - line_len;
                    memmove(og_file + offset,og_file + offset + line_len,move_size);
                    int newSize = file_size - line_len;
                    fclose(fp);
                    FILE *writefp = fopen(tunnel_log, "w+");
                    fwrite(og_file,sizeof(char),newSize,writefp);
                    fclose(writefp);
                    break;
                }
                offset += read;
            }
        }
        free(og_file);
    }
    free(bytes);
}

void load_tunnel(){
    FILE* fp = fopen(tunnel_log, "r+");
    char* line;
    char* decoded;
    size_t len = 0;
    size_t read = 0;
    if(fp != NULL){
        while(read = getline(&line,&len,fp) != -1){
            int line_len = strlen(line);
            line[line_len-1] = NULL;
            decoded = b64_decode(line,strlen(line));
            if(decoded){
                struct tunnel *tunnel = calloc(1,sizeof(struct tunnel));
                if(tunnel){
                    memcpy(tunnel,decoded,serialize_size);
                    tunnel->client_loaded = true;
                    tunnel->host_loaded = true;
                    tunnel->auth = true;
                    tunnel->dpd = false;
                    push(tunnels,tunnel);
                }
            }
        }
    }
}


/** converts ipv6 address into strings and place them in ip
 * @param ip_address ipv4 address to convert 
 * @param src_ip char pointer to store the converted source ip string
 * @param dst_ip char pointer to store the converted destination ip string
 */
void get_ipv6_hdr_string(struct rte_ipv6_hdr *hdr,char *src_ip, char *dst_ip)
{
    uint8_t *addr;
    addr = hdr->src_addr;
    snprintf(src_ip,45,"%4hx:%4hx:%4hx:%4hx:%4hx:%4hx:%4hx:%4hx \t",
           (uint16_t)((addr[0] << 8) | addr[1]),
           (uint16_t)((addr[2] << 8) | addr[3]),
           (uint16_t)((addr[4] << 8) | addr[5]),
           (uint16_t)((addr[6] << 8) | addr[7]),
           (uint16_t)((addr[8] << 8) | addr[9]),
           (uint16_t)((addr[10] << 8) | addr[11]),
           (uint16_t)((addr[12] << 8) | addr[13]),
           (uint16_t)((addr[14] << 8) | addr[15]));
    addr = hdr->dst_addr;
    snprintf(dst_ip,45,"%4hx:%4hx:%4hx:%4hx:%4hx:%4hx:%4hx:%4hx",
           (uint16_t)((addr[0] << 8) | addr[1]),
           (uint16_t)((addr[2] << 8) | addr[3]),
           (uint16_t)((addr[4] << 8) | addr[5]),
           (uint16_t)((addr[6] << 8) | addr[7]),
           (uint16_t)((addr[8] << 8) | addr[9]),
           (uint16_t)((addr[10] << 8) | addr[11]),
           (uint16_t)((addr[12] << 8) | addr[13]),
           (uint16_t)((addr[14] << 8) | addr[15]));
}

/**
 * Analyzes payload within an isakmp packet. Note that this function is recursive in nature and will continue until nxt_payload is 0
 * @param pkt Pointer to packet buffer to be analyzed
 * @param isakmp_hdr pointer to isakmp headers in a packet
 * @param offset to start analyzing
 * @param nxt_payload Should take from isakmp_hdr or payload_hdr
 * @return 1 if packet is legit, 0 if otherrwise
 */
int analyse_isakmp_payload(struct rte_mbuf *pkt,struct rte_isakmp_hdr *isakmp_hdr,struct rte_ipv4_hdr *ipv4_hdr,uint16_t offset,int nxt_payload){
    int check = 1;
    get_ip_address_string(ipv4_hdr->src_addr,src_addr);
    get_ip_address_string(ipv4_hdr->dst_addr,dst_addr);
    get_current_time(current_time);

    if(isakmp_hdr->exchange_type == IKE_SA_INIT){
        if(check_if_tunnel_exists(isakmp_hdr,ipv4_hdr)==0 && get_initiator_flag(isakmp_hdr) == 0 && isakmp_hdr->responder_spi != (rte_be64_t)0){
            //Only if server responds then tunnel should be considered legit
            struct tunnel new_tunnel;
            new_tunnel.host_ip = ipv4_hdr->src_addr;
            new_tunnel.client_ip = ipv4_hdr->dst_addr;

            new_tunnel.host_spi = isakmp_hdr->responder_spi;
            new_tunnel.client_spi = isakmp_hdr->initiator_spi;
            new_tunnel.host_esp_spi = 0;
            new_tunnel.client_esp_spi = 0;

            new_tunnel.dpd = false;
            new_tunnel.dpd_count = 0;

            new_tunnel.client_seq = 0;
            new_tunnel.host_seq = 0;
            new_tunnel.timeout = 0;
            new_tunnel.auth = false;
            new_tunnel.client_loaded = false;
            new_tunnel.host_loaded = false;
            push(tunnels,&new_tunnel);
        };
    }
   
    if((isakmp_hdr->exchange_type == IKE_SA_INIT && check_if_tunnel_exists(isakmp_hdr,ipv4_hdr)==0) || (check_if_tunnel_exists(isakmp_hdr,ipv4_hdr)==1)){
        // If tunnel does not exist, should only be IKE_SA_INIT, else sus
        switch(nxt_payload){
            case SA:
                check = analyse_SA(pkt,offset,isakmp_hdr,ipv4_hdr);
                break;

            case KE:
                check = analyse_KE(pkt,offset,isakmp_hdr,ipv4_hdr);
                break;

            case N:
                check = analyse_N(pkt,offset,isakmp_hdr,ipv4_hdr);
                break;

            case D:{
                //Session is deleted
                char log[2048] = {0};
                snprintf(log,2048,"%s;Session ended btw %s and %s\n",current_time,
                src_addr,dst_addr);
                write_log(ipsec_log,log,LOG_INFO);
                delete_tunnel(isakmp_hdr->initiator_spi,isakmp_hdr->responder_spi,ipv4_hdr->src_addr,ipv4_hdr->dst_addr);
                break;
            }
            case SK:
                check = analyse_SK(pkt,offset,isakmp_hdr,ipv4_hdr);
                break;
            
            case CERT:
                check = analyse_CERT(pkt,offset,isakmp_hdr,ipv4_hdr);
                break;

            case CERTREQ:
                check = analyse_CERT(pkt,offset,isakmp_hdr,ipv4_hdr);
                break;

            default:{
                if(offset + sizeof(struct isakmp_payload_hdr) < rte_pktmbuf_data_len(pkt) ){
                    struct isakmp_payload_hdr *payload_hdr;
                    payload_hdr = rte_pktmbuf_mtod_offset(pkt,struct isakmp_payload_hdr *,offset);
                    if(nxt_payload != SKF){
                        analyse_isakmp_payload(pkt,isakmp_hdr,ipv4_hdr,offset + rte_be_to_cpu_16(payload_hdr->length),payload_hdr->nxt_payload);
                    }
                }
                else{
                    check = 0;
                }
                
                break;
            }
        }   
    }
    else{
        check = 0;
    }
    return check;

}
