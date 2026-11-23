#ifndef __ISOTP_CONFIG__
#define __ISOTP_CONFIG__

/* Max number of messages the receiver can receive at one time, this value 
 * is affectied by can driver queue length
 */
#define ISO_TP_DEFAULT_BLOCK_SIZE   8

/* The STmin parameter value specifies the minimum time gap allowed between 
 * the transmission of consecutive frame network protocol data units
 */
#define ISO_TP_DEFAULT_ST_MIN       0

/* This parameter indicate how many FC N_PDU WTs can be transmitted by the 
 * receiver in a row.
 */
#define ISO_TP_MAX_WFT_NUMBER       10

/* Private: The default timeout to use when waiting for a response during a
 * multi-frame send or receive.
 */
/* Microseconds (isotp_user_get_us). ISO 15765-2 N_Bs / N_Cr are 1000 ms;
 * BridgeLEG used 100 ms, which can abort a transfer while a busy ECU is
 * slow to send its flow-control frame. */
#define ISO_TP_DEFAULT_RESPONSE_TIMEOUT 1000000

/* Private: Determines if by default, padding is added to ISO-TP message frames.
 */
#define ISO_TP_FRAME_PADDING

#endif

