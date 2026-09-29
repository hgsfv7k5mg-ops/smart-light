/**
 * @file temp_sensor.c
 * @brief DS18B20 驱动实现（基于 espressif/onewire_bus + ds18b20 组件）
 *
 * 两个容易被忽略的细节：
 *   1) 1-Wire 时隙只有几十微秒，总线事务期间临时提升任务优先级，避免被抢占导致时序错位；
 *   2) 读到 85℃（0x0550 上电默认值）时不能直接当真值上报，需重新触发转换（最多 3 次）。
 *
 * 总线后端由 BOARD_ONEWIRE_BACKEND 选择：1 = RMT（默认），2 = UART。
 */
#include "temp_sensor.h"

#include <stdio.h>
#include <string.h>
#include "board_config.h"
#include "ds18b20.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "onewire_bus.h"
#include "onewire_bus_impl_rmt.h"
#include "onewire_bus_impl_uart.h"
#include "onewire_cmd.h"

static const char *TAG = "temp_sensor";

#define INIT_RETRY          3       /* 上电时总线可能不稳，重试几次 */
#define READ_RETRY          3       /* 85℃ 上电值需要重转 */
#define RAW_SCRATCHPAD_CMD  0xBE

static onewire_bus_handle_t     s_bus = NULL;
static ds18b20_device_handle_t  s_dev = NULL;
static uint64_t                 s_addr = 0;
static uint32_t                 s_ok_count   = 0;
static uint32_t                 s_fail_count = 0;
static const char              *s_last_err   = "尚未读取";

/* -------------------------------------------------------------------------- */
/*                                 错误翻译                                     */
/* -------------------------------------------------------------------------- */
static const char *err_text(esp_err_t err)
{
    switch (err) {
    case ESP_ERR_INVALID_CRC:   return "CRC 校验失败（上拉/接线/干扰）";
    case ESP_ERR_INVALID_STATE: return "读到上电默认值 85℃（转换未完成或寄生供电）";
    case ESP_ERR_NOT_SUPPORTED: return "设备家族码不是 0x28，可能不是 DS18B20";
    case ESP_ERR_NOT_FOUND:     return "总线上未发现任何 1-Wire 设备";
    case ESP_ERR_TIMEOUT:       return "总线无响应（检查 4.7k 上拉电阻）";
    default:                    return esp_err_to_name(err);
    }
}

/* 1-Wire 时隙只有几十微秒，总线事务中若被高优先级任务抢占会导致复位/读写错位
   （表现为 reset 失败或 CRC 错）。故每次事务前后临时提升当前任务优先级。 */
static UBaseType_t onewire_begin(void)
{
    UBaseType_t old = uxTaskPriorityGet(NULL);
    vTaskPrioritySet(NULL, configMAX_PRIORITIES - 1);
    return old;
}

static void onewire_end(UBaseType_t old_prio)
{
    vTaskPrioritySet(NULL, old_prio);
}

/* -------------------------------------------------------------------------- */
/*                                  初始化                                      */
/* -------------------------------------------------------------------------- */
static void release_all(void)
{
    if (s_dev) {
        ds18b20_del_device(s_dev);
        s_dev = NULL;
    }
    if (s_bus) {
        onewire_bus_del(s_bus);
        s_bus = NULL;
    }
}

static esp_err_t create_device(void)
{
    /*
     * en_pull_up = 1：打开 ESP32 内部上拉（约 45kΩ）。
     * 外部有 4.7kΩ 时它只是并联，无害；外部没接或虚焊时，它能让总线有确定电平，
     * 避免引脚浮空产生毛刺 —— 毛刺正是 RMT 报 "user buffer too small" 的元凶。
     */
    onewire_bus_config_t bus_cfg = {
        .bus_gpio_num = BOARD_DS18B20_GPIO,
        .flags = {
            .en_pull_up = 1,
        },
    };

    esp_err_t err;
#if (BOARD_ONEWIRE_BACKEND == 2)
    onewire_bus_uart_config_t uart_cfg = {
        .uart_port_num = BOARD_ONEWIRE_UART_PORT,
    };
    err = onewire_new_bus_uart(&bus_cfg, &uart_cfg, &s_bus);
    ESP_LOGI(TAG, "1-Wire 后端: UART%d (GPIO%d)", BOARD_ONEWIRE_UART_PORT,
             BOARD_DS18B20_GPIO);
#else
    onewire_bus_rmt_config_t rmt_cfg = {
        .max_rx_bytes = 20,
    };
    err = onewire_new_bus_rmt(&bus_cfg, &rmt_cfg, &s_bus);
    ESP_LOGI(TAG, "1-Wire 后端: RMT (GPIO%d)", BOARD_DS18B20_GPIO);
#endif
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "1-Wire 总线创建失败: %s", esp_err_to_name(err));
        return err;
    }

    onewire_device_iter_handle_t iter = NULL;
    onewire_device_t device;
    bool found = false;

    if (onewire_new_device_iter(s_bus, &iter) != ESP_OK) {
        ESP_LOGE(TAG, "设备迭代器创建失败");
        return ESP_FAIL;
    }
    while (onewire_device_iter_get_next(iter, &device) == ESP_OK) {
        ESP_LOGI(TAG, "发现 1-Wire 设备: 0x%016llX (家族码 0x%02X)",
                 (unsigned long long)device.address, (unsigned)(device.address & 0xFF));
        found = true;
        break;
    }
    onewire_del_device_iter(iter);

    if (!found) {
        return ESP_ERR_NOT_FOUND;
    }

    ds18b20_config_t ds_cfg;
    memset(&ds_cfg, 0, sizeof(ds_cfg));

    err = ds18b20_new_device_from_enumeration(&device, &ds_cfg, &s_dev);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "按地址创建失败(%s)，改用 Skip-ROM 方式", esp_err_to_name(err));
        err = ds18b20_new_device_from_bus(s_bus, &ds_cfg, &s_dev);
        if (err != ESP_OK) {
            return err;
        }
        s_addr = 0;
    } else {
        s_addr = device.address;
    }

    /* 显式设为 12bit（0.0625℃ 步进，转换约 750ms） */
    if (ds18b20_set_resolution(s_dev, DS18B20_RESOLUTION_12B) != ESP_OK) {
        ESP_LOGW(TAG, "设置 12bit 分辨率失败，沿用芯片默认值");
    }
    return ESP_OK;
}

esp_err_t temp_sensor_init(void)
{
    for (int i = 1; i <= INIT_RETRY; i++) {
        esp_err_t err = create_device();
        if (err == ESP_OK) {
            ESP_LOGI(TAG, "DS18B20 就绪 (GPIO%d, 地址 0x%016llX)",
                     BOARD_DS18B20_GPIO, (unsigned long long)s_addr);
            return ESP_OK;
        }
        ESP_LOGW(TAG, "第 %d/%d 次探测失败: %s", i, INIT_RETRY, err_text(err));
        release_all();
        vTaskDelay(pdMS_TO_TICKS(500));
    }

    s_last_err = err_text(ESP_ERR_NOT_FOUND);
    ESP_LOGE(TAG, "========== DS18B20 不可用 ==========");
    ESP_LOGE(TAG, "  GPIO%d 上未找到设备，温度功能禁用", BOARD_DS18B20_GPIO);
    ESP_LOGE(TAG, "  检查：VCC/GND 接 3.3V、DQ 有 4.7kΩ 上拉、芯片符合 DS18B20 时序规范");
#if (BOARD_ONEWIRE_BACKEND == 1)
    ESP_LOGE(TAG, "  若日志里有 'rmt: user buffer too small'，说明总线电平不干净");
    ESP_LOGE(TAG, "  （浮空/上拉不足/干扰）：确认 4.7k 上拉，或把 board_config.h 里的");
    ESP_LOGE(TAG, "  BOARD_ONEWIRE_BACKEND 改成 2 换 UART 后端");
#endif
    ESP_LOGE(TAG, "====================================");
    return ESP_FAIL;
}

bool temp_sensor_is_ready(void)
{
    return (s_dev != NULL);
}

esp_err_t temp_sensor_rescan(void)
{
    if (s_dev != NULL) {
        return ESP_OK;                  /* 已经就绪，无需重扫 */
    }

    release_all();                      /* 清掉上次失败留下的残留句柄 */
    ESP_LOGI(TAG, "重新扫描 1-Wire 总线 (GPIO%d)...", BOARD_DS18B20_GPIO);

    esp_err_t err = temp_sensor_init();
    if (err == ESP_OK) {
        s_last_err = "正常（重扫后恢复）";
        s_ok_count = 0;
        s_fail_count = 0;
    }
    return err;
}

/* -------------------------------------------------------------------------- */
/*                                   读取                                       */
/* -------------------------------------------------------------------------- */
esp_err_t temp_sensor_read(float *celsius)
{
    if (s_dev == NULL || celsius == NULL) {
        return ESP_ERR_INVALID_STATE;
    }

    UBaseType_t old_prio = onewire_begin();

    esp_err_t err = ESP_FAIL;
    float value = 0.0f;

    for (int i = 0; i < READ_RETRY; i++) {
        /* trigger 内部已按分辨率等待转换完成（12bit = 800ms） */
        err = ds18b20_trigger_temperature_conversion(s_dev);
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "触发转换失败: %s", err_text(err));
            continue;
        }

        err = ds18b20_get_temperature(s_dev, &value);
        if (err == ESP_OK) {
            break;
        }
        /* 上电默认值 85℃：再转一次通常就正常了 */
        if (err == ESP_ERR_INVALID_STATE) {
            ESP_LOGW(TAG, "第 %d 次读到 85℃ 上电默认值，重新转换", i + 1);
            continue;
        }
        ESP_LOGW(TAG, "读取温度失败: %s", err_text(err));
    }

    onewire_end(old_prio);

    if (err == ESP_OK) {
        s_ok_count++;
        s_last_err = "正常";
        *celsius = value;
    } else {
        s_fail_count++;
        s_last_err = err_text(err);
    }
    return err;
}

esp_err_t temp_sensor_read_raw(uint8_t out[9])
{
    if (s_bus == NULL || out == NULL) {
        return ESP_ERR_INVALID_STATE;
    }

    UBaseType_t old_prio = onewire_begin();

    esp_err_t err = onewire_bus_reset(s_bus);
    if (err == ESP_OK) {
        uint8_t cmd[2] = {ONEWIRE_CMD_SKIP_ROM, RAW_SCRATCHPAD_CMD};
        err = onewire_bus_write_bytes(s_bus, cmd, sizeof(cmd));
    }
    if (err == ESP_OK) {
        err = onewire_bus_read_bytes(s_bus, out, 9);
    }

    onewire_end(old_prio);
    return err;
}

esp_err_t temp_sensor_probe_presence(bool *present)
{
    if (s_bus == NULL || present == NULL) {
        return ESP_ERR_INVALID_STATE;
    }

    UBaseType_t old_prio = onewire_begin();
    esp_err_t err = onewire_bus_reset(s_bus);
    onewire_end(old_prio);

    *present = (err == ESP_OK);
    return err;
}

int temp_sensor_diag(char *buf, size_t len)
{
    if (buf == NULL || len == 0) {
        return 0;
    }
    if (s_dev == NULL) {
        return snprintf(buf, len, "DS18B20 不可用 | %s", s_last_err);
    }
    return snprintf(buf, len,
                    "地址 0x%016llX | 成功 %lu 次 失败 %lu 次 | 最近: %s",
                    (unsigned long long)s_addr,
                    (unsigned long)s_ok_count, (unsigned long)s_fail_count,
                    s_last_err);
}
