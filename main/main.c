#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "driver/gpio.h"
#include "driver/i2s.h"
#include "esp_log.h"



#define GPIO_AUDIO_TRIGGER 37
// #define GPIO_AUDIO_IN 36
#define GPIO_AUDIO_OUT 25
#define ESP_INTR_FLAG_DEFAULT 0


static xQueueHandle gpio_evt_queue = NULL;
static xQueueHandle send_audio_queue = NULL;
// TODO: make lock
// TODO: make 2 audio buffers.

//i2s number
#define TNC_I2S_NUM           (0)
//i2s sample rate
#define TNC_I2S_SAMPLE_RATE   (9600)
//i2s data bits
#define TNC_I2S_SAMPLE_BITS   (16)
// 125ms of audio should be plenty I think
#define TNC_I2S_BUFLEN        (TNC_I2S_SAMPLE_RATE / 8)

//I2S data format
#define TNC_I2S_FORMAT        (I2S_CHANNEL_FMT_ONLY_RIGHT)
#define TNC_I2S_CHANNEL_NUM   (1)

//I2S built-in ADC unit
#define I2S_ADC_UNIT              ADC_UNIT_1
#define I2S_ADC_CHANNEL           ADC1_CHANNEL_0

#define KEEP_RECORDING_THRESH  (5)

uint16_t audio_buf1[TNC_I2S_BUFLEN];
uint16_t audio_buf2[TNC_I2S_BUFLEN];

static void IRAM_ATTR gpio_isr_handler(void* arg)
{
    uint32_t gpio_num = (uint32_t) arg;
    xQueueSendFromISR(gpio_evt_queue, &gpio_num, NULL);
}

void record_audio(uint16_t *buffer) {
    i2s_adc_enable(TNC_I2S_NUM);
    size_t bytes_read;
    esp_err_t error = i2s_read(TNC_I2S_NUM, (void*) buffer, TNC_I2S_BUFLEN * sizeof(uint16_t), &bytes_read, portMAX_DELAY /*(3 * TNC_I2S_SAMPLE_RATE / TNC_I2S_BUFLEN / 2 / portTICK_PERIOD_MS) */);
    i2s_adc_disable(TNC_I2S_NUM);

    for (int i=0; i<TNC_I2S_BUFLEN; i++) {
        buffer[i] = 4095 - buffer[i];
    }

    // printf("error %d, read %d bytes\n", error, bytes_read);
}


void process_audio(uint16_t *buffer) {
    printf("processing buffer %d\n", (int)buffer);
    for (int i=0; i<TNC_I2S_BUFLEN; i++) {
        printf(" %4d", buffer[i]);
        if (i%20 == 0) {
            printf("\n");
        }
    }
    // for byte in bytes:
    //   call function to process adc value
}

void send_audio_task() {
    // wait for data from queue
    uint32_t blah;
    for (;;) {
        if (xQueueReceive(send_audio_queue, &blah, portMAX_DELAY)) {
            // while data to send:
            //   grab audio lock if don't have
            //   modulate data into buffer
            //   send buffer
            // release lock
            printf("Would have sent audio\n");  
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
        .dma_buf_len = 1024,
        .use_apll = 1,
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

            ESP_LOG_LEVEL(ESP_LOG_INFO, "receive_audio_task", "did %d recordings in %d ticks\n", num_recordings, 0);

            gpio_isr_handler_add(GPIO_AUDIO_TRIGGER, gpio_isr_handler, (void*) GPIO_AUDIO_TRIGGER);
        }
    }
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
    send_audio_queue = xQueueCreate(10, sizeof(uint32_t));
    //start gpio task
    xTaskCreate(receive_audio_task, "receive_audio_task", 2048, NULL, 10, NULL);

    //install gpio isr service
    gpio_install_isr_service(ESP_INTR_FLAG_DEFAULT);
    //hook isr handler for specific gpio pin
    gpio_isr_handler_add(GPIO_AUDIO_TRIGGER, gpio_isr_handler, (void*) GPIO_AUDIO_TRIGGER);
}

esp_err_t app_main() {
    // set up interrupt on input pin to call receive_audio_task
    set_up_interrupts();
    example_i2s_init();
    // start send_audio_task
    xTaskCreate(send_audio_task, "send_audio_task", 2048, NULL, 10, NULL);

    // start process_audio_task on other core
    // wait?
    return 0;
}

int btstack_main(int argc, const char * argv[]) {
    return 0;
}