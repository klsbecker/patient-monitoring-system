#include <stdio.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "driver/gpio.h"
#include "esp_log.h"
#include "esp_system.h"
#include "esp_event.h"
#include "nvs_flash.h"
#include "mqtt_client.h"
#include "esp_wifi.h"
#include "mpu6050.h"
#include "driver/i2c.h"
#include "max30201.h"

//Configurações do I2C
#define I2C_MASTER_SCL_IO 22       // GPIO para SCL do I2C
#define I2C_MASTER_SDA_IO 21       // GPIO para SDA do I2C
#define I2C_MASTER_NUM I2C_NUM_0   // Porta I2C
#define I2C_MASTER_FREQ_HZ 100000  // Frequência I2C

// Configurações do MQTT
#define MQTT_URI                    "mqtt://test.mosquitto.org:1883"  // Mudado para protocolo MQTT direto
#define MQTT_TOPIC_ROOT             "patient_monitoring_system_aafk"
#define MQTT_TOPIC_HEARTRATE        MQTT_TOPIC_ROOT "/heart_rate"
#define MQTT_TOPIC_FALL             MQTT_TOPIC_ROOT "/fall"
#define MQTT_TOPIC_AGITATION        MQTT_TOPIC_ROOT "/agitation"

// Configurações principais
#define WIFI_SSID "iPhone de Klaus"
#define WIFI_PASSWORD "batebola"

// Limites para frequência cardíaca
#define HEART_RATE_MIN 60
#define HEART_RATE_MAX 120

// Thresholds para detecção de movimento e estabilidade
#define FALL_THRESHOLD 2.5     // Valor de aceleração G para detectar pico de queda
#define STABILITY_THRESHOLD 0.3 // Estabilidade de aceleração para considerar imóvel
#define AGITATION_THRESHOLD 0.5 // Threshold de variação para detectar agitação
#define STABILITY_DURATION_MS 3000  // Tempo de estabilidade (em ms) para confirmar uma queda

// Configurações do botão de reset (GPIO)
#define RESET_BUTTON_GPIO GPIO_NUM_23  // Usando GPIO0 como exemplo
#define ALARM_LED_GPIO GPIO_NUM_14

// Configurações de FreeRTOS
#define TASK_STACK_SIZE 4096
#define HEART_MONITOR_INTERVAL_MS 10000  // 10 segundos
#define MOVEMENT_MONITOR_INTERVAL_MS 500  // 500 ms para monitoramento frequente
#define ALARM_CHECK_INTERVAL_MS 1000  // Verifica alarmes a cada 1 segundo

// Definição de tipos para fila de alarme
typedef enum {
    ALARM_TRIGGERED,   // Alarme foi ativado
    ALARM_RESET        // Alarme foi resetado
} alarm_event_t;

// Variáveis globais
static const char *TAG = "PatientMonitoringSystem";
int heart_rate = 0;  // Variável simulada para frequência cardíaca
bool fallen = false;
bool agitaded = false;
esp_mqtt_client_handle_t mqtt_client;
QueueHandle_t alarm_queue;
QueueHandle_t reset_button_queue;
bool wifi_connected = false;  // Variável para verificar se Wi-Fi está conectado

// Funções de conexão Wi-Fi e MQTT
void wifi_init();
void mqtt_init();
void mqtt_event_handler(void *handler_args, esp_event_base_t base, int32_t event_id, void *event_data);

// Funções principais das tasks
void task_monitor_heart_rate(void *param);
void task_monitor_movement(void *param);
void task_alarm_handler(void *param);
void task_reset_button_handler(void *param);
void task_wifi_led_indicator(void *param);

// Função para inicializar botão de reset
void reset_button_init();
void led_wifi_init();

// Prototipo da função ISR do botão
static void gpio_isr_handler(void *arg);

void i2c_master_init() {
    i2c_config_t i2c_config = {
        .mode = I2C_MODE_MASTER,
        .sda_io_num = I2C_MASTER_SDA_IO,
        .sda_pullup_en = GPIO_PULLUP_ENABLE,
        .scl_io_num = I2C_MASTER_SCL_IO,
        .scl_pullup_en = GPIO_PULLUP_ENABLE,
        .master.clk_speed = I2C_MASTER_FREQ_HZ,
    };
    i2c_param_config(I2C_MASTER_NUM, &i2c_config);
    i2c_driver_install(I2C_MASTER_NUM, i2c_config.mode, 0, 0, 0);
}

void alarm_led_init() {
    gpio_reset_pin(ALARM_LED_GPIO);
    gpio_set_direction(ALARM_LED_GPIO, GPIO_MODE_OUTPUT);
    gpio_set_level(ALARM_LED_GPIO, 0);  // LED desligado inicialmente
}

void app_main() {
    i2c_master_init();
    mpu6050_init(I2C_MASTER_NUM);
    max30201_init();

    // Inicializar armazenamento NVS para Wi-Fi
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

    // Inicializar Wi-Fi
    wifi_init();
    led_wifi_init();
    // Inicializar botão de reset
    reset_button_init();
    // Inicializar LED local
    alarm_led_init();

    // Criar fila de eventos de alarme
    alarm_queue = xQueueCreate(10, sizeof(alarm_event_t));

    // Criar tasks para monitorar sensores e lidar com alarmes
    xTaskCreate(task_monitor_heart_rate, "MonitorHeartRate", TASK_STACK_SIZE, NULL, 1, NULL);
    xTaskCreate(task_monitor_movement, "MonitorMovement", TASK_STACK_SIZE, NULL, 1, NULL);
    xTaskCreate(task_alarm_handler, "AlarmHandler", TASK_STACK_SIZE, NULL, 1, NULL);
    xTaskCreate(task_reset_button_handler, "ResetButtonHandler", TASK_STACK_SIZE, NULL, 1, NULL);
    xTaskCreate(task_wifi_led_indicator, "WifiLEDIndicator", TASK_STACK_SIZE, NULL, 1, NULL);
}

/**
 * Inicializa a conexão Wi-Fi
 */
void wifi_init() {
    esp_netif_init();
    esp_event_loop_create_default();
    esp_netif_create_default_wifi_sta();

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    esp_wifi_init(&cfg);
    esp_wifi_set_mode(WIFI_MODE_STA);
    esp_wifi_start();

    wifi_config_t sta_config = {
        .sta = {
            .ssid = WIFI_SSID,
            .password = WIFI_PASSWORD,
        },
    };
    esp_wifi_set_config(WIFI_IF_STA, &sta_config);
    esp_wifi_connect();
}

/**
 * Inicializa a conexão MQTT e registra o handler de eventos
 */
void mqtt_init() {
    esp_mqtt_client_config_t mqtt_cfg = {
        .broker.address.uri = MQTT_URI,
    };
    mqtt_client = esp_mqtt_client_init(&mqtt_cfg);
    esp_mqtt_client_register_event(mqtt_client, ESP_EVENT_ANY_ID, mqtt_event_handler, mqtt_client);
    esp_mqtt_client_start(mqtt_client);
}

/**
 * Handler para eventos do MQTT
 */
void mqtt_event_handler(void *handler_args, esp_event_base_t base, int32_t event_id, void *event_data) {
    switch (event_id) {
        case MQTT_EVENT_CONNECTED:
            ESP_LOGI(TAG, "MQTT Connected");
            break;
        case MQTT_EVENT_DISCONNECTED:
            ESP_LOGI(TAG, "MQTT Disconnected");
            break;
        default:
            ESP_LOGI(TAG, "Other MQTT Event: %" PRId32, event_id);
            break;
    }
}

/**
 * Task para monitorar a frequência cardíaca a cada 10 segundos
 */
void task_monitor_heart_rate(void *param) {
    char heart_rate_str[10];
    while (true) {
        heart_rate = max30201_read_data();  // Leitura real do sensor MAX30201
        if (heart_rate >= 0) {  // Confirma que a leitura foi bem-sucedida
            ESP_LOGI(TAG, "Heart Rate: %d bpm", heart_rate);

            sprintf(heart_rate_str, "%d", heart_rate);
            esp_mqtt_client_publish(mqtt_client, MQTT_TOPIC_HEARTRATE, heart_rate_str, strlen(heart_rate_str), 0, 0);
        } else {
            ESP_LOGW(TAG, "Erro ao ler frequência cardíaca");
        }

        vTaskDelay(pdMS_TO_TICKS(HEART_MONITOR_INTERVAL_MS));
    }
}

/**
 * Task para monitorar movimento a cada 500ms (queda/agitação)
 */
void task_monitor_movement(void *param) {
    mpu6050_data_t sensor_data;

    while (true) {
        if (mpu6050_read_accel_gyro(I2C_MASTER_NUM, &sensor_data) == ESP_OK) {
            agitaded |= detect_agitation(&sensor_data);
            ESP_LOGI(TAG, "agitação: %s", agitaded ? "yes" : "no");

            esp_mqtt_client_publish(mqtt_client, MQTT_TOPIC_AGITATION, agitaded ? "true" : "false", 0, 0, 0);

            fallen |= detect_fall(&sensor_data) && !agitaded;
            ESP_LOGI(TAG, "Queda: %s", fallen ? "yes" : "no");

            esp_mqtt_client_publish(mqtt_client, MQTT_TOPIC_FALL, fallen ? "true" : "false", 0, 0, 0);

            if (fallen || agitaded) {
                alarm_event_t event = ALARM_TRIGGERED;
                xQueueSend(alarm_queue, &event, portMAX_DELAY);  // Envia evento de trigger do alarme para a fila
            }

            vTaskDelay(pdMS_TO_TICKS(MOVEMENT_MONITOR_INTERVAL_MS));

        }
    }
}

/**
 * Task que valida o estado de alarme e pode acionar alarmes locais
 */
void task_alarm_handler(void *param) {
    alarm_event_t event;
    while (true) {
        if (xQueueReceive(alarm_queue, &event, portMAX_DELAY)) {
            switch (event) {
                case ALARM_TRIGGERED:
                    ESP_LOGW(TAG, "Alarm triggered! Taking action...");
                    gpio_set_level(ALARM_LED_GPIO, 1);  // Acende o LED de alarme
                    break;
                case ALARM_RESET:
                    ESP_LOGI(TAG, "Alarm reset!");
                    gpio_set_level(ALARM_LED_GPIO, 0);  // Apaga o LED de alarme
                    fallen = agitaded = false;
                    break;
            }
        }
    }
}

/**
 * Task que lida com o botão de reset para desligar o alarme
 */
void task_reset_button_handler(void *param) {
    int button_state = 0;
    while (true) {
        if (xQueueReceive(reset_button_queue, &button_state, portMAX_DELAY)) {
            if (button_state == 1) {
                alarm_event_t event = ALARM_RESET;
                xQueueSend(alarm_queue, &event, portMAX_DELAY);  // Envia evento de reset do alarme para a fila
            }
        }
    }
}

/**
 * Inicializa o botão de reset
 */
void reset_button_init() {
    reset_button_queue = xQueueCreate(10, sizeof(int));

    gpio_config_t io_conf = {
        .pin_bit_mask = (1ULL << RESET_BUTTON_GPIO),
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_POSEDGE  // Interrupção na borda de subida
    };
    gpio_config(&io_conf);

    gpio_install_isr_service(0);
    gpio_isr_handler_add(RESET_BUTTON_GPIO, gpio_isr_handler, (void *)RESET_BUTTON_GPIO);
}

/**
 * Handler de interrupção para o botão de reset
 */
static void IRAM_ATTR gpio_isr_handler(void *arg) {
    int pin = (int)arg;
    int state = gpio_get_level(pin);
    xQueueSendFromISR(reset_button_queue, &state, NULL);
}

#define WIFI_LED_GPIO GPIO_NUM_2  // LED no GPIO 2

// Função para inicializar o LED
void led_wifi_init() {
    gpio_reset_pin(WIFI_LED_GPIO);
    gpio_set_direction(WIFI_LED_GPIO, GPIO_MODE_OUTPUT);
    gpio_set_level(WIFI_LED_GPIO, 0);  // LED desligado inicialmente
}

// Task para piscar o LED enquanto o Wi-Fi não estiver conectado
void task_wifi_led_indicator(void *param) {
    wifi_ap_record_t ap_info;

    while (true) {
        if (esp_wifi_sta_get_ap_info(&ap_info) == ESP_OK) {
            if (!wifi_connected) {
                wifi_connected = true;
                mqtt_init();  // Inicializa o MQTT apenas após conectar ao Wi-Fi
            }
            gpio_set_level(WIFI_LED_GPIO, 1);  // LED ligado (fixo)
        } else {
            wifi_connected = false;
            gpio_set_level(WIFI_LED_GPIO, !gpio_get_level(WIFI_LED_GPIO));  // Pisca o LED
            vTaskDelay(pdMS_TO_TICKS(500));  // Ajuste o delay para a taxa de pisca
        }

        vTaskDelay(pdMS_TO_TICKS(500));  // Reduz frequência de verificação para Wi-Fi
    }
}