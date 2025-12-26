#include <stdio.h>
#include <unistd.h>
#include <string.h>
#include <stdlib.h>

#include "ohos_init.h"
#include "cmsis_os2.h"

// 鸿蒙硬件驱动
#include "hi_gpio.h"
#include "hi_io.h"
#include "hi_pwm.h"
#include "hi_wifi_api.h"

// BSP 头文件
#include "bsp_led.h"
#include "bsp_key.h"
#include "bsp_sr04.h"
#include "bsp_sg90.h"
#include "bsp_oled.h"
#include "bsp_beep.h"
#include "bsp_wifi.h"
#include "bsp_mqtt.h"

// 网络协议栈
#include "lwip/sockets.h"
#include "lwip/netifapi.h"

/* ============================================================
 * 用户配置区域
 * ============================================================ */
// 1. WiFi 配置
#define WIFI_SSID "manbo"
#define WIFI_PAWD "skjls987"

// 2. MQTT 服务器配置 (EMQX Public Broker)
// 注意：HTML端使用的是 broker.emqx.io，IP地址可能会变动
// 建议保持 HTML 端和此处连接同一个 Broker
#define SERVER_IP_ADDR "44.232.241.40"
#define SERVER_IP_PORT 1883

// 3. MQTT 主题定义
#define MQTT_TOPIC_CONTROL "hi3861/radar/control" // 订阅
#define MQTT_TOPIC_DATA "hi3861/radar/data"       // 发布

// 4. 雷达参数配置
#define SCAN_START_ANGLE 0
#define SCAN_END_ANGLE 180
#define SCAN_STEP_ANGLE 5
#define WARNING_DISTANCE_CM 30
#define ALARM_DISTANCE_CM 10

/* ============================================================
 * 数据结构定义
 * ============================================================ */
typedef enum
{
    SYSTEM_SCANNING = 0, // 扫描中
    SYSTEM_STOPPED,      // 停止
    SYSTEM_ALARM         // 告警
} SystemState_t;

typedef enum
{
    ALARM_SAFE = 0, // 安全状态
    ALARM_WARNING,  // 警告状态
    ALARM_DANGER    // 危险状态
} AlarmState_t;

typedef struct
{
    float distance;
    uint16_t angle;
    AlarmState_t alarmState;
    SystemState_t sysState;
} RadarData_t;

/* ============================================================
 * 全局变量
 * ============================================================ */
static osThreadId_t g_scanTaskHandle = NULL;
static osThreadId_t g_displayTaskHandle = NULL;
static osThreadId_t g_mqttTaskHandle = NULL; // 负责 MQTT 通信
static osMessageQueueId_t g_dataQueue = NULL;
static osMutexId_t g_systemMutex = NULL;

// 系统状态变量
static SystemState_t g_systemState = SYSTEM_SCANNING;
static AlarmState_t g_alarmState = ALARM_SAFE;
static uint16_t g_currentAngle = 90;
static float g_currentDistance = 0;
static uint8_t g_scanEnabled = 1;
static uint8_t g_distanceUpdateCounter = 0;

/* ============================================================
 * 基础功能函数
 * ============================================================ */

// 本地蜂鸣器定义 (避免修改 common 库文件)
#define MY_BEEP_PIN HI_IO_NAME_GPIO_7
#define MY_BEEP_GPIO_FUN HI_IO_FUNC_GPIO_7_GPIO

// 重定义 BEEP 宏
#undef BEEP
#define BEEP(a) hi_gpio_set_ouput_val(MY_BEEP_PIN, a)

static void Local_Beep_Init(void)
{
    hi_gpio_init();
    hi_io_set_pull(MY_BEEP_PIN, HI_IO_PULL_UP);
    hi_io_set_func(MY_BEEP_PIN, MY_BEEP_GPIO_FUN);
    hi_gpio_set_dir(MY_BEEP_PIN, HI_GPIO_DIR_OUT);
}

/* 系统初始化 */
static void System_Init(void)
{
    led_init();
    key_init();
    sr04_init();
    sg90_init();
    oled_init();
    Local_Beep_Init(); // 使用本地初始化，GPIO 7

    // 创建互斥锁
    osMutexAttr_t mutex_attr = {0};
    g_systemMutex = osMutexNew(&mutex_attr);

    // 创建消息队列
    g_dataQueue = osMessageQueueNew(1, sizeof(RadarData_t), NULL);

    printf("超声波雷达系统初始化完成\n");
}

/* 告警控制 */
static void Alarm_Control(AlarmState_t state)
{
    static uint32_t lastBlinkTime = 0;
    static uint8_t ledState = 0;

    switch (state)
    {
    case ALARM_SAFE:
        LED(0);
        BEEP(0);
        ledState = 0;
        break;
    case ALARM_WARNING:
        BEEP(0);
        {
            uint32_t currentTime = osKernelGetTickCount();
            if (currentTime - lastBlinkTime >= 50)
            {
                ledState = !ledState;
                LED(ledState);
                lastBlinkTime = currentTime;
            }
        }
        break;
    case ALARM_DANGER:
        LED(1);
        BEEP(1);
        ledState = 1;
        break;
    }
}

/* 告警状态判定 */
static AlarmState_t Get_AlarmState(float distance)
{
    if (distance <= 0 || distance > 400)
        return ALARM_SAFE;
    if (distance <= ALARM_DISTANCE_CM)
        return ALARM_DANGER;
    else if (distance <= WARNING_DISTANCE_CM)
        return ALARM_WARNING;
    else
        return ALARM_SAFE;
}

/* 查表法 Sin 函数 */
static float GetSin(int angle)
{
    const float sin_val[] = {
        0.0000f, 0.0872f, 0.1736f, 0.2588f, 0.3420f, 0.4226f, 0.5000f, 0.5736f, 0.6428f, 0.7071f,
        0.7660f, 0.8192f, 0.8660f, 0.9063f, 0.9397f, 0.9659f, 0.9848f, 0.9962f, 1.0000f};
    if (angle < 0)
        angle = 0;
    if (angle > 180)
        angle = 180;
    int index = (angle + 2) / 5;
    if (index > 18)
        index = 18;
    if (angle <= 90)
        return sin_val[index];
    else
    {
        int mirror_angle = 180 - angle;
        index = (mirror_angle + 2) / 5;
        return sin_val[index];
    }
}

/* 查表法 Cos 函数 */
static float GetCos(int angle)
{
    if (angle < 0)
        angle = 0;
    if (angle > 180)
        angle = 180;
    if (angle <= 90)
        return GetSin(90 - angle);
    else
        return -GetSin(angle - 90);
}

/* ============================================================
 * 任务函数定义
 * ============================================================ */

/* 按键扫描任务 */
static void Key_ScanTask(void *arg)
{
    (void)arg;
    static uint32_t lastPressTime = 0;
    while (1)
    {
        uint8_t keyValue = key_scan(0);
        if (keyValue != 0)
        {
            uint32_t currentTime = osKernelGetTickCount();
            if (currentTime - lastPressTime > 300)
            {
                osMutexAcquire(g_systemMutex, osWaitForever);

                if (keyValue == KEY1_PRESS)
                {
                    // Key 1: 启动扫描
                    if (!g_scanEnabled)
                    {
                        g_scanEnabled = 1;
                        g_systemState = SYSTEM_SCANNING;
                        printf("Key1: Start Scan\n");
                    }
                }
                else if (keyValue == KEY2_PRESS)
                {
                    // Key 2: 停止扫描
                    if (g_scanEnabled)
                    {
                        g_scanEnabled = 0;
                        g_systemState = SYSTEM_STOPPED;
                        set_sg90_angle(90); // 复位到中间
                        printf("Key2: Stop Scan\n");
                    }
                }

                osMutexRelease(g_systemMutex);
                lastPressTime = currentTime;
            }
        }
        usleep(10 * 1000);
    }
}

/* 雷达扫描任务 */
static void Radar_ScanTask(void *arg)
{
    (void)arg;
    uint16_t currentAngle = 90;
    int8_t direction = 1;

    // 初始设置舵机角度
    set_sg90_angle(currentAngle);
    usleep(200 * 1000);

    while (1)
    {
        // 检查扫描是否启用
        if (!g_scanEnabled)
        {
            usleep(50 * 1000);
            continue;
        }

        // 1. 舵机动作
        set_sg90_angle(currentAngle);
        usleep(20 * 1000);

        // 2. 预读取传感器
        float rawDist = -1.0f;
        if (g_distanceUpdateCounter >= 9)
        {
            rawDist = sr04_read_distance();
        }

        // 获取互斥锁
        osMutexAcquire(g_systemMutex, osWaitForever);

        if (!g_scanEnabled)
        {
            osMutexRelease(g_systemMutex);
            usleep(10 * 1000);
            continue;
        }

        g_currentAngle = currentAngle;
        g_distanceUpdateCounter++;

        if (g_distanceUpdateCounter >= 10)
        {
            // 数据滤波逻辑
            if (rawDist >= 0)
            {
                float validDist = rawDist;
                // 限幅 + 突变抑制
                if (rawDist <= 0 || rawDist >= 400)
                {
                    validDist = g_currentDistance;
                }
                else if (g_currentDistance > 0 &&
                         (rawDist > g_currentDistance + 50 || rawDist < g_currentDistance - 50))
                {
                    validDist = g_currentDistance;
                }
                // 滑动平均
                if (g_currentDistance == 0)
                    g_currentDistance = validDist;
                else
                    g_currentDistance = g_currentDistance * 0.7f + validDist * 0.3f;
            }

            g_distanceUpdateCounter = 0;
            g_alarmState = Get_AlarmState(g_currentDistance);
            Alarm_Control(g_alarmState);

            // 只有在这里显式确认状态
            if (g_alarmState == ALARM_DANGER)
                g_systemState = SYSTEM_ALARM;
            else
                g_systemState = SYSTEM_SCANNING;

            // 发送数据到队列
            if (g_dataQueue != NULL)
            {
                RadarData_t sendData;
                sendData.distance = g_currentDistance;
                sendData.angle = currentAngle;
                sendData.alarmState = g_alarmState;
                sendData.sysState = g_systemState;
                osMessageQueuePut(g_dataQueue, &sendData, 0, 0);
            }
        }
        osMutexRelease(g_systemMutex);

        // 角度步进
        if (direction > 0)
        {
            currentAngle += SCAN_STEP_ANGLE;
            if (currentAngle >= SCAN_END_ANGLE)
            {
                direction = -1;
                currentAngle = SCAN_END_ANGLE;
            }
        }
        else
        {
            currentAngle -= SCAN_STEP_ANGLE;
            if (currentAngle <= SCAN_START_ANGLE)
            {
                direction = 1;
                currentAngle = SCAN_START_ANGLE;
            }
        }

        usleep(10 * 1000);
    }
}

/* OLED 显示任务 */
static void OLED_DisplayTask(void *arg)
{
    (void)arg;
    RadarData_t recvData = {0};
    static char displayBuffer[32];
    static uint8_t radar_history[37] = {0};

    printf("OLED显示任务启动\n");
    oled_clear();
    oled_refresh_gram();

    while (1)
    {
        // 尝试获取数据
        int hasNewData = 0;
        RadarData_t tempData;
        while (osMessageQueueGet(g_dataQueue, &tempData, NULL, 0) == osOK)
        {
            recvData = tempData;
            hasNewData = 1;
        }
        if (!hasNewData)
        {
            if (osMessageQueueGet(g_dataQueue, &recvData, NULL, 100) == osOK)
            {
                hasNewData = 1;
            }
        }

        if (hasNewData)
        {
            // 更新历史数组
            int angle_idx = recvData.angle / 5;
            if (angle_idx >= 0 && angle_idx <= 36)
            {
                radar_history[angle_idx] = (recvData.distance > 0 && recvData.distance < 100) ? (uint8_t)recvData.distance : 0;
            }

            oled_fill(0, 16, 127, 63, 0);

            // 显示状态
            uint8_t *statusText;
            if (recvData.alarmState == ALARM_DANGER)
                statusText = (uint8_t *)"ALARM";
            else if (recvData.alarmState == ALARM_WARNING)
                statusText = (uint8_t *)"WARN ";
            else
                statusText = (recvData.sysState == SYSTEM_SCANNING) ? (uint8_t *)"SCAN " : (uint8_t *)"STOP ";
            oled_showstring(0, 0, statusText, 16);

            // 显示数值
            snprintf(displayBuffer, sizeof(displayBuffer), "%-3d^%-3.0fcm", recvData.angle, recvData.distance);
            oled_showstring(42, 0, (uint8_t *)displayBuffer, 16);

            // 绘制扫描线
            float cos_val = GetCos(recvData.angle);
            float sin_val = GetSin(recvData.angle);
            int x_end = 64 + (int)(45 * cos_val);
            int y_end = 63 - (int)(45 * sin_val);
            if (x_end < 0)
                x_end = 0;
            if (x_end > 127)
                x_end = 127;
            if (y_end < 16)
                y_end = 16;
            if (y_end > 63)
                y_end = 63;
            oled_drawline(64, 63, x_end, y_end, 1);

            // 绘制历史点 (轨迹)
            for (int i = 0; i <= 36; i++)
            {
                if (radar_history[i] > 0)
                {
                    int h_angle = i * 5;
                    float h_cos = GetCos(h_angle);
                    float h_sin = GetSin(h_angle);
                    int r_obj = (int)((radar_history[i] / 100.0f) * 45);
                    int x_obj = 64 + (int)(r_obj * h_cos);
                    int y_obj = 63 - (int)(r_obj * h_sin);
                    oled_draw_bigpoint(x_obj, y_obj, 1);
                }
            }
            oled_refresh_gram();
        }
        else
        {
            // 超时刷新显示
            static uint32_t lastUpdate = 0;
            uint32_t currentTime = osKernelGetTickCount();
            if (currentTime - lastUpdate > 1000)
            {
                oled_showstring(0, 0, g_scanEnabled ? (uint8_t *)"SCAN " : (uint8_t *)"STOP ", 16);
                oled_refresh_gram();
                lastUpdate = currentTime;
            }
        }
        usleep(10 * 1000);
    }
}

/* ============================================================
 * 网络通信任务 (仅负责 MQTT，不再有 WebServer)
 * ============================================================ */

/* MQTT 订阅回调 (接收远程指令) */
static int8_t MQTT_SubCallback(unsigned char *topic, unsigned char *payload)
{
    printf("[MQTT Recv] Topic:%s Payload:%s\n", topic, payload);
    if (strstr((char *)topic, "control"))
    {
        osMutexAcquire(g_systemMutex, 100);
        if (strstr((char *)payload, "STOP"))
        {
            g_scanEnabled = 0;
            g_systemState = SYSTEM_STOPPED;
            set_sg90_angle(90);
        }
        else if (strstr((char *)payload, "START"))
        {
            g_scanEnabled = 1;
            g_systemState = SYSTEM_SCANNING;
        }
        osMutexRelease(g_systemMutex);
    }
    return 0;
}

/* MQTT 接收线程 */
static void MQTT_RecvLoopTask(void)
{
    while (1)
    {
        MQTTClient_sub();
        usleep(200 * 1000);
    }
}

/* 核心网络任务: 负责连接WiFi并建立MQTT连接 */
static void WiFi_MQTT_Task(void *arg)
{
    (void)arg;
    char payload[128];

    printf("WiFi/MQTT Task Started...\n");

    // 1. 连接WiFi
    printf("Connecting to WiFi: %s...\n", WIFI_SSID);
    WifiErrorCode wifi_res = WiFi_connectHotspots(WIFI_SSID, WIFI_PAWD);
    if (wifi_res != WIFI_SUCCESS)
    {
        printf("WiFi Connect Error: %d\n", wifi_res);
    }

    // 等待获取IP (静默等待，不打印IP)
    int wait_count = 0;
    int is_connected = 0;
    while (wait_count < 20)
    {
        char *ip = WiFi_GetLocalIP();
        if (ip && strlen(ip) > 0 && strcmp(ip, "EC800M_4G") != 0 && strcmp(ip, "0.0.0.0") != 0)
        {
            printf("WiFi Connected Successfully.\n");
            is_connected = 1;
            break;
        }
        sleep(1);
        wait_count++;
    }

    if (!is_connected)
    {
        printf("WiFi connection timeout!\n");
        // 显示连接失败信息到 OLED (可选)
        osMutexAcquire(g_systemMutex, 100);
        oled_clear();
        oled_showstring(0, 0, (uint8_t *)"WiFi Failed", 16);
        oled_refresh_gram();
        osMutexRelease(g_systemMutex);

        // 即使没有网络，也保持线程运行，防止系统Crash
        while (1)
        {
            sleep(10);
        }
    }

    // 2. 连接 MQTT 服务器
    printf("Connecting to MQTT Server...\n");
    int retry = 0;
    while (retry < 5)
    {
        if (MQTTClient_connectServer(SERVER_IP_ADDR, SERVER_IP_PORT) == 0)
        {
            printf("MQTT Server Connected!\n");
            break;
        }
        printf("MQTT Connect Failed, Retrying...\n");
        sleep(2);
        retry++;
    }

    if (retry < 5)
    {
        // 初始化 MQTT
        if (MQTTClient_init("hi3861_radar_pro", "user", "pass") == 0)
        {
            // 订阅主题
            p_MQTTClient_sub_callback = &MQTT_SubCallback;
            MQTTClient_subscribe(MQTT_TOPIC_CONTROL);

            // 启动接收子线程
            osThreadAttr_t attr = {0};
            attr.name = "MQTT_RecvLoop";
            attr.stack_size = 4096;
            attr.priority = osPriorityNormal;
            osThreadNew((osThreadFunc_t)MQTT_RecvLoopTask, NULL, &attr);

            // 数据上报循环
            while (1)
            {
                if (g_scanEnabled)
                {
                    snprintf(payload, sizeof(payload),
                             "{\"angle\":%d,\"dist\":%.1f,\"state\":%d}",
                             g_currentAngle, g_currentDistance, g_systemState);
                    MQTTClient_pub(MQTT_TOPIC_DATA, (unsigned char *)payload, strlen(payload));
                }
                sleep(1); // 1秒上报一次，防止拥塞
            }
        }
    }
    else
    {
        printf("MQTT Failed to Connect.\n");
    }

    while (1)
        sleep(10);
}

/* ============================================================
 * 主函数入口
 * ============================================================ */
static void UltrasonicRadarApp(void)
{
    printf("\n=== Hi3861 Smart Radar System Starting (Clean Mode) ===\n");

    // 1. 初始化硬件
    System_Init();

    // 2. 启动网络任务 (WiFi + MQTT)
    osThreadAttr_t mqtt_attr = {
        .name = "WiFi_MQTT_Task",
        .stack_size = 8192,
        .priority = osPriorityAboveNormal};
    g_mqttTaskHandle = osThreadNew(WiFi_MQTT_Task, NULL, &mqtt_attr);

    // 给网络任务一点时间
    usleep(100 * 1000);

    // 3. 启动应用逻辑任务

    // 按键任务
    osThreadAttr_t key_attr = {
        .name = "KeyScanTask",
        .stack_size = 1024,
        .priority = osPriorityNormal};
    osThreadNew(Key_ScanTask, NULL, &key_attr);

    // 雷达扫描任务
    osThreadAttr_t scan_attr = {
        .name = "RadarScanTask",
        .stack_size = 5120,
        .priority = osPriorityNormal};
    g_scanTaskHandle = osThreadNew(Radar_ScanTask, NULL, &scan_attr);

    // OLED 显示任务
    osThreadAttr_t display_attr = {
        .name = "OLEDDisplayTask",
        .stack_size = 8192,
        .priority = osPriorityNormal};
    g_displayTaskHandle = osThreadNew(OLED_DisplayTask, NULL, &display_attr);

    printf("=== System Running ===\n");
}

SYS_RUN(UltrasonicRadarApp);
