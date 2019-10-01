#include "esp_https_server.h"
#include "esp_log.h"
#include "LibAPRS.h"

esp_err_t handler_get_root(httpd_req_t *req) {
    extern const char resp[] asm("_binary_index_html_start");
    httpd_resp_send(req, resp, strlen(resp));
    return ESP_OK;
}

void format_latlon(float fDegrees, char *buf, size_t buflen, bool is_lat) {
    bool southwest = false;
    if (fDegrees < 0.0) {
        fDegrees *= -1;
        southwest = true;
    }

    int degrees = (int)fDegrees;
    fDegrees -= degrees;
    fDegrees *= 60;
    int minutes = (int)fDegrees;
    fDegrees -= minutes;
    fDegrees *= 60;
    int seconds = (int)fDegrees;

    char nsew;
    if (is_lat) {
        if (southwest) {
            nsew = 'S';
        } else {
            nsew = 'N';
        }
    } else {
        if (southwest) {
            nsew = 'W';
        } else {
            nsew = 'E';
        }
    }

    if (is_lat) {
        snprintf(buf, buflen, "%02d%02d.%02d%c", degrees, minutes, seconds, nsew);
    } else {
        snprintf(buf, buflen, "%03d%02d.%02d%c", degrees, minutes, seconds, nsew);
    }
}

esp_err_t handler_post_location(httpd_req_t *req) {
    char content[100];
    /* Truncate if content length larger than the buffer */
    size_t recv_size = req->content_len < sizeof(content) ? req->content_len : sizeof(content);

    int ret = httpd_req_recv(req, content, recv_size);
    if (ret <= 0) {  /* 0 return value indicates connection closed */
        /* Check if timeout occurred */
        if (ret == HTTPD_SOCK_ERR_TIMEOUT) {
            /* In case of timeout one can choose to retry calling
             * httpd_req_recv(), but to keep it simple, here we
             * respond with an HTTP 408 (Request Timeout) error */
            httpd_resp_send_408(req);
        }
        /* In case of error, returning ESP_FAIL will
         * ensure that the underlying socket is closed */
        return ESP_FAIL;
    }
    content[ret] = '\0';

    char valbuf[20];
    char outbuf[10]; // 10 (9+\0) for longitude, 9 (8+\0) for latitude

    httpd_query_key_value(content, "longitude", valbuf, sizeof(valbuf));
    format_latlon(atof(valbuf), outbuf, 10, false);
    APRS_setLon(outbuf);

    httpd_query_key_value(content, "latitude", valbuf, sizeof(valbuf));
    format_latlon(atof(valbuf), outbuf, 10, true);
    APRS_setLat(outbuf);

    // char aprsbuf[1024];
    APRS_sendLoc(NULL, 0);

    /* Send a simple response */
    const char resp[] = "URI POST Response";
    httpd_resp_send(req, resp, strlen(resp));
    return ESP_OK;
}

httpd_uri_t uri_get_root = {
    .uri      = "/",
    .method   = HTTP_GET,
    .handler  = handler_get_root,
    .user_ctx = NULL
};

httpd_uri_t uri_post_location = {
    .uri      = "/location",
    .method   = HTTP_POST,
    .handler  = handler_post_location,
    .user_ctx = NULL
};

httpd_handle_t start_webserver(void) {
    /* Generate default configuration */
    httpd_ssl_config_t conf = HTTPD_SSL_CONFIG_DEFAULT();

    extern const unsigned char cacert_pem_start[] asm("_binary_cert_pem_start");
    extern const unsigned char cacert_pem_end[]   asm("_binary_cert_pem_end");
    conf.cacert_pem = cacert_pem_start;
    conf.cacert_len = cacert_pem_end - cacert_pem_start;

    extern const unsigned char private_key_start[] asm("_binary_private_key_start");
    extern const unsigned char private_key_end[]   asm("_binary_private_key_end");
    conf.prvtkey_pem = private_key_start;
    conf.prvtkey_len = private_key_end - private_key_start;

    /* Empty handle to esp_http_server */
    httpd_handle_t server = NULL;

    /* Start the httpd server */
    if (httpd_ssl_start(&server, &conf) == ESP_OK) {
        /* Register URI handlers */
        httpd_register_uri_handler(server, &uri_get_root);
        httpd_register_uri_handler(server, &uri_post_location);
    }
    /* If server failed to start, handle will be NULL */
    return server;
}