#include <string.h>
#include <stdio.h>
#include <time.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h" 
#include "freertos/event_groups.h"
#include "esp_system.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_http_client.h"
#include "esp_crt_bundle.h"  // <-- ADDED: Crucial for SSL/TLS handshakes
#include "nvs_flash.h"
#include "lwip/err.h"
#include "lwip/sys.h"
#include "cJSON.h"
#include "driver/uart.h"
#include "driver/gpio.h"
#include "esp_rom_crc.h" // standard CRC32 algorithm

// ============================================================================
// CONFIGURATIONS & PROTOCOL CONSTANTS
// ============================================================================

// Wi-Fi Access Point Credentials
#define MY_ESP_WIFI_SSID              "AP"
#define MY_ESP_WIFI_PASS              "Mesnilsdugolf7474"
#define MY_ESP_MAXIMUM_RETRY          4

// Security Threshold: Forces the ESP32 to only connect if the network uses WPA2 or better.
#define ESP_WIFI_SCAN_AUTH_MODE_THRESHOLD WIFI_AUTH_WPA2_PSK

// Hardware Mapping: UART Bus 1 dedicated to communicating with the STM32
#define STM32_UART_PORT               UART_NUM_1
#define STM32_UART_BAUD               1500000 // 1.5 Mbps

// Hardware GPIO Pins for UART1
#define STM32_TX_PIN                  (17) 
#define STM32_RX_PIN                  (18) 

// Size allocation for the UART hardware ring buffer
#define BUF_SIZE                      (1024)

// Protocol Command Definition matching your bare-metal bootloader spec.
#define CMD_UPDATE_INFO_REPLY         0x20  // ESP32: "This is the FW version I fetched from server"
#define CMD_CHECK_UPDATE_REQ          0x25  // STM32 -> ESP32: "Check for updates!"
#define CMD_REQ_UPDATE_SERVER         0x28  // STM32 -> ESP32: "You can update the new FW"

#define PACKET_START_BYTE     0x02
#define ACK_BYTE              0x06
#define NAK_GENERIC           0x15
#define NAK_CRC_ERROR         0x16
#define NAK_VERSION_MISMATCH  0x17
#define NAK_MAGIC_MISSING     0x18
#define ERR_FLASH_ERASE       0xE1
#define ERR_FLASH_WRITE       0xE2
#define WAIT_BYTE             0x19 // used during erase time of STM32 Flash

#define PAYLOAD_MAX_SIZE  512

// ADDED: Explicit Context Enumerations to completely prevent stream-crossing memory bugs
// Helps the HTTP callback distinguish what format of network chunk it is processing.
typedef enum {
    HTTP_CONTEXT_JSON_VERSION,
    HTTP_CONTEXT_FIRMWARE_BINARY
} HttpContextType_t;

// Structured container for the version payload buffer to avoid dirty type-casting
typedef struct {
    HttpContextType_t type;
    char buffer[512];
} HttpVersionContext_t;

// FIXED: Removed start_byte from this struct to precisely match Python's 12-byte format: struct.pack(">IHHI")
// __attribute__((packed)) forces the compiler to eliminate structural padding bytes, ensuring the struct maps
// byte-for-byte identically over the raw UART data frame.
typedef struct __attribute__((packed)) {
    uint32_t total_size;     // Big-Endian: Full padded binary image size footprint
    uint16_t packet_id;      // Big-Endian: Current step counter sequence index
    uint16_t payload_len;    // Big-Endian: Length of raw application bytes in current frame
    uint16_t version_major;  // New field (2 bytes): Big-Endian Target major validation tag
    uint16_t version_minor;  // New field (2 bytes): Big-Endian Target minor validation tag
    uint32_t total_crc;      // Big-Endian: Global master validation file-wide check token
} FirmwareHeader_t;

// Stores the final full-file checksum and version obtained upfront from firmware.json
static uint32_t server_firmware_crc = 0;
static uint16_t server_version_major = 0; 
static uint16_t server_version_minor = 0; 

// To prevent using heap memory or overloading the stack, we will define a persistent static streaming structure. 
// This space safely accumulates the bytes flowing in from the network callback.
typedef struct {
    uint8_t  buffer[PAYLOAD_MAX_SIZE];
    uint16_t current_len;
    uint16_t packet_id;
    uint32_t total_file_size;
    uint32_t accumulated_bytes_processed;
    uint32_t running_total_crc;
} FirmwareStreamContext_t;

static FirmwareStreamContext_t fw_ctx;

static volatile bool ota_abort_requested = false; // Global state variable to allow safe aborts

// Logging Tags for IDF Monitor tracking
static const char *WIFI_TAG = "WIFI_MOD";
static const char *APP_TAG  = "OTA_MGMT";

// Counter to track consecutive failed Wi-Fi reconnection attempts
static int s_retry_num = 0;

// ============================================================================
// SYNCHRONIZATION MECHANISMS (FreeRTOS Event Groups)
// ============================================================================
static EventGroupHandle_t s_wifi_event_group;
#define WIFI_CONNECTED_BIT            BIT0
#define WIFI_FAIL_BIT                 BIT1

// ============================================================================
// MEMORY STRUCTURE DEFINITIONS
// ============================================================================
typedef struct __attribute__((packed)) {
    uint8_t command;        // 1 byte  - Identifier (0x20)
    uint8_t version_major;  // 1 byte  - Major release version integer
    uint8_t version_minor;  // 1 byte  - Minor release version integer
} WebFirmwareInfo_t;

// ============================================================================
// HARDWARE INITIALIZATION MODULES
// ============================================================================
static void init_stm32_uart(void) {
    uart_config_t uart_config = {
        .baud_rate = STM32_UART_BAUD,
        .data_bits = UART_DATA_8_BITS,
        .parity    = UART_PARITY_DISABLE,
        .stop_bits = UART_STOP_BITS_1,
        .flow_ctrl = UART_HW_FLOWCTRL_DISABLE,
        .source_clk = UART_SCLK_DEFAULT,
    }; 
    
    // Configure structural parameters, assign custom IO multiplexer pins, and allocate peripheral space
    ESP_ERROR_CHECK(uart_param_config(STM32_UART_PORT, &uart_config));
    ESP_ERROR_CHECK(uart_set_pin(STM32_UART_PORT, STM32_TX_PIN, STM32_RX_PIN, UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE));
    ESP_ERROR_CHECK(uart_driver_install(STM32_UART_PORT, BUF_SIZE * 2, 0, 0, NULL, 0));
    
    ESP_LOGI(APP_TAG, "UART initialized on Pins TX:%d, RX:%d", STM32_TX_PIN, STM32_RX_PIN);
}

// ============================================================================
// NETWORK CORE LOGIC (HTTP Execution & Parsing)
// ============================================================================

static uint32_t calc_esp32_crc32(const uint8_t *data, size_t len) {
    // FIXED: Passing 0 to esp_rom_crc32_le causes it to start with 0xFFFFFFFF internally,
    // which flawlessly matches your standard STM32 software CRC32 algorithm.
    return esp_rom_crc32_le(0, data, len);
}

static bool send_packet_to_stm32(const uint8_t *payload, uint16_t len) {
    FirmwareHeader_t header;
    uint8_t start_byte = PACKET_START_BYTE;   

    // Convert to Big-Endian network order for matching script definitions
    // __builtin_bswap functions swap bytes on runtime to ensure the data layouts match little-endian vs big-endian boundaries
    header.total_size  = __builtin_bswap32(fw_ctx.total_file_size);
    header.packet_id   = __builtin_bswap16(fw_ctx.packet_id);
    header.payload_len = __builtin_bswap16(len);
    header.version_major = __builtin_bswap16(server_version_major);
    header.version_minor = __builtin_bswap16(server_version_minor);
    header.total_crc   = __builtin_bswap32(server_firmware_crc);

    //ESP_LOGI(APP_TAG, "total_crc = %u", header.total_crc);
    
    // Generate individual slice frame verification token
    uint32_t payload_crc = calc_esp32_crc32(payload, len);
    uint32_t payload_crc_be = __builtin_bswap32(payload_crc);
    
    int max_retries = 3;
    int retry_count = 0;
    
    // Synchronous Frame transmission sequence: loops if corruption or timeout occurs
    while (retry_count < max_retries) {
        if (ota_abort_requested) return false;

        ESP_LOGI(APP_TAG, "Sending Packet %d (Size: %d) ...", fw_ctx.packet_id, len);        
        // Clean UART queue before transmit to erase residual noise bytes
        uart_flush_input(STM32_UART_PORT);  

        // SEND the start_byte sequentially first, followed by the clean 12-byte header
        uart_write_bytes(STM32_UART_PORT, (const char*)&start_byte, 1);
        uart_write_bytes(STM32_UART_PORT, (const char*)&header, sizeof(FirmwareHeader_t));
        uart_write_bytes(STM32_UART_PORT, (const char*)payload, len);
        uart_write_bytes(STM32_UART_PORT, (const char*)&payload_crc_be, sizeof(payload_crc_be));
        
        // Then wait up to 100ms for STM32 response byte (excepted WAIT_BYTE)
        uint8_t response = 0;
        int rx_len = uart_read_bytes(STM32_UART_PORT, &response, 1, pdMS_TO_TICKS(100));
        
        if (rx_len > 0) {
            if (response == WAIT_BYTE) {
                ESP_LOGI(APP_TAG, "STM32 is busy erasing flash (>350ms). Extending timeout window...");                
                // Block and wait with a generous window specifically for the hardware operation
                rx_len = uart_read_bytes(STM32_UART_PORT, &response, 1, pdMS_TO_TICKS(2500));
                if (rx_len <= 0) {
                    ESP_LOGE(APP_TAG, "Timeout waiting for STM32 to finish flash operation.");
                    retry_count++;
                    continue;
                }
            }
              switch(response) {
                case ACK_BYTE:
                    ESP_LOGI(APP_TAG, "ACK received from STM32.");
                    fw_ctx.packet_id++;
                    fw_ctx.accumulated_bytes_processed += len;
                    return true; // Frame complete, move to next network chunk
                case NAK_CRC_ERROR:
                    retry_count++;
                    ESP_LOGW(APP_TAG, "STM32 reported NAK: CRC Line Corruption! Retrying packet %d (%d/%d)...", 
                             fw_ctx.packet_id, retry_count, max_retries);
                    vTaskDelay(pdMS_TO_TICKS(50));
                    break;
                case NAK_VERSION_MISMATCH:
                    ota_abort_requested = true;
                    ESP_LOGE(APP_TAG, "CRITICAL ABORT: App version compiled in .bin doesn't match manifest JSON!");
                    return false; // Stop downloading from server immediately
                case NAK_MAGIC_MISSING:
                    ota_abort_requested = true;
                    ESP_LOGE(APP_TAG, "CRITICAL ABORT: Valid 'VERS' metadata anchor missing from target address!");
                    return false; // Stop pipeline
                case ERR_FLASH_ERASE:
                    ota_abort_requested = true;
                    ESP_LOGE(APP_TAG, "HARDWARE FAULT: STM32 failed to physically erase internal flash sector.");
                    return false; // Hardware dead end
                case ERR_FLASH_WRITE:
                    ota_abort_requested = true;
                    ESP_LOGE(APP_TAG, "HARDWARE FAULT: Flash verification mismatch! Corrupted silicon or data write.");
                    return false; // Hardware dead end
                case NAK_GENERIC:
                default:
                    retry_count++;
                    ESP_LOGW(APP_TAG, "Received unhandled or generic NAK (0x%02X). Retrying...", response);
                    vTaskDelay(pdMS_TO_TICKS(100));
                    break;
            }
        } else {
            retry_count++;
            ESP_LOGW(APP_TAG, "Timeout waiting for STM32 response. Retrying...");
        }
    } 
    return false;
}

static esp_err_t _http_event_handler(esp_http_client_event_t *evt) {    
    // After a request, the server doesn't always send the whole file in a single chunk. The data flows
    // over the network in small packets and every time one arrives, the network stack triggers HTTP_EVENT_ON_DATA
    switch(evt->event_id) {
        case HTTP_EVENT_ON_DATA: {
            if (!evt->user_data) return ESP_OK;
            
            // Read the safe enum context pointer instead of memory-punning a string buffer
            HttpContextType_t context_type = *(HttpContextType_t*)evt->user_data;
            
            if (context_type == HTTP_CONTEXT_FIRMWARE_BINARY) {
                if (ota_abort_requested) return ESP_FAIL; 
        
                // If this is the very first packet fragment, resolve total file size if provided by client layers
                if (fw_ctx.total_file_size == 0) {
                    // The code reads the HTTP header (Content-Length) to see how large the overall file is.
                    // Not always reliable: if the web server or a proxy (like Cloudflare) decides to compress the file 
                    // on the fly or stream it dynamically, it will omit the Content-Length header entirely
                    int content_len = esp_http_client_get_content_length(evt->client);
                    // Handle alignment constraints: pad file length calculation up to 4-byte boundaries if needed
                    if (content_len > 0) {
                        int remainder = content_len % 4;
                        fw_ctx.total_file_size = (remainder == 0) ? content_len : (content_len + (4 - remainder));
                    }
                }
                // Grabs a pointer to the newly arrived network packet (evt->data) and checks how many bytes it contains
                // The data pointed to by evt->data is stored inside a buffer managed by the ESP-IDF TCP/IP stack (lwIP)
                uint8_t *incoming_data = (uint8_t *)evt->data;
                int incoming_len = evt->data_len; // Network packets usually arrive in random sizes
                               
                // Stream parsing pump: extracts incoming TCP payload bytes into the fixed static UART chunk cache
                for (int i = 0; i < incoming_len; i++) {
                    fw_ctx.buffer[fw_ctx.current_len] = incoming_data[i];
                    fw_ctx.current_len++;
                    
                    // Transmit the payload packet to the STM32 only if the buffer is complete (512B)
                    if (fw_ctx.current_len == PAYLOAD_MAX_SIZE) {
                        if (!send_packet_to_stm32(fw_ctx.buffer, PAYLOAD_MAX_SIZE)) {
                            // If the UART communication fails (STM32 drops out, returns NAK or error)
                            ESP_LOGE(APP_TAG, "Aborting download pipeline due to STM32 handshake fault.");                                                 
                            return ESP_FAIL; // Tells HTTP client to halt execution cleanly
                        }
                        fw_ctx.current_len = 0; // Clear index window for next block
                        // If the ESP32 is busy processing UART bytes and hasn't finished its event handler, 
                        // the ESP32's Wi-Fi hardware will naturally delay sending a TCP ACK back to the web server
                    }
                    // Once the _http_event_handler finishes, control drops back into fetch_and_forward_firmware. 
                    // Indeed, we must wait until esp_http_client_perform finishes and confirming a 200 status, 
                    // only then we can push the final "tail" packet (< 512B) over UART.
                }
            } else if (context_type == HTTP_CONTEXT_JSON_VERSION) {
                // Takes the generic void* pointer stored in evt->user_data and casts it back into a pointer to the
                // HttpVersionContext_t structure. Now, the event handler has direct access to the 512B destination buffer
                HttpVersionContext_t *version_ctx = (HttpVersionContext_t *)evt->user_data;                
                int current_len = strlen(version_ctx->buffer); // How many characters have already been saved into the buffer ?
                // Preventing a Buffer Overflow (if the server suddenly responds with an unexpected 1000-byte file)
                if (current_len + evt->data_len < 512) {
                    // Copies the new raw data (evt->data) into the buffer, starting where the previous packet left off
                    memcpy(version_ctx->buffer + current_len, evt->data, evt->data_len);
                    version_ctx->buffer[current_len + evt->data_len] = '\0'; // Adding Null Terminator
                }
            }
            break;
        }
        default:
            break;
    }
    return ESP_OK;
}

static void fetch_and_forward_version(void) {
    // Instantiated explicit structured context type to prevent data text from acting as a 'true' boolean flag
    HttpVersionContext_t version_context;
    memset(&version_context, 0, sizeof(HttpVersionContext_t));
    version_context.type = HTTP_CONTEXT_JSON_VERSION; // enum HTTP_CONTEXT_JSON_VERSION = 0
    
    esp_http_client_config_t config = {
        .url = "https://exercices.nprata.de/STM32_FW/firmware.json",    
        // Passing a custom struct into .user_data makes it available inside the _http_event_handler   
        .user_data = &version_context,
         // ESP-IDF background tasks will automatically jump to _http_event_handler() whenever a http event happens
        .event_handler = _http_event_handler, 
        // If the server responds "this file moved to a different URL", the ESP32 will automatically follow the new link
        .disable_auto_redirect = false,
        .transport_type = HTTP_TRANSPORT_OVER_SSL, // use TLS/SSL encryption 
        // Allows the ESP32 to check the server against a pre-compiled bundle of trusted global certificate authorities  
        .crt_bundle_attach = esp_crt_bundle_attach, // Important ! Enabled in the project's sdkconfig.
    };
    // "client" variable holds the reference to the HTTP session. It will be passed into every future called HTTP function
    esp_http_client_handle_t client = esp_http_client_init(&config);
    
    /* The ESP32 successfully verified the server's certificate and completed a standard, clean TLS handshake, 
     the firewall categorized it as a legitimate client rather than a malicious script */
    esp_http_client_set_header(client, "Accept", "application/json");
    
    // Execute synchronous network transactions
    ESP_LOGI(APP_TAG, "Executing Secure HTTPS GET request with Cert Bundle...");
    
    // Executes the entire HTTP request (connection, request sending, and response receiving) in a blocking manner
    esp_err_t err = esp_http_client_perform(client);
    
    if (err == ESP_OK) {
        int status_code = esp_http_client_get_status_code(client);
        if (status_code == 200) {
            ESP_LOGI(APP_TAG, "Download Successful. Raw JSON: %s", version_context.buffer);
            
            cJSON *json = cJSON_Parse(version_context.buffer);
            if (json != NULL) {
                cJSON *major_obj = cJSON_GetObjectItemCaseSensitive(json, "version_major");
                cJSON *minor_obj = cJSON_GetObjectItemCaseSensitive(json, "version_minor");
                cJSON *crc_obj   = cJSON_GetObjectItemCaseSensitive(json, "total_crc");
                
                if (cJSON_IsNumber(major_obj) && cJSON_IsNumber(minor_obj)) {                  
                    // Save the upfront full-file CRC globally for the next step
                    if (cJSON_IsNumber(crc_obj)) {
                        server_firmware_crc = (uint32_t)crc_obj->valuedouble; 
                    } else if (cJSON_IsString(crc_obj) && crc_obj->valuestring != NULL) {
                        server_firmware_crc = (uint32_t)strtoul(crc_obj->valuestring, NULL, 10);
                    }
                    // Save versions globally so the firmware streaming task can access them
                    server_version_major = (uint16_t)major_obj->valueint;
                    server_version_minor = (uint16_t)minor_obj->valueint; 
                    ESP_LOGI(APP_TAG, "Upfront File CRC Captured: %u", server_firmware_crc);
                                                  
                    WebFirmwareInfo_t tx_packet;
                    tx_packet.command = CMD_UPDATE_INFO_REPLY;
                    tx_packet.version_major = (uint8_t)major_obj->valueint;
                    tx_packet.version_minor = (uint8_t)minor_obj->valueint;
                    
                    // Transmit response metadata array frame back upstream over UART lines
                    uart_write_bytes(STM32_UART_PORT, (const char*)&tx_packet, sizeof(WebFirmwareInfo_t));
                    ESP_LOGI(APP_TAG, "Packed struct frame successfully forwarded over UART to the STM32 brain!");
                }
                cJSON_Delete(json); // Release form the heap the allocated parsing descriptors memory blocks 
            }
        } else {
            ESP_LOGE(APP_TAG, "Server returned Bad HTTP Code: %d", status_code);
        }
    } else {
        ESP_LOGE(APP_TAG, "Network layer failure: %s", esp_err_to_name(err));
    }    
    esp_http_client_cleanup(client); // terminating the p-to-p connection with the server and frees memory
}

static void fetch_and_forward_firmware(void) {
    HttpContextType_t context_type = HTTP_CONTEXT_FIRMWARE_BINARY;
    
    // Reset and initialize context tracker fields
    ota_abort_requested = false;
    memset(&fw_ctx, 0, sizeof(FirmwareStreamContext_t));
    fw_ctx.packet_id = 1;

    esp_http_client_config_t config = {
        .url = "https://exercices.nprata.de/STM32_FW/application.bin",
        .event_handler = _http_event_handler, 
        .user_data = &context_type,
        .disable_auto_redirect = false,
        .transport_type = HTTP_TRANSPORT_OVER_SSL,      
        .crt_bundle_attach = esp_crt_bundle_attach,
    };
    
    esp_http_client_handle_t client = esp_http_client_init(&config);
    esp_http_client_set_header(client, "Accept", "application/octet-stream");
    
    ESP_LOGI(APP_TAG, "Initiating Secure Download and Framing Stream...");
    esp_err_t err = esp_http_client_perform(client);
    
    if (err == ESP_OK) {
        int status_code = esp_http_client_get_status_code(client);
        if (status_code == 200) {
            // Check if there are remaining trailing bytes in the buffer 
            // after _http_event_handler has already processed the stream of all full-sized blocks
            if (fw_ctx.current_len > 0 && !ota_abort_requested) {
                // Apply 4-byte padding alignment constraints to the final sub-block if needed
                int remainder = fw_ctx.current_len % 4;
                if (remainder != 0) {
                    int padding_needed = 4 - remainder;
                    for (int p = 0; p < padding_needed; p++) {
                        if (fw_ctx.current_len < PAYLOAD_MAX_SIZE) {
                            fw_ctx.buffer[fw_ctx.current_len] = 0xFF; // Padding trailing space
                            fw_ctx.current_len++;
                        }
                    }
                }                
                // Flush final padded packet downstream
                // Changed length from PAYLOAD_MAX_SIZE to fw_ctx.current_len to perfectly replicate the unpadded tail payload slice
                send_packet_to_stm32(fw_ctx.buffer, fw_ctx.current_len);
            }
            if (!ota_abort_requested) {
                ESP_LOGI(APP_TAG, "Flashing sequence finished! Processed: %d bytes total.", fw_ctx.accumulated_bytes_processed); // [cite: 104]
            } else {
                ESP_LOGE(APP_TAG, "Flashing sequence stopped due to bootloader abort.");
            }
        } else {
            ESP_LOGE(APP_TAG, "Server returned bad HTTP Code: %d", status_code);
        }      
    } else {       
        // If perform failed because we deliberately returned ESP_FAIL in the handler:
        if (ota_abort_requested) {
            ESP_LOGE(APP_TAG, "Pipeline terminated: Aborted by STM32 request command.");
        } else {
            ESP_LOGE(APP_TAG, "HTTPS download pipe failed network layer: %s", esp_err_to_name(err)); // [cite: 105]
        }
    }    
    esp_http_client_cleanup(client);
} 


// ============================================================================
// ASYNCHRONOUS EVENT HANDLER (System Callback Architecture)
// ============================================================================
static void event_handler(void* arg, esp_event_base_t event_base, int32_t event_id, void* event_data) {
    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START) {
        ESP_LOGI(WIFI_TAG, "Wi-Fi hardware radio online. Launching network AP connection request...");
        esp_wifi_connect();      
    } else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED) {
        if (s_retry_num < MY_ESP_MAXIMUM_RETRY) {
            esp_wifi_connect(); 
            s_retry_num++;
            ESP_LOGI(WIFI_TAG, "AP connection dropped. Retrying link layer attempt (%d/%d)...", s_retry_num, MY_ESP_MAXIMUM_RETRY);
        } else {
            // Signal failure condition to blocking initialization tasks
            xEventGroupSetBits(s_wifi_event_group, WIFI_FAIL_BIT);
            ESP_LOGE(WIFI_TAG, "Critical limit reached. Failed to securely link with the Router.");
        }
    } else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        // IP acquisition success handler via DHCP service
        ip_event_got_ip_t* event = (ip_event_got_ip_t*) event_data; // cast generic void* event_data pointer to ip_event_got_ip_t*
        ESP_LOGI(WIFI_TAG, "DHCP Validation Success. Assigned IP Address: " IPSTR, IP2STR(&event->ip_info.ip));
        s_retry_num = 0; 
        xEventGroupSetBits(s_wifi_event_group, WIFI_CONNECTED_BIT); // Set sync event bit flag
    }
}

// ============================================================================
// SYSTEM LAYER INITIALIZATION (Wi-Fi Core Setup)
// ============================================================================
void wifi_init_sta(void) {
    // FreeRTOS event group allocation: returns a handle used to synchronize 
    // network connection and IP acquisition states across asynchronous callback threads
    s_wifi_event_group = xEventGroupCreate();   

    // Setup network configurations interfaces layers
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default()); 
    esp_netif_create_default_wifi_sta();  

    // Macro initializer: populates the Wi-Fi configuration structure with 
    // default hardware settings (OS task priorities, buffer allocations, Rx/Tx structures)
    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));    

    //  Unique instance identifiers (opaque structures) passed by pointer to the event loop registration API to keep
    // track of these specific event hooks. If this application ever needed to toggle Wi-Fi dynamically to save battery, 
    // passing these instance variables back to esp_event_handler_instance_unregister() prevents memory leaks
    esp_event_handler_instance_t instance_any_id; // handler
    esp_event_handler_instance_t instance_got_ip;

    // Bind our custom 'event_handler' to intercept any base Wi-Fi status changes (like radio starts or drops)
    ESP_ERROR_CHECK(esp_event_handler_instance_register(WIFI_EVENT, ESP_EVENT_ANY_ID, &event_handler, NULL, &instance_any_id));
    // Trigger when the DHCP client finishes assigning an IP - The event_handler function is called when the event is dispatched
    ESP_ERROR_CHECK(esp_event_handler_instance_register(IP_EVENT, IP_EVENT_STA_GOT_IP, &event_handler, NULL, &instance_got_ip));      

    wifi_config_t wifi_config = {
        .sta = {
            .ssid = MY_ESP_WIFI_SSID,
            .password = MY_ESP_WIFI_PASS,
            .threshold.authmode = ESP_WIFI_SCAN_AUTH_MODE_THRESHOLD,         
        },
    };
    
    // Set Wi-Fi Mode and configuration configurations cleanly
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wifi_config));
    ESP_ERROR_CHECK(esp_wifi_start());
    
    // Core setup initialization sequence completely loaded.
    ESP_LOGI(WIFI_TAG, "Core Wi-Fi initialization phases complete.");
}

static void stm32_uart_listener_task(void *pvParameters) {
    uint8_t data[BUF_SIZE];
    ESP_LOGI(APP_TAG, "STM32 UART listener task started. Waiting for commands...");

    while (1) {
        // This blocks efficiently until data arrives in the RX ring buffer
        int len = uart_read_bytes(STM32_UART_PORT, data, BUF_SIZE, pdMS_TO_TICKS(100));
        
        if (len > 0) {
            // Check if the incoming byte matches our update request command
            if (data[0] == CMD_CHECK_UPDATE_REQ) {
                ESP_LOGI(APP_TAG, "Received fw version request command (0x%02X) from STM32!", data[0]);                
                // Fire off the network check
                fetch_and_forward_version();
            }             
            else if (data[0] == CMD_REQ_UPDATE_SERVER) {
                // STM32 command received to download and forward new firmware back to the STM32
                ESP_LOGI(APP_TAG, "Received request command (0x%02X) to upload and forward new FW from STM32!", data[0]);
                fetch_and_forward_firmware();
            }
            else {
                ESP_LOGW(APP_TAG, "Unknown UART command received: 0x%02X", data[0]);
            }
        }        
        // Minor delay to yield to other lower priority background tasks     
        vTaskDelay(pdMS_TO_TICKS(10)); // Crucial to prevent watchdog starvation conditions on the execution core
    }
}

// ============================================================================
// MAIN SYSTEM APPLICATION ENTRY POINT
// ============================================================================
void app_main(void) {
    
    // Initialize Non-Volatile Storage (NVS) memory block subsystem
    // Used by internal Wi-Fi drivers to load/store operational configurations
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
      ESP_ERROR_CHECK(nvs_flash_erase());
      ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

    ESP_LOGI(APP_TAG, "Boot Sequence Active. Initializing target peripherals...");

    // Initialize the physical hardware communication lines to the STM32 motherboard
    init_stm32_uart();
    wifi_init_sta();

    // Launch the listener task. Priority 5 ensures it responds quickly to the STM32.
    // Stack allocation size 8192 prevents runtime overflows when processing intense JSON files.
    xTaskCreate(stm32_uart_listener_task, "stm32_uart_listener", 8192, NULL, 5, NULL);
    
    while(1) {
        // Background tick keepalive loop processing execution markers
        vTaskDelay(pdMS_TO_TICKS(2000));
    }
}



/* 
uart_read_bytes: Processing Latency
While the data isn't lost or ignored during the vTaskDelay(pdMS_TO_TICKS(10)), 
you must consider the execution time of the commands themselves.
Functions like fetch_and_forward_version() make network requests over HTTPS. 
Network handshakes and API downloads take anywhere from 200ms to over 1000ms.
Because the listener loop is synchronous, the ESP32 will be busy talking to the server and won't loop back 
to check the UART buffer until that network operation completely finishes. 
If the STM32 sends multiple commands rapid-fire while the ESP32 is stuck waiting on the internet, 
those commands will stack up inside the 2048-byte ring buffer (BUF_SIZE * 2) 
and be processed one by one after the network function returns. 

*/