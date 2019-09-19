#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "driver/gpio.h"
#include "driver/i2s.h"
#include "esp_log.h"
#include "LibAPRS.h"
#include "tcp_kiss.h"
#include "device.h"

#define GPIO_AUDIO_TRIGGER 37
// #define GPIO_AUDIO_IN 36
#define GPIO_AUDIO_OUT 25
#define ESP_INTR_FLAG_DEFAULT 0


static xQueueHandle gpio_evt_queue = NULL;

uint16_t audio_buf1[TNC_I2S_BUFLEN];
uint16_t audio_buf2[TNC_I2S_BUFLEN];

#define FULL_BUF_LEN (DESIRED_SAMPLE_RATE * 2)
int8_t audio_buf_full[FULL_BUF_LEN];
size_t audio_buf_full_idx = 0;

static void IRAM_ATTR gpio_isr_handler(void* arg)
{
    uint32_t gpio_num = (uint32_t) arg;
    xQueueSendFromISR(gpio_evt_queue, &gpio_num, NULL);
}

void record_audio(uint16_t *buffer) {
    size_t bytes_read;
    esp_err_t error = i2s_read(TNC_I2S_NUM, (void*) buffer, TNC_I2S_BUFLEN * sizeof(uint16_t), &bytes_read, portMAX_DELAY /*(3 * TNC_I2S_SAMPLE_RATE / TNC_I2S_BUFLEN / 2 / portTICK_PERIOD_MS) */);

    for (int i=0; i<TNC_I2S_BUFLEN; i++) {
        buffer[i] = 4095 - buffer[i];
    }

    // printf("error %d, read %d bytes\n", error, bytes_read);
}

void process_audio(uint16_t *buffer) {
    // printf("processing buffer %d\n", (int)buffer);
    for (int i=0; i<TNC_I2S_BUFLEN; i += OVERSAMPLING) {
        int average = 0;
        for (int j=0; j<OVERSAMPLING && j+i < TNC_I2S_BUFLEN; j++) {
            average += buffer[i+j];
        }
        average /= OVERSAMPLING;

        average -= 717; // empirically measured hackily, we high-pass-filter later so this doesn't matter much.
        average = average * 255 / 1564;
        if (average < -128) average = -128;
        if (average > 127) average = 127;

        if (audio_buf_full_idx < FULL_BUF_LEN) {
            audio_buf_full[audio_buf_full_idx++] = average;
        }
    }
}


void example_i2s_init()
{
     int i2s_num = TNC_I2S_NUM;
     i2s_config_t i2s_config = {
        .mode = I2S_MODE_MASTER | I2S_MODE_RX | I2S_MODE_TX | I2S_MODE_DAC_BUILT_IN | I2S_MODE_ADC_BUILT_IN,
        .sample_rate =  TNC_I2S_SAMPLE_RATE,
        .bits_per_sample = TNC_I2S_SAMPLE_BITS,
        .communication_format = I2S_COMM_FORMAT_I2S_MSB,
        .channel_format = TNC_I2S_FORMAT,
        .intr_alloc_flags = 0,
        .dma_buf_count = 2,
        .dma_buf_len = 300,
        // .use_apll = 1,
        .use_apll = 0,
     };
     //install and start i2s driver
     i2s_driver_install(i2s_num, &i2s_config, 0, NULL);
     //init DAC pad
     i2s_set_dac_mode(I2S_DAC_CHANNEL_BOTH_EN);
     //init ADC pad
     i2s_set_adc_mode(I2S_ADC_UNIT, I2S_ADC_CHANNEL);
     adc1_config_channel_atten(I2S_ADC_CHANNEL, ADC_ATTEN_DB_11);
}


void receive_audio_task() {
    uint32_t io_num;
    for(;;) {
        if(xQueueReceive(gpio_evt_queue, &io_num, portMAX_DELAY)) {
            gpio_isr_handler_remove(GPIO_AUDIO_TRIGGER);
            uint32_t bogus;
            while (xQueueReceive(gpio_evt_queue, &bogus, 0)); // clear queue

            // grab audio lock
            // until tail N bytes of buffer are < threshold:
            //   record audio into buffer
            //   swap to other buffer
            //   send audio buffer to queue for processing
            ESP_LOG_LEVEL(ESP_LOG_INFO, "receive_audio_task", "GPIO[%d] intr, val: %d\n", io_num, gpio_get_level(io_num));

            uint16_t *buffer = audio_buf1;

            int num_recordings = 0;

            i2s_adc_enable(TNC_I2S_NUM);

            for (bool keep_recording = true; keep_recording; ) {
                num_recordings++;
                record_audio(buffer);
                process_audio(buffer);

                // printf("still recording\n");
                keep_recording = false;
                for (int i=0; i<TNC_I2S_BUFLEN; i++) {
                    if (buffer[i] > KEEP_RECORDING_THRESH) {
                        keep_recording = true;
                        break;
                    }
                }

                //switch to other buffer.
                if (buffer == audio_buf1) {
                    buffer = audio_buf2;
                    // printf("switched to audio_buf2\n");
                } else {
                    buffer = audio_buf1;
                    // printf("switched to audio_buf1\n");
                }
            }

            i2s_adc_disable(TNC_I2S_NUM);

            int running_sum = 0;
            uint8_t running_sum_len = 0;
            #define MAX_RUNNING_SUM_LEN (DESIRED_SAMPLE_RATE / 600)
            ESP_LOG_LEVEL(ESP_LOG_INFO, "receive_audio_task", "did %d recordings in %d ticks\n", num_recordings, 0);
            uint8_t poll_timer = 0;

            // viterbi(1.0);

            for (int i=0; i<audio_buf_full_idx; i++) {
                int16_t sample = audio_buf_full[i];
                running_sum += sample;
                if (running_sum_len >= MAX_RUNNING_SUM_LEN) {
                    running_sum -= audio_buf_full[i - (running_sum_len)];
                } else {
                    running_sum_len++;
                }

                sample -= running_sum / running_sum_len;
                if (sample > 127) sample = 127;
                if (sample < -128) sample = 128;

                // printf("%d ", sample);
                // if (i%20 == 19) printf("\n");

                AFSK_adc_isr(AFSK_modem, sample);
                poll_timer++;
                if (poll_timer > 3) {
                    poll_timer = 0;
                    APRS_poll();
                }
            }

            audio_buf_full_idx = 0;

            gpio_isr_handler_add(GPIO_AUDIO_TRIGGER, gpio_isr_handler, (void*) GPIO_AUDIO_TRIGGER);
        }
    }
}


// callback from LibAPRS
void aprs_msg_callback(struct AX25Msg *msg) {
    forward_packet_to_kiss(msg);
    printf("Got a message!\n");
    printf("SRC: %.6s-%d. ", msg->src.call, msg->src.ssid);
    printf("DST: %.6s-%d. ", msg->dst.call, msg->dst.ssid);
    printf("Data: %.*s\n", msg->len, msg->info);
}

void set_up_interrupts() {
    gpio_config_t io_conf;
    //interrupt of rising edge
    io_conf.intr_type = GPIO_PIN_INTR_POSEDGE;
    //bit mask of the pins, use GPIO4/5 here
    io_conf.pin_bit_mask = (1ULL<<GPIO_AUDIO_TRIGGER);
    //set as input mode
    io_conf.mode = GPIO_MODE_INPUT;
    //enable pull-up mode
    io_conf.pull_up_en = 1;
    gpio_config(&io_conf);

    //create a queue to handle gpio event from isr
    gpio_evt_queue = xQueueCreate(10, sizeof(uint32_t));
    //start gpio task
    xTaskCreate(receive_audio_task, "receive_audio_task", 2048, NULL, 10, NULL);

    //install gpio isr service
    gpio_install_isr_service(ESP_INTR_FLAG_DEFAULT);
    //hook isr handler for specific gpio pin
    gpio_isr_handler_add(GPIO_AUDIO_TRIGGER, gpio_isr_handler, (void*) GPIO_AUDIO_TRIGGER);
}

void start_wifi(); // from softap.c
esp_err_t app_main() {
    // set up interrupt on input pin to call receive_audio_task
    APRS_init(0, false);
    APRS_setCallsign("KC9IAE", 1);
    APRS_setDestination("APRS", 0);
    APRS_setPath1("WIDE1", 1);
    APRS_setPath1("WIDE2", 1);
    set_up_interrupts();
    example_i2s_init();

    start_wifi();
    start_kiss_server();

    // start process_audio_task on other core
    // wait?
    return 0;
}

int btstack_main(int argc, const char * argv[]) {
    return 0;
}
