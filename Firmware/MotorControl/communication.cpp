
/* Includes ------------------------------------------------------------------*/

// TODO: remove this option
// and once the legacy protocol is phased out, remove the seq-no hack in protocol.py
// todo: make clean switches for protocol
#define ENABLE_LEGACY_PROTOCOL

#include "communication.h"
//#include "low_level.h"
#include "odrive_main.hpp"
#include "protocol.hpp"
#include "freertos_vars.h"
#include "utils.h"

#ifdef ENABLE_LEGACY_PROTOCOL
#include "legacy_commands.h"
#endif

#include <cmsis_os.h>
#include <memory>
#include <usbd_cdc_if.h>
#include <usb_device.h>
#include <usart.h>
#include <gpio.h>

#define UART_TX_BUFFER_SIZE 64

/* Private defines -----------------------------------------------------------*/
/* Private macros ------------------------------------------------------------*/
/* Private typedef -----------------------------------------------------------*/
/* Global constant data ------------------------------------------------------*/
/* Global variables ----------------------------------------------------------*/

extern PCD_HandleTypeDef hpcd_USB_OTG_FS;
extern USBD_HandleTypeDef hUsbDeviceFS;
uint64_t serial_number;

/* Private constant data -----------------------------------------------------*/
/* Private variables ---------------------------------------------------------*/

static uint8_t* usb_buf;
static uint32_t usb_len;

// FIXME: the stdlib doesn't know about CMSIS threads, so this is just a global variable
static thread_local uint32_t deadline_ms = 0;


#if defined(USB_PROTOCOL_NATIVE)

class USBSender : public PacketSink {
public:
    int process_packet(const uint8_t* buffer, size_t length) {
        // cannot send partial packets
        if (length > USB_TX_DATA_SIZE)
            return -1;
        // wait for USB interface to become ready
        if (osSemaphoreWait(sem_usb_tx, deadline_to_timeout(deadline_ms)) != osOK)
            return -1;
        // transmit packet
        uint8_t status = CDC_Transmit_FS(
                const_cast<uint8_t*>(buffer) /* casting this const away is safe because...
                well... it's not actually. Stupid STM. */, length);
        return (status == USBD_OK) ? 0 : -1;
    }
} usb_sender;

BidirectionalPacketBasedChannel usb_channel(usb_sender);

#elif defined(USB_PROTOCOL_NATIVE_STREAM_BASED)

class USBSender : public StreamSink {
public:
    int process_bytes(const uint8_t* buffer, size_t length) {
        // Loop to ensure all bytes get sent
        while (length) {
            size_t chunk = length < USB_TX_DATA_SIZE ? length : USB_TX_DATA_SIZE;
            // wait for USB interface to become ready
            if (osSemaphoreWait(sem_usb_tx, deadline_to_timeout(deadline_ms)) != osOK)
                return -1;
            // transmit chunk
            if (CDC_Transmit_FS(
                    const_cast<uint8_t*>(buffer) /* casting this const away is safe because...
                    well... it's not actually. Stupid STM. */, chunk) != USBD_OK)
                return -1;
            buffer += chunk;
            length -= chunk;
        }
        return 0;
    }

    size_t get_free_space() { return SIZE_MAX; }
} usb_sender;

PacketToStreamConverter usb_packet_sender(usb_sender);
BidirectionalPacketBasedChannel usb_channel(endpoints, NUM_ENDPOINTS, usb_packet_sender);
StreamToPacketConverter usb_stream_sink(usb_channel);

#endif

#if defined(UART_PROTOCOL_NATIVE)
class UART4Sender : public StreamSink {
public:
    int process_bytes(const uint8_t* buffer, size_t length) {
        // Loop to ensure all bytes get sent
        while (length) {
            size_t chunk = length < UART_TX_BUFFER_SIZE ? length : UART_TX_BUFFER_SIZE;
            // wait for USB interface to become ready
            // TODO: implement ring buffer to get a more continuous stream of data
            if (osSemaphoreWait(sem_uart_dma, deadline_to_timeout(deadline_ms)) != osOK)
                return -1;
            // transmit chunk
            memcpy(tx_buf_, buffer, chunk);
            if (HAL_UART_Transmit_DMA(&huart4, tx_buf_, chunk) != HAL_OK)
                return -1;
            buffer += chunk;
            length -= chunk;
        }
        return 0;
    }

    size_t get_free_space() { return SIZE_MAX; }
private:
    uint8_t tx_buf_[UART_TX_BUFFER_SIZE];
} uart4_sender;

PacketToStreamConverter uart4_packet_sender(uart4_sender);
BidirectionalPacketBasedChannel uart4_channel(endpoints, NUM_ENDPOINTS, uart4_packet_sender);
StreamToPacketConverter UART4_stream_sink(uart4_channel);
#endif


/* Private function prototypes -----------------------------------------------*/
/* Function implementations --------------------------------------------------*/

void enter_dfu_mode() {
    *((unsigned long *)0x2001C000) = 0xDEADBEEF;
    NVIC_SystemReset();
}

void init_communication(void) {
    printf("hi!\r\n");

    // Start command handling thread
    osThreadDef(task_cmd_parse, communication_task, osPriorityNormal, 0, 5000 /* in 32-bit words */); // TODO: fix stack issues
    thread_cmd_parse = osThreadCreate(osThread(task_cmd_parse), NULL);

    // Start USB interrupt handler thread
    osThreadDef(task_usb_pump, usb_update_thread, osPriorityNormal, 0, 512);
    thread_usb_pump = osThreadCreate(osThread(task_usb_pump), NULL);
}


uint32_t comm_stack_info = 0; // for debugging only

// Helper class because the protocol library doesn't yet
// support non-member functions
// TODO: make this go away
class StaticFunctions {
public:
    void save_configuration_helper() { save_configuration(); }
    void erase_configuration_helper() { erase_configuration(); }
    void NVIC_SystemReset_helper() { NVIC_SystemReset(); }
    void enter_dfu_mode_helper() { enter_dfu_mode(); }
} static_functions;

// When adding new functions/variables to the protocol, be careful not to
// blow the communication stack. You can check comm_stack_info to see
// how much headroom you have.
static inline auto make_obj_tree() {
    return make_protocol_member_list(
        make_protocol_ro_property("vbus_voltage", &vbus_voltage),
        make_protocol_ro_property("comm_stack_info", &comm_stack_info),
        make_protocol_ro_property("serial_number", &serial_number),
        make_protocol_object("config",
            make_protocol_property("brake_resistance", &board_config.brake_resistance),
            // TODO: changing this currently requires a reboot - fix this
            make_protocol_property("enable_uart", &board_config.enable_uart)
        ),
        make_protocol_object("axis0", axes[0]->make_protocol_definitions()),
        make_protocol_object("axis1", axes[1]->make_protocol_definitions()),
        make_protocol_property("adc0", &adc_measurements_[0]),
        make_protocol_property("adc1", &adc_measurements_[1]),
        make_protocol_property("adc2", &adc_measurements_[2]),
        make_protocol_property("adc3", &adc_measurements_[3]),
        make_protocol_property("adc4", &adc_measurements_[4]),
        make_protocol_property("adc5", &adc_measurements_[5]),
        make_protocol_property("adc6", &adc_measurements_[6]),
        make_protocol_function("save_configuration", static_functions, &StaticFunctions::save_configuration_helper),
        make_protocol_function("erase_configuration", static_functions, &StaticFunctions::erase_configuration_helper),
        make_protocol_function("reboot", static_functions, &StaticFunctions::NVIC_SystemReset_helper),
        make_protocol_function("enter_dfu_mode", static_functions, &StaticFunctions::enter_dfu_mode_helper)
    );
}

using tree_type = decltype(make_obj_tree());
uint8_t tree_buffer[sizeof(tree_type)];

// the protocol has one additional built-in endpoint
constexpr size_t MAX_ENDPOINTS = decltype(make_obj_tree())::endpoint_count + 1;
Endpoint* endpoints_[MAX_ENDPOINTS] = { 0 };
const size_t max_endpoints_ = MAX_ENDPOINTS;
size_t n_endpoints_ = 0;

// Thread to handle deffered processing of USB interrupt, and
// read commands out of the UART DMA circular buffer
void communication_task(void * ctx) {
    (void) ctx; // unused parameter

    // TODO: this is supposed to use the move constructor, but currently
    // the compiler uses the copy-constructor instead. Thus the make_obj_tree
    // ends up with a stupid stack size of around 8000 bytes. Fix this.
    auto tree_ptr = new (tree_buffer) tree_type(make_obj_tree());
    auto endpoint_provider = EndpointProvider_from_MemberList<tree_type>(*tree_ptr);
    set_application_endpoints(&endpoint_provider);
    comm_stack_info = uxTaskGetStackHighWaterMark(nullptr);
    
#if !defined(UART_PROTOCOL_NONE)
    //DMA open loop continous circular buffer
    //1ms delay periodic, chase DMA ptr around

    #define UART_RX_BUFFER_SIZE 64
    static uint8_t dma_circ_buffer[UART_RX_BUFFER_SIZE];

    // DMA is set up to recieve in a circular buffer forever.
    // We dont use interrupts to fetch the data, instead we periodically read
    // data out of the circular buffer into a parse buffer, controlled by a state machine
    HAL_UART_Receive_DMA(&huart4, dma_circ_buffer, sizeof(dma_circ_buffer));
    uint32_t last_rcv_idx = UART_RX_BUFFER_SIZE - huart4.hdmarx->Instance->NDTR;
#endif

    // Re-run state-machine forever
    for (;;) {
#if !defined(UART_PROTOCOL_NONE)
        // Check for UART errors and restart recieve DMA transfer if required
        if (huart4.ErrorCode != HAL_UART_ERROR_NONE) {
            HAL_UART_AbortReceive(&huart4);
            HAL_UART_Receive_DMA(&huart4, dma_circ_buffer, sizeof(dma_circ_buffer));
        }
        // Fetch the circular buffer "write pointer", where it would write next
        uint32_t new_rcv_idx = UART_RX_BUFFER_SIZE - huart4.hdmarx->Instance->NDTR;

        deadline_ms = timeout_to_deadline(PROTOCOL_SERVER_TIMEOUT_MS);
#if defined(UART_PROTOCOL_NATIVE)
        // Process bytes in one or two chunks (two in case there was a wrap)
        if (new_rcv_idx < last_rcv_idx) {
            UART4_stream_sink.process_bytes(dma_circ_buffer + last_rcv_idx,
                    UART_RX_BUFFER_SIZE - last_rcv_idx);
            last_rcv_idx = 0;
        }
        if (new_rcv_idx > last_rcv_idx) {
            UART4_stream_sink.process_bytes(dma_circ_buffer + last_rcv_idx,
                    new_rcv_idx - last_rcv_idx);
            last_rcv_idx = new_rcv_idx;
        }
#elif defined(UART_PROTOCOL_LEGACY)
        // Process bytes in one or two chunks (two in case there was a wrap)
        if (new_rcv_idx < last_rcv_idx) {
            legacy_parse_stream(dma_circ_buffer + last_rcv_idx,
                    UART_RX_BUFFER_SIZE - last_rcv_idx);
            last_rcv_idx = 0;
        }
        if (new_rcv_idx > last_rcv_idx) {
            legacy_parse_stream(dma_circ_buffer + last_rcv_idx,
                    new_rcv_idx - last_rcv_idx);
            last_rcv_idx = new_rcv_idx;
        }
#endif
#endif

#if !defined(USB_PROTOCOL_NONE)
        // When we reach here, we are out of immediate characters to fetch out of UART buffer
        // Now we check if there is any USB processing to do: we wait for up to 1 ms,
        // before going back to checking UART again.
        const uint32_t usb_check_timeout = 1; // ms
        osStatus sem_stat = osSemaphoreWait(sem_usb_rx, usb_check_timeout);
        if (sem_stat == osOK) {
            deadline_ms = timeout_to_deadline(PROTOCOL_SERVER_TIMEOUT_MS);
#if defined(USB_PROTOCOL_NATIVE)
            usb_channel.process_packet(usb_buf, usb_len);
#elif defined(USB_PROTOCOL_NATIVE_STREAM_BASED)
            usb_stream_sink.process_bytes(usb_buf, usb_len);
#elif defined(USB_PROTOCOL_LEGACY)
            legacy_parse_cmd(usb_buf, usb_len, USB_RX_DATA_SIZE, SERIAL_PRINTF_IS_USB);
#endif
            USBD_CDC_ReceivePacket(&hUsbDeviceFS);  // Allow next packet
        }
#endif

#if defined(USB_PROTOCOL_NONE) && defined(UART_PROTOCOL_NONE)
        osDelay(1); // don't starve other threads
#endif
    }

    // If we get here, then this task is done
    vTaskDelete(osThreadGetId());
}

// Called from CDC_Receive_FS callback function, this allows motor_parse_cmd to access the
// incoming USB data
void set_cmd_buffer(uint8_t *buf, uint32_t len) {
    usb_buf = buf;
    usb_len = len;
}

void usb_update_thread(void * ctx) {
    (void) ctx; // unused parameter

    for (;;) {
        // Wait for signalling from USB interrupt (OTG_FS_IRQHandler)
        osStatus semaphore_status = osSemaphoreWait(sem_usb_irq, osWaitForever);
        if (semaphore_status == osOK) {
            // We have a new incoming USB transmission: handle it
            HAL_PCD_IRQHandler(&hpcd_USB_OTG_FS);
            // Let the irq (OTG_FS_IRQHandler) fire again.
            HAL_NVIC_EnableIRQ(OTG_FS_IRQn);
        }
    }

    vTaskDelete(osThreadGetId());
}
