/*
 * dht11.h — DHT11 温湿度传感器 (单总线)
 */
#pragma once
#include <stdbool.h>

typedef struct {
    float temperature;  /* °C */
    float humidity;     /* %RH */
} dht11_data_t;

void dht11_init(void);
bool dht11_read(dht11_data_t *out);  /* true=读取成功 */
