#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "driver/gpio.h"
#include "driver/i2s.h"
#include "esp_log.h"
#include "LibAPRS.h"
#include "tcp_kiss.h"
#include "device.h"


// callback from LibAPRS
void aprs_msg_callback(struct AX25Msg *msg) {
    forward_packet_to_kiss(msg);
    printf("Got a message!\n");
    printf("SRC: %.6s-%d. ", msg->src.call, msg->src.ssid);
    printf("DST: %.6s-%d. ", msg->dst.call, msg->dst.ssid);
    printf("Data: %.*s\n", msg->len, msg->info);
}

void start_wifi(); // from softap.c
esp_err_t app_main() {
    APRS_init(0, false);
    APRS_setCallsign("KC9IAE", 1);
    APRS_setDestination("APRS", 0);
    APRS_setPath1("WIDE1", 1);
    APRS_setPath1("WIDE2", 1);

    start_wifi();
    start_kiss_server();

    return 0;
}

int btstack_main(int argc, const char * argv[]) {
    return 0;
}
