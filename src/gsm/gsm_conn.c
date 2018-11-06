/**
 * \file            gsm_conn.c
 * \brief           Connection API
 */
 
/*
 * Copyright (c) 2018 Tilen Majerle
 *  
 * Permission is hereby granted, free of charge, to any person
 * obtaining a copy of this software and associated documentation
 * files (the "Software"), to deal in the Software without restriction,
 * including without limitation the rights to use, copy, modify, merge,
 * publish, distribute, sublicense, and/or sell copies of the Software, 
 * and to permit persons to whom the Software is furnished to do so, 
 * subject to the following conditions:
 * 
 * The above copyright notice and this permission notice shall be
 * included in all copies or substantial portions of the Software.
 * 
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,
 * EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES
 * OF MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE
 * AND NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT
 * HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY,
 * WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING 
 * FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR
 * OTHER DEALINGS IN THE SOFTWARE.
 *
 * This file is part of GSM-AT library.
 *
 * Author:          Tilen MAJERLE <tilen@majerle.eu>
 */
#include "gsm/gsm_private.h"
#include "gsm/gsm_conn.h"
#include "gsm/gsm_mem.h"
#include "gsm/gsm_timeout.h"

#if GSM_CFG_CONN || __DOXYGEN__

/**
 * \brief           Timeout callback for connection
 * \param[in]       arg: Timeout callback custom argument
 */
static void
conn_timeout_cb(void* arg) {
    gsm_conn_p conn = arg;                      /* Argument is actual connection */

    if (conn->status.f.active) {                /* Handle only active connections */
        gsm.evt.type = GSM_EVT_CONN_POLL;       /* Poll connection event */
        gsm.evt.evt.conn_poll.conn = conn;      /* Set connection pointer */
        gsmi_send_conn_cb(conn, NULL);          /* Send connection callback */
        
        gsm_timeout_add(GSM_CFG_CONN_POLL_INTERVAL, conn_timeout_cb, conn); /* Schedule timeout again */
        GSM_DEBUGF(GSM_CFG_DBG_CONN | GSM_DBG_TYPE_TRACE,
            "[CONN] Poll event: %p\r\n", conn);
    }
}

/**
 * \brief           Start timeout function for connection
 * \param[in]       conn: Connection handle as user argument
 */
void
gsmi_conn_start_timeout(gsm_conn_p conn) {
    gsm_timeout_add(GSM_CFG_CONN_POLL_INTERVAL, conn_timeout_cb, conn); /* Add connection timeout */
}

/**
 * \brief           Get connection validation ID
 * \param[in]       conn: Connection handle
 * \return          Connection current validation ID
 */
uint8_t
conn_get_val_id(gsm_conn_p conn) {
    uint8_t val_id;
    GSM_CORE_PROTECT();                         /* Protect core */
    val_id = conn->val_id;
    GSM_CORE_UNPROTECT();                       /* Unprotect core */
    
    return val_id;
}

/**
 * \brief           Send data on already active connection of type UDP to specific remote IP and port
 * \note            In case IP and port values are not set, it will behave as normal send function (suitable for TCP too)
 * \param[in]       conn: Pointer to connection to send data
 * \param[in]       ip: Remote IP address for UDP connection
 * \param[in]       port: Remote port connection
 * \param[in]       data: Pointer to data to send
 * \param[in]       btw: Number of bytes to send
 * \param[out]      bw: Pointer to output variable to save number of sent data when successfully sent
 * \param[in]       fau: "Free After Use" flag. Set to `1` if stack should free the memory after data sent
 * \param[in]       blocking: Status whether command should be blocking or not
 * \return          \ref gsmOK on success, member of \ref gsmr_t enumeration otherwise
 */
static gsmr_t
conn_send(gsm_conn_p conn, const gsm_ip_t* const ip, gsm_port_t port, const void* data, size_t btw, size_t* const bw, uint8_t fau, const uint32_t blocking) {
    GSM_MSG_VAR_DEFINE(msg);                    /* Define variable for message */
    
    GSM_ASSERT("conn != NULL", conn != NULL);   /* Assert input parameters */
    GSM_ASSERT("data != NULL", data != NULL);   /* Assert input parameters */
    GSM_ASSERT("btw > 0", btw > 0);             /* Assert input parameters */
    
    if (bw != NULL) {
        *bw = 0;
    }
    
    GSM_MSG_VAR_ALLOC(msg);                     /* Allocate memory for variable */
    GSM_MSG_VAR_REF(msg).cmd_def = GSM_CMD_CIPSEND;
    
    GSM_MSG_VAR_REF(msg).msg.conn_send.conn = conn;
    GSM_MSG_VAR_REF(msg).msg.conn_send.data = data;
    GSM_MSG_VAR_REF(msg).msg.conn_send.btw = btw;
    GSM_MSG_VAR_REF(msg).msg.conn_send.bw = bw;
    GSM_MSG_VAR_REF(msg).msg.conn_send.remote_ip = ip;
    GSM_MSG_VAR_REF(msg).msg.conn_send.remote_port = port;
    GSM_MSG_VAR_REF(msg).msg.conn_send.fau = fau;
    GSM_MSG_VAR_REF(msg).msg.conn_send.val_id = conn_get_val_id(conn);
    
    return gsmi_send_msg_to_producer_mbox(&GSM_MSG_VAR_REF(msg), gsmi_initiate_cmd, blocking, 60000);   /* Send message to producer queue */
}

/**
 * \brief           Flush buffer on connection
 * \param[in]       conn: Connection to flush buffer on
 * \return          gsmOK if data flushed and put to queue, member of \ref gsmr_t otherwise
 */
static gsmr_t
flush_buff(gsm_conn_p conn) {
    gsmr_t res = gsmOK;
    GSM_CORE_PROTECT();                         /* Protect core */
    if (conn != NULL && conn->buff.buff != NULL) {  /* Do we have something ready? */
        /*
         * If there is nothing to write or if write was not successful,
         * simply free the memory and stop execution
         */
        if (conn->buff.ptr) {                   /* Anything to send at the moment? */
            res = conn_send(conn, NULL, 0, conn->buff.buff, conn->buff.ptr, NULL, 1, 0);
        } else {
            res = gsmERR;
        }
        if (res != gsmOK) {
            GSM_DEBUGF(GSM_CFG_DBG_CONN | GSM_DBG_TYPE_TRACE,
                "[CONN] Free write buffer: %p\r\n", (void *)conn->buff.buff);
            gsm_mem_free(conn->buff.buff);      /* Manually free memory */
        }
        conn->buff.buff = NULL;
    }
    GSM_CORE_UNPROTECT();                       /* Unprotect core */
    return res;
}

/**
 * \brief           Initialize connection module
 */
void
gsmi_conn_init(void) {
    
}

/**
 * \brief           Start a new connection of specific type
 * \param[out]      conn: Pointer to connection handle to set new connection reference in case of successful connection
 * \param[in]       type: Connection type. This parameter can be a value of \ref gsm_conn_type_t enumeration
 * \param[in]       host: Connection host. In case of IP, write it as string, ex. "192.168.1.1"
 * \param[in]       port: Connection port
 * \param[in]       arg: Pointer to user argument passed to connection if successfully connected
 * \param[in]       evt_fn: Callback function for this connection
 * \param[in]       blocking: Status whether command should be blocking or not
 * \return          \ref gsmOK on success, member of \ref gsmr_t enumeration otherwise
 */
gsmr_t
gsm_conn_start(gsm_conn_p* conn, gsm_conn_type_t type, const char* const host, gsm_port_t port, void* const arg, gsm_evt_fn evt_fn, const uint32_t blocking) {
    GSM_MSG_VAR_DEFINE(msg);                    /* Define variable for message */

    GSM_ASSERT("host != NULL", host != NULL);   /* Assert input parameters */
    GSM_ASSERT("port > 0", port > 0);           /* Assert input parameters */
    GSM_ASSERT("evt_fn != NULL", evt_fn != NULL);   /* Assert input parameters */

    GSM_MSG_VAR_ALLOC(msg);                     /* Allocate memory for variable */
    GSM_MSG_VAR_REF(msg).cmd_def = GSM_CMD_CIPSTART;
    GSM_MSG_VAR_REF(msg).cmd = GSM_CMD_CIPSTATUS;
    GSM_MSG_VAR_REF(msg).msg.conn_start.num = GSM_CFG_MAX_CONNS;/* Set maximal value as invalid number */
    GSM_MSG_VAR_REF(msg).msg.conn_start.conn = conn;
    GSM_MSG_VAR_REF(msg).msg.conn_start.type = type;
    GSM_MSG_VAR_REF(msg).msg.conn_start.host = host;
    GSM_MSG_VAR_REF(msg).msg.conn_start.port = port;
    GSM_MSG_VAR_REF(msg).msg.conn_start.evt_func = evt_fn;
    GSM_MSG_VAR_REF(msg).msg.conn_start.arg = arg;
    
    return gsmi_send_msg_to_producer_mbox(&GSM_MSG_VAR_REF(msg), gsmi_initiate_cmd, blocking, 60000);   /* Send message to producer queue */
}

/**
 * \brief           Close specific or all connections
 * \param[in]       conn: Connection handle to close. Set to NULL if you want to close all connections.
 * \param[in]       blocking: Status whether command should be blocking or not
 * \return          \ref gsmOK on success, member of \ref gsmr_t enumeration otherwise
 */
gsmr_t
gsm_conn_close(gsm_conn_p conn, const uint32_t blocking) {
    gsmr_t res = gsmOK;
    GSM_MSG_VAR_DEFINE(msg);                    /* Define variable for message */
    
    GSM_ASSERT("conn != NULL", conn != NULL);   /* Assert input parameters */
    
    GSM_CORE_PROTECT();                         /* Protect core */
    if (conn->status.f.in_closing || !conn->status.f.active) {  /* Check if already in closing mode or already closed */
        res = gsmERR;
    }
    GSM_CORE_UNPROTECT();                       /* Unprotect core */
    if (res != gsmOK) {
        return res;
    }
    
    /* Proceed with close event at this point! */
    GSM_MSG_VAR_ALLOC(msg);                     /* Allocate memory for variable */
    GSM_MSG_VAR_REF(msg).cmd_def = GSM_CMD_CIPCLOSE;
    GSM_MSG_VAR_REF(msg).msg.conn_close.conn = conn;
    GSM_MSG_VAR_REF(msg).msg.conn_close.val_id = conn_get_val_id(conn);
    
    flush_buff(conn);                           /* First flush buffer */
    res = gsmi_send_msg_to_producer_mbox(&GSM_MSG_VAR_REF(msg), gsmi_initiate_cmd, blocking, 1000); /* Send message to producer queue */
    if (res == gsmOK && !blocking) {            /* Function succedded in non-blocking mode */
        GSM_CORE_PROTECT();                     /* Protect core */
        GSM_DEBUGF(GSM_CFG_DBG_CONN | GSM_DBG_TYPE_TRACE,
            "[CONN] Connection %d set to closing state\r\n", (int)conn->num);
        conn->status.f.in_closing = 1;          /* Connection is in closing mode but not yet closed */
        GSM_CORE_UNPROTECT();                   /* Unprotect core */
    }
    return res;
}

/**
 * \brief           Send data on active connection of type UDP to specific remote IP and port
 * \note            In case IP and port values are not set, it will behave as normal send function (suitable for TCP too)
 * \param[in]       conn: Connection handle to send data
 * \param[in]       ip: Remote IP address for UDP connection
 * \param[in]       port: Remote port connection
 * \param[in]       data: Pointer to data to send
 * \param[in]       btw: Number of bytes to send
 * \param[out]      bw: Pointer to output variable to save number of sent data when successfully sent
 * \param[in]       blocking: Status whether command should be blocking or not
 * \return          \ref gsmOK on success, member of \ref gsmr_t enumeration otherwise
 */
gsmr_t
gsm_conn_sendto(gsm_conn_p conn, const gsm_ip_t* const ip, gsm_port_t port, const void* data, size_t btw, size_t* bw, const uint32_t blocking) {
    GSM_ASSERT("conn != NULL", conn != NULL);   /* Assert input parameters */

    flush_buff(conn);                           /* Flush currently written memory if exists */
    return conn_send(conn, ip, port, data, btw, bw, 0, blocking);
}

/**
 * \brief           Send data on already active connection either as client or server
 * \param[in]       conn: Connection handle to send data
 * \param[in]       data: Data to send
 * \param[in]       btw: Number of bytes to send
 * \param[out]      bw: Pointer to output variable to save number of sent data when successfully sent.
 *                      Parameter value might not be accurate if you combine \ref gsm_conn_write and \ref gsm_conn_send functions
 * \param[in]       blocking: Status whether command should be blocking or not
 * \return          \ref gsmOK on success, member of \ref gsmr_t enumeration otherwise
 */
gsmr_t
gsm_conn_send(gsm_conn_p conn, const void* data, size_t btw, size_t* const bw, const uint32_t blocking) {
    gsmr_t res;
    const uint8_t* d = data;

    GSM_ASSERT("conn != NULL", conn != NULL);   /* Assert input parameters */
    GSM_ASSERT("data != NULL", data != NULL);   /* Assert input parameters */
    GSM_ASSERT("btw > 0", btw > 0);             /* Assert input parameters */

    GSM_CORE_PROTECT();                         /* Protect core */
    if (conn->buff.buff != NULL) {              /* Check if memory available */
        size_t to_copy;
        to_copy = GSM_MIN(btw, conn->buff.len - conn->buff.ptr);
        if (to_copy) {
            GSM_MEMCPY(&conn->buff.buff[conn->buff.ptr], d, to_copy);
            conn->buff.ptr += to_copy;
            d += to_copy;
            btw -= to_copy;
        }
    }
    GSM_CORE_UNPROTECT();                       /* Unprotect core */
    res = flush_buff(conn);                     /* Flush currently written memory if exists */
    if (btw) {                                  /* Check for remaining data */
        res = conn_send(conn, NULL, 0, d, btw, bw, 0, blocking);
    }
    return res;
}

/**
 * \brief           Notify connection about received data which means connection is ready to accept more data
 * 
 *                  Once data reception is confirmed, stack will try to send more data to user.
 * 
 * \note            Since this feature is not supported yet by AT commands, function is only prototype
 *                  and should be used in connection callback when data are received
 *
 * \note            Function is not thread safe and may only be called from callback function
 *
 * \param[in]       conn: Connection handle
 * \param[in]       pbuf: Packet buffer received on connection
 * \return          \ref gsmOK on success, member of \ref gsmr_t enumeration otherwise
 */
gsmr_t
gsm_conn_recved(gsm_conn_p conn, gsm_pbuf_p pbuf) {
#if GSM_CFG_CONN_MANUAL_TCP_RECEIVE
    size_t len;
    len = gsm_pbuf_length(pbuf, 1);             /* Get length of pbuf */
    if (conn->tcp_available_data > len) {
        conn->tcp_available_data -= len;        /* Decrease for available length */
        if (conn->tcp_available_data) {
            /* Start new manual receive here... */
        }
    }
#else /* GSM_CFG_CONN_MANUAL_TCP_RECEIVE */
    GSM_UNUSED(conn);
    GSM_UNUSED(pbuf);
#endif /* !GSM_CFG_CONN_MANUAL_TCP_RECEIVE */
    return gsmOK;
}

/**
 * \brief           Set argument variable for connection
 * \param[in]       conn: Connection handle to set argument
 * \param[in]       arg: Pointer to argument
 * \return          \ref gsmOK on success, member of \ref gsmr_t enumeration otherwise
 * \sa              gsm_conn_get_arg
 */
gsmr_t
gsm_conn_set_arg(gsm_conn_p conn, void* const arg) {
    GSM_CORE_PROTECT();                         /* Protect core */
    conn->arg = arg;                            /* Set argument for connection */
    GSM_CORE_UNPROTECT();                       /* Unprotect core */
    return gsmOK;
}

/**
 * \brief           Get user defined connection argument
 * \param[in]       conn: Connection handle to get argument
 * \return          User argument
 * \sa              gsm_conn_set_arg
 */
void *
gsm_conn_get_arg(gsm_conn_p conn) {
    void* arg;
    GSM_CORE_PROTECT();                         /* Protect core */
    arg = conn->arg;                            /* Set argument for connection */
    GSM_CORE_UNPROTECT();                       /* Unprotect core */
    return arg;
}

/**
 * \brief           Gets connections status
 * \param[in]       blocking: Status whether command should be blocking or not
 * \return          \ref gsmOK on success, member of \ref gsmr_t enumeration otherwise
 */
gsmr_t
gsm_get_conns_status(uint32_t blocking) {
    GSM_MSG_VAR_DEFINE(msg);                    /* Define variable for message */
    
    GSM_MSG_VAR_ALLOC(msg);                     /* Allocate memory for variable */
    GSM_MSG_VAR_REF(msg).cmd_def = GSM_CMD_CIPSTATUS;
    
    return gsmi_send_msg_to_producer_mbox(&GSM_MSG_VAR_REF(msg), gsmi_initiate_cmd, blocking, 1000);    /* Send message to producer queue */
}

/**
 * \brief           Check if connection type is client
 * \param[in]       conn: Pointer to connection to check for status
 * \return          `1` on success, `0` otherwise
 */
uint8_t
gsm_conn_is_client(gsm_conn_p conn) {
    uint8_t res = 0;
    if (conn != NULL && gsmi_is_valid_conn_ptr(conn)) {
        GSM_CORE_PROTECT();                     /* Protect core */
        res = conn->status.f.active && conn->status.f.client;
        GSM_CORE_UNPROTECT();                   /* Unprotect core */
    }
    return res;
}

/**
 * \brief           Check if connection type is server
 * \param[in]       conn: Pointer to connection to check for status
 * \return          `1` on success, `0` otherwise
 */
uint8_t
gsm_conn_is_server(gsm_conn_p conn) {
    uint8_t res = 0;
    if (conn != NULL && gsmi_is_valid_conn_ptr(conn)) {
        GSM_CORE_PROTECT();
        res = conn->status.f.active && !conn->status.f.client;
        GSM_CORE_UNPROTECT();
    }
    return res;
}

/**
 * \brief           Check if connection is active
 * \param[in]       conn: Pointer to connection to check for status
 * \return          `1` on success, `0` otherwise
 */
uint8_t
gsm_conn_is_active(gsm_conn_p conn) {
    uint8_t res = 0;
    if (conn != NULL && gsmi_is_valid_conn_ptr(conn)) {
        GSM_CORE_PROTECT();
        res = conn->status.f.active;
        GSM_CORE_UNPROTECT();
    }
    return res;
}

/**
 * \brief           Check if connection is closed
 * \param[in]       conn: Pointer to connection to check for status
 * \return          `1` on success, `0` otherwise
 */
uint8_t
gsm_conn_is_closed(gsm_conn_p conn) {
    uint8_t res = 0;
    if (conn != NULL && gsmi_is_valid_conn_ptr(conn)) {
        GSM_CORE_PROTECT();
        res = !conn->status.f.active;
        GSM_CORE_UNPROTECT();
    }
    return res;
}

/**
 * \brief           Get the number from connection
 * \param[in]       conn: Connection pointer
 * \return          Connection number in case of success or -1 on failure
 */
int8_t
gsm_conn_getnum(gsm_conn_p conn) {
    int8_t res = -1;
    if (conn != NULL && gsmi_is_valid_conn_ptr(conn)) {
        /* Protection not needed as every connection has always the same number */
        res = conn->num;                        /* Get number */
    }
    return res;
}

/**
 * \brief           Get connection from connection based event
 * \param[in]       evt: Event which happened for connection
 * \return          Connection pointer on success, `NULL` otherwise
 */
gsm_conn_p
gsm_conn_get_from_evt(gsm_evt_t* evt) {
    if (evt->type == GSM_EVT_CONN_ACTIVE) {
        return gsm_evt_conn_active_get_conn(evt);
    } else if (evt->type == GSM_EVT_CONN_CLOSED) {
        return gsm_evt_conn_closed_get_conn(evt);
    } else if (evt->type == GSM_EVT_CONN_DATA_RECV) {
        return gsm_evt_conn_data_recv_get_conn(evt);
    } else if (evt->type == GSM_EVT_CONN_DATA_SEND) {
        return gsm_evt_conn_data_send_get_conn(evt);
    } else if (evt->type == GSM_EVT_CONN_POLL) {
        return gsm_evt_conn_poll_get_conn(evt);
    }
    return NULL;
}

/**
 * \brief           Write data to connection buffer and if it is full, send it non-blocking way
 * \note            This function may only be called from core (connection callbacks)
 * \param[in]       conn: Connection to write
 * \param[in]       data: Data to copy to write buffer
 * \param[in]       btw: Number of bytes to write
 * \param[in]       flush: Flush flag. Set to `1` if you want to send data immediatelly after copying
 * \param[out]      mem_available: Available memory size available in current write buffer.
 *                  When the buffer length is reached, current one is sent and a new one is automatically created.
 *                  If function returns gsmOK and *mem_available = 0, there was a problem
 *                  allocating a new buffer for next operation
 * \return          \ref gsmOK on success, member of \ref gsmr_t enumeration otherwise
 */
gsmr_t
gsm_conn_write(gsm_conn_p conn, const void* data, size_t btw, uint8_t flush, size_t* const mem_available) {
    size_t len;
    
    const uint8_t* d = data;
    
    GSM_ASSERT("conn != NULL", conn != NULL);   /* Assert input parameters */
    
    /*
     * Steps, performed in write process:
     * 
     * 1. Check if we have buffer already allocated and
     *      write data to the tail of buffer
     *   1.1. In case buffer is full, send it non-blocking,
     *      and enable freeing after it is sent
     * 2. Check how many bytes we can copy as single buffer directly and send
     * 3. Create last buffer and copy remaining data to it even if no remaining data
     *      This is useful when calling function with no parameters (len = 0)
     * 4. Flush (send) current buffer if necessary
     */
    
    /* Step 1 */
    if (conn->buff.buff != NULL) {
        len = GSM_MIN(conn->buff.len - conn->buff.ptr, btw);
        GSM_MEMCPY(&conn->buff.buff[conn->buff.ptr], d, len);
        
        d += len;
        btw -= len;
        conn->buff.ptr += len;
        
        /* Step 1.1 */
        if (conn->buff.ptr == conn->buff.len || flush) {
            /* Try to send to processing queue in non-blocking way */
            if (conn_send(conn, NULL, 0, conn->buff.buff, conn->buff.ptr, NULL, 1, 0) != gsmOK) {
                GSM_DEBUGF(GSM_CFG_DBG_CONN | GSM_DBG_TYPE_TRACE,
                    "[CONN] Free write buffer: %p\r\n", conn->buff.buff);
                gsm_mem_free(conn->buff.buff);  /* Manually free memory */
            }
            conn->buff.buff = NULL;             /* Reset pointer */
        }
    }
    
    /* Step 2 */
    while (btw >= GSM_CFG_CONN_MAX_DATA_LEN) {
        uint8_t* buff;
        buff = gsm_mem_alloc(sizeof(*buff) * GSM_CFG_CONN_MAX_DATA_LEN);    /* Allocate memory */
        if (buff != NULL) {
            GSM_MEMCPY(buff, d, GSM_CFG_CONN_MAX_DATA_LEN); /* Copy data to buffer */
            if (conn_send(conn, NULL, 0, buff, GSM_CFG_CONN_MAX_DATA_LEN, NULL, 1, 0) != gsmOK) {
                GSM_DEBUGF(GSM_CFG_DBG_CONN | GSM_DBG_TYPE_TRACE,
                    "[CONN] Free write buffer: %p\r\n", (void *)buff);
                gsm_mem_free(buff);             /* Manually free memory */
                buff = NULL;
                return gsmERRMEM;
            }
        } else {
            return gsmERRMEM;
        }
        
        btw -= GSM_CFG_CONN_MAX_DATA_LEN;       /* Decrease remaining length */
        d += GSM_CFG_CONN_MAX_DATA_LEN;         /* Advance data pointer */
    }
    
    /* Step 3 */
    if (conn->buff.buff == NULL) {
        conn->buff.buff = gsm_mem_alloc(sizeof(*conn->buff.buff) * GSM_CFG_CONN_MAX_DATA_LEN);  /* Allocate memory for temp buffer */
        conn->buff.len = GSM_CFG_CONN_MAX_DATA_LEN;
        conn->buff.ptr = 0;
        
        GSM_DEBUGW(GSM_CFG_DBG_CONN | GSM_DBG_TYPE_TRACE, conn->buff.buff != NULL,
            "[CONN] New write buffer allocated, addr = %p\r\n", conn->buff.buff);
        GSM_DEBUGW(GSM_CFG_DBG_CONN | GSM_DBG_TYPE_TRACE, conn->buff.buff == NULL,
            "[CONN] Cannot allocate new write buffer\r\n");
    }
    if (btw) {
        if (conn->buff.buff != NULL) {
            GSM_MEMCPY(conn->buff.buff, d, btw);    /* Copy data to memory */
            conn->buff.ptr = btw;
        } else {
            return gsmERRMEM;
        }
    }
    
    /* Step 4 */
    if (flush && conn->buff.buff != NULL) {
        flush_buff(conn);
    }
    
    /* Calculate number of available memory after write operation */
    if (mem_available != NULL) {
        if (conn->buff.buff != NULL) {
            *mem_available = conn->buff.len - conn->buff.ptr;
        } else {
            *mem_available = 0;
        }
    }
    return gsmOK;
}

/**
 * \brief           Get total number of bytes ever received on connection and sent to user
 * \param[in]       conn: Connection handle
 */
size_t
gsm_conn_get_total_recved_count(gsm_conn_p conn) {
    size_t tot;

    GSM_ASSERT("conn != NULL", conn != NULL);   /* Assert input parameters */

    GSM_CORE_PROTECT();                         /* Protect core */
    tot = conn->total_recved;                   /* Get total received bytes */
    GSM_CORE_UNPROTECT();                       /* Unprotect core */

    return tot;
}



/**
 * \brief           Get connection remote IP address
 * \param[in]       conn: Connection handle
 * \param[out]      ip: Pointer to IP output handle
 * \return          `1` on success, `0` otherwise
 */
uint8_t
gsm_conn_get_remote_ip(gsm_conn_p conn, gsm_ip_t* ip) {
    if (conn != NULL && ip != NULL) {
        GSM_CORE_PROTECT();                     /* Protect core */
        GSM_MEMCPY(ip, &conn->remote_ip, sizeof(*ip));  /* Copy data */
        GSM_CORE_UNPROTECT();                   /* Unprotect core */
        return 1;
    }
    return 0;
}

/**
 * \brief           Get connection remote port number
 * \param[in]       conn: Connection handle
 * \return          Port number on success, `0` otherwise
 */
gsm_port_t
gsm_conn_get_remote_port(gsm_conn_p conn) {
    gsm_port_t port = 0;
    if (conn != NULL) {
        GSM_CORE_PROTECT();                     /* Protect core */
        port = conn->remote_port;
        GSM_CORE_UNPROTECT();                   /* Unprotect core */
    }
    return port;
}

/**
 * \brief           Get connection local port number
 * \param[in]       conn: Connection handle
 * \return          Port number on success, `0` otherwise
 */
gsm_port_t
gsm_conn_get_local_port(gsm_conn_p conn) {
    gsm_port_t port = 0;
    if (conn != NULL) {
        GSM_CORE_PROTECT();                     /* Protect core */
        port = conn->local_port;
        GSM_CORE_UNPROTECT();                   /* Unprotect core */
    }
    return port;
}


#endif /* GSM_CFG_CONN || __DOXYGEN__ */