/**
 ****************************************************************************************************
 * @file        template.c
 * @author      普中科技
 * @version     V1.0
 * @date        2024-06-05
 * @brief       GPIO与PWM蜂鸣器音乐实验
 * @license     Copyright (c) 2024-2034, 深圳市普中科技有限公司
 ****************************************************************************************************
 * @attention
 *
 * 实验现象：按KEY1播放音乐，KEY2暂停
 *
 ****************************************************************************************************
 */

#include <stdio.h>
#include <unistd.h>

#include "ohos_init.h"
#include "cmsis_os2.h"

#include "bsp_beep.h"
#include "bsp_key.h"
#include "bsp_led.h"
#include "hi_gpio.h"
#include "hi_io.h"
#include "hi_pwm.h"

// 播放状态
typedef enum
{
    STATE_STOPPED = 0,
    STATE_PLAYING,
    STATE_PAUSED
} PlayState;

// 音符定义
typedef struct
{
    int freq;
    int duration;
} Note;

// 小星星乐曲
Note music[] = {
    {262, 300}, // C4
    {262, 300}, // C4
    {392, 300}, // G4
    {392, 300}, // G4
    {440, 300}, // A4
    {440, 300}, // A4
    {392, 600}, // G4
    {349, 300}, // F4
    {349, 300}, // F4
    {330, 300}, // E4
    {330, 300}, // E4
    {294, 300}, // D4
    {294, 300}, // D4
    {262, 600}, // C4
};

#define MUSIC_LEN (sizeof(music) / sizeof(music[0]))

static volatile PlayState playState = STATE_STOPPED;
static volatile int currentNote = 0;
static volatile uint8_t lastKey1State = 0;
static volatile uint8_t lastKey2State = 0;

// 用PWM输出指定频率
void play_tone(int freq, int duration_ms)
{
    if (freq == 0)
    {
        printf("播放休止符 %d ms\r\n", duration_ms);
        hi_pwm_stop(HI_PWM_PORT_PWM2);
        usleep(duration_ms * 1000);
        return;
    }

    printf("播放频率: %d Hz, 时长: %d ms\r\n", freq, duration_ms);

    // 尝试新公式：period = 20000000 / freq
    long period = 20000000 / freq;
    long duty = period / 2; // 占空比50%

    // 限制在合理范围（16位无符号整数范围）
    if (period > 65535)
        period = 65535;
    if (period < 50)
        period = 50;
    if (duty > period)
        duty = period - 1;

    printf("计算得：周期=%ld, 占空比=%ld\r\n", period, duty);

    // 停止之前的PWM
    hi_pwm_stop(HI_PWM_PORT_PWM2);
    usleep(5 * 1000);

    // 启动PWM
    printf("启动PWM: port=PWM2, duty=%ld, period=%ld\r\n", duty, period);
    hi_pwm_start(HI_PWM_PORT_PWM2, duty, period);

    // 持续播放指定时长
    usleep(duration_ms * 1000);
}

// 播放音符的蜂鸣器任务
osThreadId_t MUSIC_Task_ID;

void MUSIC_Task(void)
{
    int i = 0;

    // 初始化PWM（只做一次）
    printf("\r\n初始化PWM...\r\n");
    hi_gpio_init();
    hi_io_set_func(HI_IO_NAME_GPIO_2, HI_IO_FUNC_GPIO_2_PWM2_OUT);
    hi_gpio_set_dir(HI_IO_NAME_GPIO_2, HI_GPIO_DIR_OUT);
    hi_pwm_init(HI_PWM_PORT_PWM2);
    printf("PWM初始化完成\r\n");

    while (1)
    {
        if (playState == STATE_PLAYING)
        {
            printf("\r\n========== 开始播放乐曲 ==========\r\n");
            printf("乐曲长度: %d 个音符\r\n", MUSIC_LEN);

            // 播放完整乐曲
            for (i = 0; i < MUSIC_LEN; ++i)
            {
                printf("\r\n[音符 %d/%d] ", i + 1, MUSIC_LEN);

                // 检查是否暂停
                while (playState == STATE_PAUSED)
                {
                    printf("(暂停中...)\r\n");
                    hi_pwm_stop(HI_PWM_PORT_PWM2); // 暂停时关闭PWM
                    usleep(100 * 1000);
                }

                // 检查是否停止
                if (playState == STATE_STOPPED)
                {
                    printf("\r\n(停止播放)\r\n");
                    hi_pwm_stop(HI_PWM_PORT_PWM2);
                    break;
                }

                // 播放当前音符
                play_tone(music[i].freq, music[i].duration);
                usleep(50 * 1000); // 音符间隔
            }

            printf("\r\n========== 乐曲播放完毕 ==========\r\n");
            hi_pwm_stop(HI_PWM_PORT_PWM2); // 乐曲结束关闭PWM
            playState = STATE_STOPPED;
        }
        else
        {
            usleep(100 * 1000);
        }
    }
}

void music_task_create(void)
{
    osThreadAttr_t taskOptions;
    taskOptions.name = "musicTask";
    taskOptions.attr_bits = 0;
    taskOptions.cb_mem = NULL;
    taskOptions.cb_size = 0;
    taskOptions.stack_mem = NULL;
    taskOptions.stack_size = 1024;
    taskOptions.priority = osPriorityNormal;

    MUSIC_Task_ID = osThreadNew((osThreadFunc_t)MUSIC_Task, NULL, &taskOptions);
    if (MUSIC_Task_ID != NULL)
    {
        printf("Music Task Create OK!\r\n");
    }
}

// 按键控制任务
osThreadId_t KEY_Task_ID;

void KEY_Task(void)
{
    uint8_t key = 0;

    key_init(); // 按键初始化

    while (1)
    {
        key = key_scan(0); // 按键扫描

        // 检测KEY1按下（上升沿触发）
        if (key == KEY1_PRESS && lastKey1State == 0)
        {
            lastKey1State = 1;
            printf("KEY1 按下 - 播放音乐\r\n");
            if (playState == STATE_STOPPED)
            {
                playState = STATE_PLAYING;
            }
            else if (playState == STATE_PAUSED)
            {
                playState = STATE_PLAYING;
            }
            usleep(200 * 1000); // 防抖延迟
        }
        else if (key != KEY1_PRESS)
        {
            lastKey1State = 0;
        }

        // 检测KEY2按下（上升沿触发）
        if (key == KEY2_PRESS && lastKey2State == 0)
        {
            lastKey2State = 1;
            printf("KEY2 按下 - 暂停音乐\r\n");
            if (playState == STATE_PLAYING)
            {
                playState = STATE_PAUSED;
            }
            usleep(200 * 1000); // 防抖延迟
        }
        else if (key != KEY2_PRESS)
        {
            lastKey2State = 0;
        }

        usleep(50 * 1000);
    }
}

void key_task_create(void)
{
    osThreadAttr_t taskOptions;
    taskOptions.name = "keyTask";
    taskOptions.attr_bits = 0;
    taskOptions.cb_mem = NULL;
    taskOptions.cb_size = 0;
    taskOptions.stack_mem = NULL;
    taskOptions.stack_size = 1024;
    taskOptions.priority = osPriorityNormal;

    KEY_Task_ID = osThreadNew((osThreadFunc_t)KEY_Task, NULL, &taskOptions);
    if (KEY_Task_ID != NULL)
    {
        printf("Key Task Create OK!\r\n");
    }
}

/**
 * @description: 初始化并创建任务
 * @param {*}
 * @return {*}
 */
static void template_demo(void)
{
    printf("普中-Hi3861开发板--GPIO与PWM蜂鸣器音乐实验\r\n");

    beep_init(); // 蜂鸣器初始化
    led_init();  // LED初始化

    key_task_create();   // 按键任务
    music_task_create(); // 音乐播放任务

    printf("按KEY1播放/继续，KEY2暂停\r\n");
}
SYS_RUN(template_demo);
