/**
 ****************************************************************************************************
 * @file        template.c
 * @author      普中科技（改造示例）
 * @version     V1.0
 * @date        2025-12-23
 * @brief       WiFi 通信实验：MQTT远程控制LED亮度并上报光照强度
 ****************************************************************************************************
 * @attention
 *
 * 实验平台: 普中-Hi3861
 * 参考实验: 16 PWM、26 WiFi MQTT、41 光敏传感器
 *
 * 实验内容：
 * - 通过WiFi连接到MQTT Broker
 * - 定期采集光敏传感器ADC值并发布到主题
 * - 订阅主题以控制LED亮度（0-100；0表示灭）
 *
 ****************************************************************************************************
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "ohos_init.h"
#include "cmsis_os2.h"

#include "bsp_led.h"
#include "bsp_pwm.h"
#include "bsp_adc.h"
#include "bsp_wifi.h"
#include "bsp_mqtt.h"

#include "lwip/netifapi.h"
#include "lwip/sockets.h"
#include "lwip/api_shell.h"

// ========================= 配置区域 =========================
// WiFi 热点配置（改成你的手机热点名称和密码）
#ifndef WIFI_SSID
#define WIFI_SSID "test" // ← 改成你的手机热点名称
#endif
#ifndef WIFI_PAWD
#define WIFI_PAWD "testpassword1" // ← 改成你的手机热点密码
#endif

// MQTT Broker 配置（使用公网免费服务器）
// 注意：Hi3861 不支持域名，必须用 IP 地址！
// broker.emqx.io 的IP会变化，可用 nslookup broker.emqx.io 查询最新IP
#ifndef MQTT_SERVER_IP
#define MQTT_SERVER_IP "35.172.255.228" // broker.emqx.io 当前IP
#endif
#ifndef MQTT_SERVER_PORT
#define MQTT_SERVER_PORT 1883
#endif

// MQTT 主题配置
#ifndef MQTT_TOPIC_PUB_LIGHT
#define MQTT_TOPIC_PUB_LIGHT "hi3861/sensor/light"
#endif
#ifndef MQTT_TOPIC_SUB_BRIGHTNESS
#define MQTT_TOPIC_SUB_BRIGHTNESS "hi3861/led/brightness"
#endif

// 发布与订阅任务时间间隔
#define TASK_INIT_DELAY_S 2                     // seconds，初始化阶段各环节的间隔
#define MQTT_RECV_TASK_INTERVAL_US (200 * 1000) // 接收轮询间隔
#define LIGHT_PUB_INTERVAL_S 2                  // 光照上报间隔

// PWM 占空比范围（参考实验16）
#define PWM_DUTY_MIN 0
#define PWM_DUTY_MAX 3000

// ========================= 任务与句柄 =========================
static osThreadId_t g_mqtt_send_task_id; // MQTT发布与系统初始化任务
static osThreadId_t g_mqtt_recv_task_id; // MQTT订阅接收轮询任务

// ========================= 工具函数 =========================
// 将 0-100 的亮度映射到 PWM 占空比 0-3000
static inline uint16_t BrightnessToDuty(int brightness)
{
    if (brightness <= 0)
        return PWM_DUTY_MIN;
    if (brightness >= 100)
        return PWM_DUTY_MAX;
    // 线性映射：100 -> 3000（全整数运算，避免引入浮点）
    return (uint16_t)(((uint32_t)brightness * (uint32_t)PWM_DUTY_MAX) / 100U);
}

// 从 payload 中解析 0-100 的亮度值。
// 注意：回调只给指针不含长度，这里不依赖 '\0' 结尾，最多读取 3 位数字。
static int ParseBrightnessPayload(const unsigned char *payload)
{
    if (payload == NULL)
        return -1;

    int value = 0;
    int digits = 0;
    for (int i = 0; i < 4; i++)
    {
        unsigned char c = payload[i];
        if (c < '0' || c > '9')
        {
            break;
        }
        value = value * 10 + (c - '0');
        digits++;
        if (value > 100)
        {
            value = 100;
            break;
        }
    }
    if (digits == 0)
        return -1;
    if (value < 0)
        value = 0;
    if (value > 100)
        value = 100;
    return value;
}

// ========================= 订阅回调 =========================
// 收到亮度控制消息时的回调：payload 为字符串数字 "0-100"
int8_t mqtt_sub_payload_callback(unsigned char *topic, unsigned char *payload)
{
    if (topic == NULL || payload == NULL)
    {
        printf("[warn] MQTT callback: null topic/payload\r\n");
        return -1;
    }

    // 将 payload 解析为整数亮度值（0-100）
    int brightness = ParseBrightnessPayload(payload);
    if (brightness < 0)
    {
        printf("[warn] topic:[%s] invalid payload\r\n", topic);
        return -1;
    }

    uint16_t duty = BrightnessToDuty(brightness);
    pwm_set_duty(duty);

    printf("[info] topic:[%s] set brightness=%d => duty=%u\r\n", topic, brightness, duty);
    return 0;
}

// ========================= 接收轮询任务 =========================
static void mqtt_recv_task(void)
{
    while (1)
    {
        // 驱动内部轮询并触发回调（参考实验26）
        MQTTClient_sub();
        usleep(MQTT_RECV_TASK_INTERVAL_US);
    }
}

// ========================= 发送与系统初始化任务 =========================
static void mqtt_send_task(void)
{
    // 1. 基础外设初始化：LED、PWM、ADC
    led_init();
    pwm_init();
    adc5_init();

    // 默认给一个较低亮度，避免突兀
    pwm_set_duty(BrightnessToDuty(10));
    LED(1);

    // 2. 连接 WiFi
    if (WiFi_connectHotspots(WIFI_SSID, WIFI_PAWD) != WIFI_SUCCESS)
    {
        printf("[error] WiFi_connectHotspots\r\n");
    }
    else
    {
        printf("[success] WiFi connected: SSID=%s\r\n", WIFI_SSID);
    }

    sleep(TASK_INIT_DELAY_S);

    // 3. 连接 MQTT 服务器
    if (MQTTClient_connectServer(MQTT_SERVER_IP, MQTT_SERVER_PORT) != 0)
    {
        printf("[error] MQTTClient_connectServer\r\n");
    }
    else
    {
        printf("[success] MQTTClient_connectServer\r\n");
    }

    sleep(TASK_INIT_DELAY_S);

    // 4. 初始化 MQTT 客户端
    if (MQTTClient_init("hi3861_client", "username", "password") != 0)
    {
        printf("[error] MQTTClient_init\r\n");
    }
    else
    {
        printf("[success] MQTTClient_init\r\n");
    }

    sleep(TASK_INIT_DELAY_S);

    // 5. 设置订阅回调并订阅亮度控制主题
    p_MQTTClient_sub_callback = &mqtt_sub_payload_callback;
    if (MQTTClient_subscribe(MQTT_TOPIC_SUB_BRIGHTNESS) != 0)
    {
        printf("[error] MQTTClient_subscribe:%s\r\n", MQTT_TOPIC_SUB_BRIGHTNESS);
    }
    else
    {
        printf("[success] MQTTClient_subscribe:%s\r\n", MQTT_TOPIC_SUB_BRIGHTNESS);
    }

    sleep(TASK_INIT_DELAY_S);

    // 6. 创建接收轮询任务
    osThreadAttr_t recvOpt;
    recvOpt.name = "mqtt_recv_task";
    recvOpt.attr_bits = 0;
    recvOpt.cb_mem = NULL;
    recvOpt.cb_size = 0;
    recvOpt.stack_mem = NULL;
    recvOpt.stack_size = 1024 * 5;
    recvOpt.priority = osPriorityNormal;

    g_mqtt_recv_task_id = osThreadNew((osThreadFunc_t)mqtt_recv_task, NULL, &recvOpt);
    if (g_mqtt_recv_task_id != NULL)
    {
        printf("ID = %d, Create mqtt_recv_task OK!\r\n", g_mqtt_recv_task_id);
    }

    // 7. 周期性读取光照并发布
    char msgBuf[64];
    while (1)
    {
        uint16_t adc_value = get_adc5_value();
        // 以纯数字字符串发布，便于客户端显示
        int len = snprintf(msgBuf, sizeof(msgBuf), "%u", adc_value);
        if (len < 0)
            len = 0;

        MQTTClient_pub(MQTT_TOPIC_PUB_LIGHT, msgBuf, (size_t)len);
        printf("[pub] %s => %s\r\n", MQTT_TOPIC_PUB_LIGHT, msgBuf);

        sleep(LIGHT_PUB_INTERVAL_S);
    }
}

// ========================= 任务创建 =========================
static void wifi_light_mqtt_task_create(void)
{
    osThreadAttr_t sendOpt;
    sendOpt.name = "mqtt_send_task";
    sendOpt.attr_bits = 0;
    sendOpt.cb_mem = NULL;
    sendOpt.cb_size = 0;
    sendOpt.stack_mem = NULL;
    sendOpt.stack_size = 1024 * 5;
    sendOpt.priority = osPriorityNormal;

    g_mqtt_send_task_id = osThreadNew((osThreadFunc_t)mqtt_send_task, NULL, &sendOpt);
    if (g_mqtt_send_task_id != NULL)
    {
        printf("ID = %d, mqtt_send_task Create OK!\n", g_mqtt_send_task_id);
    }
}

// ========================= 入口 =========================
static void template_demo(void)
{
    printf("普中-Hi3861开发板——WiFi通信实验（MQTT控制LED亮度，上报光照）\r\n");
    wifi_light_mqtt_task_create();
}

SYS_RUN(template_demo);
