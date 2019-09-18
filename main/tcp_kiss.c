/* BSD Socket API Example

   This example code is in the Public Domain (or CC0 licensed, at your option.)

   Unless required by applicable law or agreed to in writing, this
   software is distributed on an "AS IS" BASIS, WITHOUT WARRANTIES OR
   CONDITIONS OF ANY KIND, either express or implied.
*/
#include <string.h>
#include <sys/param.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_system.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_log.h"
#include "nvs_flash.h"
#include "tcpip_adapter.h"

#include "lwip/err.h"
#include "lwip/sockets.h"
#include "lwip/sys.h"
#include <lwip/netdb.h>

#include "LibAPRS.h"

#include "tcp_kiss.h"


#define PORT 8001

static const char *TAG = "tcp_kiss";

static int bind_port() {

    char addr_str[128];
    int addr_family;
    int ip_protocol;

    #ifdef CONFIG_EXAMPLE_IPV4
            struct sockaddr_in dest_addr;
            dest_addr.sin_addr.s_addr = htonl(INADDR_ANY);
            dest_addr.sin_family = AF_INET;
            dest_addr.sin_port = htons(PORT);
            addr_family = AF_INET;
            ip_protocol = IPPROTO_IP;
            inet_ntoa_r(dest_addr.sin_addr, addr_str, sizeof(addr_str) - 1);
    #else // IPV6
            struct sockaddr_in6 dest_addr;
            bzero(&dest_addr.sin6_addr.un, sizeof(dest_addr.sin6_addr.un));
            dest_addr.sin6_family = AF_INET6;
            dest_addr.sin6_port = htons(PORT);
            addr_family = AF_INET6;
            ip_protocol = IPPROTO_IPV6;
            inet6_ntoa_r(dest_addr.sin6_addr, addr_str, sizeof(addr_str) - 1);
    #endif

    int listen_sock = socket(addr_family, SOCK_STREAM, ip_protocol);
    if (listen_sock < 0) {
        ESP_LOGE(TAG, "Unable to create socket: errno %d", errno);
        return -1;
    }
    ESP_LOGI(TAG, "Socket created");

    int err = bind(listen_sock, (struct sockaddr *)&dest_addr, sizeof(dest_addr));
    if (err != 0 && errno != 112) {
        ESP_LOGE(TAG, "Socket unable to bind: errno %d", errno);
        return -1;
    }
    ESP_LOGI(TAG, "Socket bound, port %d", PORT);

    err = listen(listen_sock, 1);
    if (err != 0) {
        ESP_LOGE(TAG, "Error occurred during listen: errno %d", errno);
        return -1;
    }
    ESP_LOGI(TAG, "Socket listening");

    return listen_sock;
}

static int accepted_socket = -1;

static void tcp_server_task(void *pvParameters)
{
    char rx_buffer[2048];
    int listen_sock = bind_port();

    while (listen_sock > 0) {
        struct sockaddr_in6 source_addr; // Large enough for both IPv4 or IPv6
        uint addr_len = sizeof(source_addr);
        accepted_socket = accept(listen_sock, (struct sockaddr *)&source_addr, &addr_len);
        if (accepted_socket < 0) {
            ESP_LOGE(TAG, "Unable to accept connection: errno %d", errno);
            break;
        }
        ESP_LOGI(TAG, "Socket accepted");

        while (1) {
            int len = recv(accepted_socket, rx_buffer, sizeof(rx_buffer) - 1, 0);
            // Error occurred during receiving
            if (len < 0) {
                ESP_LOGE(TAG, "recv failed: errno %d", errno);
                break;
            }
            // Connection closed
            else if (len == 0) {
                ESP_LOGI(TAG, "Connection closed");
                break;
            }
            // Data received
            else {
                char addr_str[128];
                // Get the sender's ip address as string
                if (source_addr.sin6_family == PF_INET) {
                    inet_ntoa_r(((struct sockaddr_in *)&source_addr)->sin_addr.s_addr, addr_str, sizeof(addr_str) - 1);
                } else if (source_addr.sin6_family == PF_INET6) {
                    inet6_ntoa_r(source_addr.sin6_addr, addr_str, sizeof(addr_str) - 1);
                }

                rx_buffer[len] = 0; // Null-terminate whatever we received and treat like a string
                ESP_LOGI(TAG, "Received %d bytes from %s:", len, addr_str);
                ESP_LOGI(TAG, "%s", rx_buffer);

                int err = send(accepted_socket, rx_buffer, len, 0);
                APRS_setPreamble(350);
                APRS_setTail(150);
                APRS_sendPkt(rx_buffer, len);
                ESP_LOGI(TAG, "Transmitted data as AFSK");
                if (err < 0) {
                    ESP_LOGE(TAG, "Error occurred during sending: errno %d", errno);
                    break;
                }
            }
        }

        if (accepted_socket != -1) {
            ESP_LOGE(TAG, "Shutting down socket and restarting...");
            shutdown(accepted_socket, 0);
            close(accepted_socket);
            accepted_socket = -1;
        }
    }
    vTaskDelete(NULL);
}

void start_kiss_server()
{
    ESP_ERROR_CHECK(nvs_flash_init());
    tcpip_adapter_init();
    xTaskCreate(tcp_server_task, "tcp_server", 40960, NULL, 5, NULL);
}


#define BUFLEN (1024)
static char buf[BUFLEN];
void forward_packet_to_kiss(struct AX25Msg *msg) {
    printf("Sending message to kiss socket");
    if (accepted_socket > 0) {
        // TODO: actually format according to KISS.
        printf("%d\n", snprintf(buf, BUFLEN, "Got a message!\n"));
        int err = send(accepted_socket, buf, strnlen(buf, BUFLEN), 0);
        if (err < 0) {
            ESP_LOGE(TAG, "Error occurred during sending: errno %d", errno);
            return;
        }
        printf("%d\n", snprintf(buf, BUFLEN, "SRC: %.6s-%d. ", msg->src.call, msg->src.ssid));
        err = send(accepted_socket, buf, strnlen(buf, BUFLEN), 0);
        if (err < 0) {
            ESP_LOGE(TAG, "Error occurred during sending: errno %d", errno);
            return;
        }
        printf("%d\n", snprintf(buf, BUFLEN, "DST: %.6s-%d. ", msg->dst.call, msg->dst.ssid));
        err = send(accepted_socket, buf, strnlen(buf, BUFLEN), 0);
        if (err < 0) {
            ESP_LOGE(TAG, "Error occurred during sending: errno %d", errno);
            return;
        }
        printf("%d\n", snprintf(buf, BUFLEN, "Data: %.*s\n", msg->len, msg->info));
        err = send(accepted_socket, buf, strnlen(buf, BUFLEN), 0);
        if (err < 0) {
            ESP_LOGE(TAG, "Error occurred during sending: errno %d", errno);
            return;
        }
    }
}