/*
 * Copyright (C) 2015 BlueKitchen GmbH
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 *
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 * 3. Neither the name of the copyright holders nor the names of
 *    contributors may be used to endorse or promote products derived
 *    from this software without specific prior written permission.
 * 4. Any redistribution, use, or modification is done solely for
 *    personal benefit and not for any commercial purpose or for
 *    monetary gain.
 *
 * THIS SOFTWARE IS PROVIDED BY BLUEKITCHEN GMBH AND CONTRIBUTORS
 * ``AS IS'' AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
 * LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS
 * FOR A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL MATTHIAS
 * RINGWALD OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT,
 * INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING,
 * BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS
 * OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED
 * AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY,
 * OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF
 * THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF
 * SUCH DAMAGE.
 *
 * Please inquire about commercial licensing options at 
 * contact@bluekitchen-gmbh.com
 *
 */

/*
 *  btstack_uart_wiced.c
 *
 *  UART driver for WICED
 */

#define __BTSTACK_FILE__ "btstack_uart_wiced.c"

#include "btstack_config.h"
#include "btstack_run_loop_wiced.h"

#include "btstack_debug.h"
#include "hci.h"
#include "hci_transport.h"
#include "platform_bluetooth.h"

#include "wiced.h"

#include <stdio.h>
#include <string.h>

// priority higher than WIFI to make sure RTS is set
#define WICED_BT_UART_THREAD_PRIORITY        (WICED_NETWORK_WORKER_PRIORITY - 2)
#define WICED_BT_UART_THREAD_STACK_SIZE      300

// assert pre-buffer for packet type is available
#if !defined(HCI_OUTGOING_PRE_BUFFER_SIZE) || (HCI_OUTGOING_PRE_BUFFER_SIZE == 0)
#error HCI_OUTGOING_PRE_BUFFER_SIZE not defined. Please update hci.h
#endif

// Default of 512 bytes should be fine. Only needed with BTSTACK_FLOW_CONTROL_UART
#ifndef RX_RING_BUFFER_SIZE
#define RX_RING_BUFFER_SIZE 512
#endif

// Use BTSTACK_FLOW_CONTROL_MANUAL is used when Bluetooth RTS/CTS are not connected to UART RTS/CTS pins
// E.g. on RedBear Duo - WICED_BT_UART_MANUAL_CTS_RTS is defined

static enum {
    BTSTACK_FLOW_CONTROL_OFF,
    BTSTACK_FLOW_CONTROL_UART,
    BTSTACK_FLOW_CONTROL_MANUAL,
} btstack_flow_control_mode;

static wiced_result_t btstack_uart_wiced_rx_worker_receive_block(void * arg);

static int                   btstack_uart_wiced_initialized;
static int                   btstack_uart_wiced_opened;

static wiced_worker_thread_t tx_worker_thread;
static const uint8_t *       tx_worker_data_buffer;
static uint16_t              tx_worker_data_size;

static wiced_worker_thread_t rx_worker_thread;
static uint8_t *             rx_worker_read_buffer;
static uint16_t              rx_worker_read_size;

static wiced_ring_buffer_t   rx_ring_buffer;
static uint8_t               rx_data[RX_RING_BUFFER_SIZE];

// uart config
static const btstack_uart_config_t * uart_config;

// callbacks
static void (*block_sent)(void);
static void (*block_received)(void);

// workaround for API change in WICED
static uint32_t btstack_uart_wiced_read_bytes(uint8_t * buffer, uint32_t bytes_to_read, uint32_t timeout){
    uint32_t bytes = bytes_to_read;
#ifdef WICED_UART_READ_DOES_NOT_RETURN_BYTES_READ
    // older API passes in number of bytes to read (checked in 3.3.1 and 3.4.0)
    if (timeout != WICED_NEVER_TIMEOUT){
        log_error("btstack_uart_wiced_read_bytes called with timeout != WICED_NEVER_TIMEOUT -> not supported in older WICED Versions");
    }
    platform_uart_receive_bytes(wiced_bt_uart_driver, buffer, bytes_to_read, timeout);
#else
    // newer API uses pointer to return number of read bytes
    platform_uart_receive_bytes(wiced_bt_uart_driver, buffer, &bytes, timeout);
#endif
    return bytes;
}

// executed on main run loop
static wiced_result_t btstack_uart_wiced_main_notify_block_send(void *arg){
    if (block_sent){
        block_sent();
    }
    return WICED_SUCCESS;
}

// executed on main run loop
static wiced_result_t btstack_uart_wiced_main_notify_block_read(void *arg){
    if (block_received){
        block_received();
    }
    return WICED_SUCCESS;
}

// executed on tx worker thread
static wiced_result_t btstack_uart_wiced_tx_worker_send_block(void * arg){
    // wait for CTS to become low in manual flow control mode
    if (btstack_flow_control_mode == BTSTACK_FLOW_CONTROL_MANUAL && wiced_bt_uart_pins[WICED_BT_PIN_UART_CTS]){
        while (platform_gpio_input_get(wiced_bt_uart_pins[WICED_BT_PIN_UART_CTS]) == WICED_TRUE){
            wiced_rtos_delay_milliseconds(10);
        }        
    }

    // blocking send
    platform_uart_transmit_bytes(wiced_bt_uart_driver, tx_worker_data_buffer, tx_worker_data_size);

    // let transport know
    btstack_run_loop_wiced_execute_code_on_main_thread(&btstack_uart_wiced_main_notify_block_send, NULL);
    return WICED_SUCCESS;
}

// executed on rx worker thread
static wiced_result_t btstack_uart_wiced_rx_worker_receive_block(void * arg){

    if (btstack_flow_control_mode == BTSTACK_FLOW_CONTROL_MANUAL && wiced_bt_uart_pins[WICED_BT_PIN_UART_CTS]){
        platform_gpio_output_low(wiced_bt_uart_pins[WICED_BT_PIN_UART_RTS]);
    }

    btstack_uart_wiced_read_bytes(rx_worker_read_buffer, rx_worker_read_size, WICED_NEVER_TIMEOUT);

    if (btstack_flow_control_mode == BTSTACK_FLOW_CONTROL_MANUAL && wiced_bt_uart_pins[WICED_BT_PIN_UART_CTS]){
        platform_gpio_output_high(wiced_bt_uart_pins[WICED_BT_PIN_UART_RTS]);
    }

    // let transport know
    btstack_run_loop_wiced_execute_code_on_main_thread(&btstack_uart_wiced_main_notify_block_read, NULL);
    return WICED_SUCCESS;
}

static int btstack_uart_wiced_init(const btstack_uart_config_t * config){

    if (btstack_uart_wiced_initialized) {
        log_info("init / already initialized");
        return 0;
    }
    btstack_uart_wiced_initialized = 1;

    uart_config = config;
    btstack_uart_wiced_opened = 0;

#ifdef ENABLE_H5
    log_info("init / h5 supported");
#else
    log_info("init / h5 not supported");
#endif

    // determine flow control mode based on hardware config and uart config
    if (uart_config->flowcontrol){
#ifdef WICED_BT_UART_MANUAL_CTS_RTS
        btstack_flow_control_mode = BTSTACK_FLOW_CONTROL_MANUAL;
#else
        btstack_flow_control_mode = BTSTACK_FLOW_CONTROL_UART;
#endif
    } else {
        btstack_flow_control_mode = BTSTACK_FLOW_CONTROL_OFF;
    }
    return 0;
}

static int btstack_uart_wiced_open(void){

    if (btstack_uart_wiced_opened) {
        log_info("open (already)");
        return 0;
    }
    btstack_uart_wiced_opened = 1;

    log_info("open");

    // UART config
    wiced_uart_config_t wiced_uart_config =
    {
        .baud_rate    = uart_config->baudrate,
        .data_width   = DATA_WIDTH_8BIT,
        .parity       = NO_PARITY,
        .stop_bits    = STOP_BITS_1,
    };

    if (btstack_flow_control_mode == BTSTACK_FLOW_CONTROL_UART){
        wiced_uart_config.flow_control = FLOW_CONTROL_CTS_RTS;
    } else {
        wiced_uart_config.flow_control = FLOW_CONTROL_DISABLED;
    }
    wiced_ring_buffer_t * ring_buffer = NULL;

    // configure HOST and DEVICE WAKE PINs
    platform_gpio_init(wiced_bt_control_pins[WICED_BT_PIN_HOST_WAKE], INPUT_HIGH_IMPEDANCE);
    platform_gpio_init(wiced_bt_control_pins[WICED_BT_PIN_DEVICE_WAKE], OUTPUT_PUSH_PULL);
    platform_gpio_output_low(wiced_bt_control_pins[WICED_BT_PIN_DEVICE_WAKE]);

    /* Configure Reg Enable pin to output. Set to HIGH */
    if (wiced_bt_control_pins[ WICED_BT_PIN_POWER ]){
        platform_gpio_init( wiced_bt_control_pins[ WICED_BT_PIN_POWER ], OUTPUT_OPEN_DRAIN_PULL_UP );
        platform_gpio_output_high( wiced_bt_control_pins[ WICED_BT_PIN_POWER ] );
    }

    wiced_rtos_delay_milliseconds( 100 );

    // Configure RTS
    if (wiced_bt_uart_pins[WICED_BT_PIN_UART_RTS]){
        switch (btstack_flow_control_mode){
            case BTSTACK_FLOW_CONTROL_OFF:
                // configure RTS pin as output and set to low - always on
                platform_gpio_init(wiced_bt_uart_pins[WICED_BT_PIN_UART_RTS], OUTPUT_PUSH_PULL);
                platform_gpio_output_low(wiced_bt_uart_pins[WICED_BT_PIN_UART_RTS]);
                break;
            case BTSTACK_FLOW_CONTROL_UART:
                // configuration done by platform_uart_init
                break;
            case BTSTACK_FLOW_CONTROL_MANUAL:
                // configure RTS pin as output and set to high - controlled by btstack_uart_wiced_rx_worker_receive_block
                platform_gpio_init(wiced_bt_uart_pins[WICED_BT_PIN_UART_RTS], OUTPUT_PUSH_PULL);
                platform_gpio_output_high(wiced_bt_uart_pins[WICED_BT_PIN_UART_RTS]);
                break;
        }
    }

    // Configure CTS
    if (wiced_bt_uart_pins[WICED_BT_PIN_UART_CTS]){
        switch (btstack_flow_control_mode){
            case BTSTACK_FLOW_CONTROL_OFF:
                // don't care
                break;
            case BTSTACK_FLOW_CONTROL_UART:
                // configuration done by platform_uart_init
                break;
            case BTSTACK_FLOW_CONTROL_MANUAL:
                // configure CTS to input, pull-up
                platform_gpio_init(wiced_bt_uart_pins[WICED_BT_PIN_UART_CTS], INPUT_PULL_UP);
                break;
        }
    }

    // use ring buffer to allow to receive RX_RING_BUFFER_SIZE/2 addition bytes - not needed with hardware UART
    if (btstack_flow_control_mode != BTSTACK_FLOW_CONTROL_UART){
        ring_buffer_init((wiced_ring_buffer_t *) &rx_ring_buffer, (uint8_t*) rx_data, sizeof( rx_data ) );
        ring_buffer = (wiced_ring_buffer_t *) &rx_ring_buffer;
    }

    platform_uart_init( wiced_bt_uart_driver, wiced_bt_uart_peripheral, &wiced_uart_config, ring_buffer );


    // Reset Bluetooth via RESET line. Fallback to toggling POWER otherwise
    if ( wiced_bt_control_pins[ WICED_BT_PIN_RESET ]){
        platform_gpio_init( wiced_bt_control_pins[ WICED_BT_PIN_RESET ], OUTPUT_PUSH_PULL );
        platform_gpio_output_high( wiced_bt_control_pins[ WICED_BT_PIN_RESET ] );

        platform_gpio_output_low( wiced_bt_control_pins[ WICED_BT_PIN_RESET ] );
        wiced_rtos_delay_milliseconds( 100 );
        platform_gpio_output_high( wiced_bt_control_pins[ WICED_BT_PIN_RESET ] );
    }
    else if ( wiced_bt_control_pins[ WICED_BT_PIN_POWER ]){
        platform_gpio_output_low( wiced_bt_control_pins[ WICED_BT_PIN_POWER ] );
        wiced_rtos_delay_milliseconds( 100 );
        platform_gpio_output_high( wiced_bt_control_pins[ WICED_BT_PIN_POWER ] );
    }

    // wait for Bluetooth to start up
    wiced_rtos_delay_milliseconds( 500 );

    // create worker threads for rx/tx. only single request is posted to their queues
    wiced_rtos_create_worker_thread(&tx_worker_thread, WICED_BT_UART_THREAD_PRIORITY, WICED_BT_UART_THREAD_STACK_SIZE, 1);
    wiced_rtos_create_worker_thread(&rx_worker_thread, WICED_BT_UART_THREAD_PRIORITY, WICED_BT_UART_THREAD_STACK_SIZE, 1);

    // tx is ready
    tx_worker_data_size = 0;

    return 0;
}

static int btstack_uart_wiced_close(void){
    // not implemented
    return 0;
}

static void btstack_uart_wiced_set_block_received( void (*block_handler)(void)){
    block_received = block_handler;
}

static void btstack_uart_wiced_set_block_sent( void (*block_handler)(void)){
    block_sent = block_handler;
}

static int btstack_uart_wiced_set_baudrate(uint32_t baudrate){

#if defined(_STM32F205RGT6_) || defined(STM32F40_41xxx)

    // directly use STM peripheral functions to change baud rate dynamically
    
    // set TX to high
    log_info("set baud %u", (int) baudrate);
    const platform_gpio_t* gpio = wiced_bt_uart_pins[WICED_BT_PIN_UART_TX];
    platform_gpio_output_high(gpio);

    // reconfigure TX pin as GPIO
    GPIO_InitTypeDef gpio_init_structure;
    gpio_init_structure.GPIO_Speed = GPIO_Speed_50MHz;
    gpio_init_structure.GPIO_Mode  = GPIO_Mode_OUT;
    gpio_init_structure.GPIO_OType = GPIO_OType_PP;
    gpio_init_structure.GPIO_PuPd  = GPIO_PuPd_NOPULL;
    gpio_init_structure.GPIO_Pin   = (uint32_t) ( 1 << gpio->pin_number );
    GPIO_Init( gpio->port, &gpio_init_structure );

    // disable USART
    USART_Cmd( wiced_bt_uart_peripheral->port, DISABLE );

    // setup init structure
    USART_InitTypeDef uart_init_structure;
    uart_init_structure.USART_Mode       = USART_Mode_Rx | USART_Mode_Tx;
    uart_init_structure.USART_BaudRate   = baudrate;
    uart_init_structure.USART_WordLength = USART_WordLength_8b;
    uart_init_structure.USART_StopBits   = USART_StopBits_1;
    uart_init_structure.USART_Parity     = USART_Parity_No;

    if (btstack_flow_control_mode == BTSTACK_FLOW_CONTROL_UART){
        uart_init_structure.USART_HardwareFlowControl = USART_HardwareFlowControl_RTS_CTS;
    } else {
        uart_init_structure.USART_HardwareFlowControl = USART_HardwareFlowControl_None;
    }
    USART_Init(wiced_bt_uart_peripheral->port, &uart_init_structure);

    // enable USART again
    USART_Cmd( wiced_bt_uart_peripheral->port, ENABLE );

    // set TX pin as USART again
    gpio_init_structure.GPIO_Mode  = GPIO_Mode_AF;
    GPIO_Init( gpio->port, &gpio_init_structure );

#else
    log_error("btstack_uart_wiced_set_baudrate not implemented for this WICED Platform");
#endif

    // without flowcontrol, wait a bit to make sure Broadcom module is ready again
    if (btstack_flow_control_mode == BTSTACK_FLOW_CONTROL_OFF){
        wiced_rtos_delay_milliseconds( 100 );
    }

    return 0;
}

static int btstack_uart_wiced_set_parity(int parity){
    log_error("set_parity(%u) not implemented", parity);
    return 0;
}

static void btstack_uart_wiced_send_block(const uint8_t *buffer, uint16_t length){
    // log_info("send block, size %u", length);
    // store in request
    tx_worker_data_buffer = buffer;
    tx_worker_data_size = length;
    wiced_rtos_send_asynchronous_event(&tx_worker_thread, &btstack_uart_wiced_tx_worker_send_block, NULL);    
}

static void btstack_uart_wiced_receive_block(uint8_t *buffer, uint16_t len){
    // log_info("receive block, size %u", len);
    rx_worker_read_buffer = buffer;
    rx_worker_read_size   = len;
    wiced_rtos_send_asynchronous_event(&rx_worker_thread, &btstack_uart_wiced_rx_worker_receive_block, NULL);    
}


// SLIP Implementation Start
#ifdef ENABLE_H5

#include "btstack_slip.h"

// max size of outgoing SLIP chunks 
#define SLIP_TX_CHUNK_LEN   128

#define SLIP_RECEIVE_BUFFER_SIZE 128

// encoded SLIP chunk
static uint8_t   btstack_uart_slip_outgoing_buffer[SLIP_TX_CHUNK_LEN+1];

// block read
static uint8_t         btstack_uart_slip_receive_buffer[SLIP_RECEIVE_BUFFER_SIZE];
static uint16_t        btstack_uart_slip_receive_pos;
static uint16_t        btstack_uart_slip_receive_len;
static uint16_t        btstack_uart_slip_receive_frame_size;

// callbacks
static void (*frame_sent)(void);
static void (*frame_received)(uint16_t frame_size);

// -----------------------------
// SLIP DECODING

// executed on main run loop
static wiced_result_t btstack_uart_wiced_main_notify_frame_received(void *arg){
    if (frame_received){
        frame_received(btstack_uart_slip_receive_frame_size);
    }
    return WICED_SUCCESS;
}

// @returns frame size if complete frame decoded and delivered
static uint16_t btstack_uart_wiced_process_buffer(void){
    // log_debug("process buffer: pos %u, len %u", btstack_uart_slip_receive_pos, btstack_uart_slip_receive_len);

    uint16_t frame_size = 0;
    while (btstack_uart_slip_receive_pos < btstack_uart_slip_receive_len && frame_size == 0){
        btstack_slip_decoder_process(btstack_uart_slip_receive_buffer[btstack_uart_slip_receive_pos++]);
        frame_size = btstack_slip_decoder_frame_size();
    }

#if 0
    // reset buffer if fully processed
    if (btstack_uart_slip_receive_pos == btstack_uart_slip_receive_len ){
        btstack_uart_slip_receive_len = 0;
        btstack_uart_slip_receive_pos = 0;
    }
#endif

    return frame_size;
}

// executed on tx worker thread
static wiced_result_t btstack_uart_wiced_rx_worker_receive_frame(void * arg){
    
    // manual flow control, clear RTS
    if (btstack_flow_control_mode == BTSTACK_FLOW_CONTROL_MANUAL && wiced_bt_uart_pins[WICED_BT_PIN_UART_CTS]){
        platform_gpio_output_low(wiced_bt_uart_pins[WICED_BT_PIN_UART_RTS]);
    }

    // first time, we wait for a single byte to avoid polling until frame has started
    btstack_uart_wiced_read_bytes(btstack_uart_slip_receive_buffer, 1, WICED_NEVER_TIMEOUT);
    btstack_slip_decoder_process(btstack_uart_slip_receive_buffer[0]);

    // however, that's certainly not enough to receive a complete SLIP frame, now, try reading with low timeout
    uint16_t frame_size = 0;
    while (!frame_size){
        btstack_uart_slip_receive_pos = 0;
        btstack_uart_slip_receive_len = btstack_uart_wiced_read_bytes(btstack_uart_slip_receive_buffer, 1, WICED_NEVER_TIMEOUT);
        frame_size = btstack_uart_wiced_process_buffer();
    }

    // raise RTS again
    if (btstack_flow_control_mode == BTSTACK_FLOW_CONTROL_MANUAL && wiced_bt_uart_pins[WICED_BT_PIN_UART_CTS]){
        platform_gpio_output_high(wiced_bt_uart_pins[WICED_BT_PIN_UART_RTS]);
    }

    // let transport know
    btstack_uart_slip_receive_frame_size = frame_size;
    btstack_run_loop_wiced_execute_code_on_main_thread(&btstack_uart_wiced_main_notify_frame_received, NULL);
    return WICED_SUCCESS;
}

static void btstack_uart_wiced_receive_frame(uint8_t *buffer, uint16_t len){
    log_debug("receive frame, size %u", len);

    // setup SLIP decoder
    btstack_slip_decoder_init(buffer, len);
    // process bytes received in earlier read. might deliver packet, which in turn will call us again. 
    // just make sure to exit right away
    if (btstack_uart_slip_receive_len){
        int frame_size = btstack_uart_wiced_process_buffer();
        if (frame_size) {
            if (frame_received) {
                (*frame_received)(frame_size);
            }
            return;
        }
    }
    // receive frame on worker thread
    wiced_rtos_send_asynchronous_event(&rx_worker_thread, &btstack_uart_wiced_rx_worker_receive_frame, NULL);    
}

// -----------------------------
// SLIP ENCODING

// executed on main run loop
static wiced_result_t btstack_uart_wiced_main_notify_frame_sent(void *arg){
    if (frame_sent){
        frame_sent();
    }
    return WICED_SUCCESS;
}

// executed on tx worker thread
static wiced_result_t btstack_uart_wiced_tx_worker_send_frame(void * arg){
    while (btstack_slip_encoder_has_data()){
        // encode chunk
        uint16_t pos = 0;
        while (btstack_slip_encoder_has_data() & (pos < SLIP_TX_CHUNK_LEN)) {
            btstack_uart_slip_outgoing_buffer[pos++] = btstack_slip_encoder_get_byte();
        }
#if 0
        // wait for CTS to become low in manual flow control mode
        if (btstack_flow_control_mode == BTSTACK_FLOW_CONTROL_MANUAL && wiced_bt_uart_pins[WICED_BT_PIN_UART_CTS]){
            while (platform_gpio_input_get(wiced_bt_uart_pins[WICED_BT_PIN_UART_CTS]) == WICED_TRUE){
                wiced_rtos_delay_milliseconds(10);
            }        
        }
#endif
        // blocking send
        platform_uart_transmit_bytes(wiced_bt_uart_driver, btstack_uart_slip_outgoing_buffer, pos);
    }

    // let transport know
    btstack_run_loop_wiced_execute_code_on_main_thread(&btstack_uart_wiced_main_notify_frame_sent, NULL);
    return WICED_SUCCESS;
}

static void btstack_uart_wiced_send_frame(const uint8_t * frame, uint16_t frame_size){
    log_debug("send frame, size %u", frame_size);
    // Prepare encoding of frame
    btstack_slip_encoder_start(frame, frame_size);
    // send on tx worker
    wiced_rtos_send_asynchronous_event(&tx_worker_thread, &btstack_uart_wiced_tx_worker_send_frame, NULL);    
}

// SLIP ENCODING
// -----------------------------

static void btstack_uart_wiced_set_frame_received( void (*block_handler)(uint16_t frame_size)){
    frame_received = block_handler;
}

static void btstack_uart_wiced_set_frame_sent( void (*block_handler)(void)){
    frame_sent = block_handler;
}

// SLIP Implementation End
#endif

// static void btstack_uart_wiced_set_sleep(uint8_t sleep){
// }
// static void btstack_uart_wiced_set_csr_irq_handler( void (*csr_irq_handler)(void)){
// }

static const btstack_uart_t btstack_uart_wiced = {
    /* int  (*init)(hci_transport_config_uart_t * config); */              &btstack_uart_wiced_init,
    /* int  (*open)(void); */                                              &btstack_uart_wiced_open,
    /* int  (*close)(void); */                                             &btstack_uart_wiced_close,
    /* void (*set_block_received)(void (*handler)(void)); */               &btstack_uart_wiced_set_block_received,
    /* void (*set_block_sent)(void (*handler)(void)); */                   &btstack_uart_wiced_set_block_sent,
#ifdef ENABLE_H5
    /* void (*set_frame_received)(void (*handler)(uint16_t frame_size); */ &btstack_uart_wiced_set_frame_received,
    /* void (*set_fraae_sent)(void (*handler)(void)); */                   &btstack_uart_wiced_set_frame_sent,
#endif
    /* int  (*set_baudrate)(uint32_t baudrate); */                         &btstack_uart_wiced_set_baudrate,
    /* int  (*set_parity)(int parity); */                                  &btstack_uart_wiced_set_parity,
    /* int  (*set_flowcontrol)(int flowcontrol); */                        NULL,
    /* void (*receive_block)(uint8_t *buffer, uint16_t len); */            &btstack_uart_wiced_receive_block,
    /* void (*send_block)(const uint8_t *buffer, uint16_t length); */      &btstack_uart_wiced_send_block,
#ifdef ENABLE_H5
    /* void (*receive_block)(uint8_t *buffer, uint16_t len); */            &btstack_uart_wiced_receive_frame,
    /* void (*send_block)(const uint8_t *buffer, uint16_t length); */      &btstack_uart_wiced_send_frame,    
#endif
    /* int (*get_supported_sleep_modes); */                                NULL,
    /* void (*set_sleep)(btstack_uart_sleep_mode_t sleep_mode); */         NULL,
    /* void (*set_wakeup_handler)(void (*handler)(void)); */               NULL,
};

const btstack_uart_t * btstack_uart_wiced_instance(void){
    return &btstack_uart_wiced;
}
