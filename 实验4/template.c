/**
 ****************************************************************************************************
 * @file        template.c
 * @author      普中科技
 * @version     V1.0
 * @date        2024-06-05
 * @brief       跑酷游戏实验
 * @license     Copyright (c) 2024-2034, 深圳市普中科技有限公司
 ****************************************************************************************************
 * @attention
 *
 * 实验平台:普中-Hi3861
 *
 ****************************************************************************************************
 * 实验现象：
 * 1. OLED显示角色和障碍物
 * 2. 按键(KEY1)控制跳跃
 * 3. PS2摇杆(ADC0)控制左右移动
 * 4. 蜂鸣器播放音效
 ****************************************************************************************************
 */

#include <stdio.h>
#include <unistd.h>
#include <stdlib.h>

#include "ohos_init.h"
#include "cmsis_os2.h"

#include "bsp_led.h"
#include "bsp_beep.h"
// #include "bsp_key.h" // 移除bsp_key.h以避免GPIO12冲突
#include "bsp_oled.h"
#include "hi_adc.h"
#include "hi_io.h"
#include "hi_gpio.h"

// 游戏参数
#define GROUND_Y 50
#define DINO_W 10
#define DINO_H 10
#define OBS_W 8
#define OBS_H 10
#define GRAVITY 2
#define JUMP_FORCE -12
#define MOVE_SPEED 3
#define OBS_SPEED 4

// Pin Definitions
#define ADC0_PIN HI_IO_NAME_GPIO_12
#define KEY1_PIN HI_IO_NAME_GPIO_11
#define KEY1_GPIO_FUN HI_IO_FUNC_GPIO_11_GPIO

// 全局变量
int dino_x = 10;
int dino_y = GROUND_Y;
int dino_vy = 0;
int obs_x = 128;
int obs_y = GROUND_Y;
int score = 0;
int game_over = 0;

// ADC初始化 (PS2 X轴)
void ps2_adc_init(void)
{
    hi_gpio_init();
    // hi_io_set_pull(ADC0_PIN, HI_IO_PULL_UP); // 原有上拉
    hi_io_set_pull(ADC0_PIN, HI_IO_PULL_NONE); // 改为无上下拉，避免干扰模拟电压读取
}

// 按键初始化 (仅KEY1)
void key1_init(void)
{
    hi_gpio_init();
    // 实体按键通常为低电平有效，需要上拉
    hi_io_set_pull(KEY1_PIN, HI_IO_PULL_UP);
    hi_io_set_func(KEY1_PIN, KEY1_GPIO_FUN);
    hi_gpio_set_dir(KEY1_PIN, HI_GPIO_DIR_IN);
}

// 读取ADC值
uint16_t get_ps2_x_value(void)
{
    uint16_t data;
    hi_adc_read(HI_ADC_CHANNEL_0, &data, HI_ADC_EQU_MODEL_8, HI_ADC_CUR_BAIS_DEFAULT, 0xff);
    return data;
}

// 读取按键状态
uint8_t get_key1_state(void)
{
    hi_gpio_value val;
    hi_gpio_get_input_val(KEY1_PIN, &val);
    // 调试打印原始电平值，方便排查
    // printf("Key Raw: %d\r\n", val);
    return (val == HI_GPIO_VALUE0); // 改为低电平有效
}

// 简单的整数转字符串函数
void simple_itoa(int num, char *str)
{
    int i = 0;
    if (num == 0)
    {
        str[i++] = '0';
        str[i] = '\0';
        return;
    }
    int temp = num;
    int len = 0;
    while (temp > 0)
    {
        temp /= 10;
        len++;
    }
    for (int j = 0; j < len; j++)
    {
        str[len - 1 - j] = (num % 10) + '0';
        num /= 10;
    }
    str[len] = '\0';
}

// 游戏主任务
osThreadId_t Game_Task_ID;

void Game_Task(void)
{
    // 初始化外设
    led_init();
    beep_init();
    // key_init(); // 使用自定义初始化
    key1_init();
    oled_init();
    ps2_adc_init();

    oled_clear();
    oled_showstring(30, 20, "PARKOUR", 16);
    oled_refresh_gram();
    usleep(1000000);

    while (1)
    {
        if (game_over)
        {
            oled_clear();
            oled_showstring(30, 20, "GAME OVER", 16);
            char score_str[20] = "Score: ";
            simple_itoa(score, score_str + 7);
            oled_showstring(30, 40, (uint8_t *)score_str, 16);
            oled_refresh_gram();

            // 按键重启
            if (get_key1_state())
            {
                game_over = 0;
                score = 0;
                dino_x = 10;
                dino_y = GROUND_Y;
                dino_vy = 0;
                obs_x = 128;
                beep_alarm(100, 100); // 提示音
            }
            usleep(100000);
            continue;
        }

        // 1. 输入处理
        // 跳跃 (KEY1)
        if (dino_y == GROUND_Y) // 只有在地面才能跳
        {
            if (get_key1_state())
            {
                dino_vy = JUMP_FORCE;
                beep_alarm(50, 10); // 跳跃音效
            }
        }

        // 左右移动 (PS2 ADC0)
        uint16_t adc_val = get_ps2_x_value();

        // 调试输出
        printf("ADC: %d, Key: %d\r\n", adc_val, get_key1_state());

        // 假设ADC范围0-4096，中间约2000
        // 实际测试中，摇杆不动可能在2000左右
        // 向一个方向推可能变小，向另一个变大
        if (adc_val < 1000) // 左移 (阈值放宽)
        {
            dino_x -= MOVE_SPEED;
        }
        else if (adc_val > 3000) // 右移 (阈值放宽)
        {
            dino_x += MOVE_SPEED;
        }

        // 限制角色在屏幕内
        if (dino_x < 0)
            dino_x = 0;
        if (dino_x > 128 - DINO_W)
            dino_x = 128 - DINO_W;

        // 2. 物理更新
        dino_y += dino_vy;
        dino_vy += GRAVITY;

        if (dino_y > GROUND_Y)
        {
            dino_y = GROUND_Y;
            dino_vy = 0;
        }

        // 障碍物移动
        obs_x -= OBS_SPEED;
        if (obs_x < -OBS_W)
        {
            obs_x = 128;
            score++;
            printf("Score: %d\r\n", score); // 调试输出分数
            // 难度增加：每得5分速度加1
            // if (score % 5 == 0) OBS_SPEED++;
        }

        // 3. 碰撞检测
        // 简单的矩形碰撞
        if (dino_x + DINO_W > obs_x && dino_x < obs_x + OBS_W &&
            dino_y + DINO_H > obs_y && dino_y < obs_y + OBS_H)
        {
            game_over = 1;
            beep_alarm(500, 50); // 碰撞音效
        }

        // 4. 渲染
        oled_clear();

        // 画地面
        oled_draw_hline(0, GROUND_Y + DINO_H, 128, 1);

        // 画角色 (实心矩形)
        oled_fill_rectangle(dino_x, dino_y, DINO_W, DINO_H, 1);

        // 画障碍物 (实心矩形) - 安全绘制
        int draw_obs_x = obs_x;
        int draw_obs_w = OBS_W;
        if (draw_obs_x < 0)
        {
            draw_obs_w += draw_obs_x;
            draw_obs_x = 0;
        }
        if (draw_obs_w > 0 && draw_obs_x < 128)
        {
            oled_fill_rectangle(draw_obs_x, obs_y, draw_obs_w, OBS_H, 1);
        }

        // 显示分数
        char score_buf[10];
        simple_itoa(score, score_buf);
        oled_showstring(0, 0, (uint8_t *)score_buf, 12);

        oled_refresh_gram();

        // 帧率控制
        usleep(30 * 1000); // 30ms
    }
}

// 任务创建
void game_task_create(void)
{
    osThreadAttr_t taskOptions;
    taskOptions.name = "GameTask";
    taskOptions.attr_bits = 0;
    taskOptions.cb_mem = NULL;
    taskOptions.cb_size = 0;
    taskOptions.stack_mem = NULL;
    taskOptions.stack_size = 4096; // 增加栈大小以防万一
    taskOptions.priority = osPriorityNormal;

    Game_Task_ID = osThreadNew((osThreadFunc_t)Game_Task, NULL, &taskOptions);
    if (Game_Task_ID != NULL)
    {
        printf("Game Task Created!\n");
    }
}

static void template_demo(void)
{
    printf("Parkour Game Demo Start\r\n");
    game_task_create();
}
SYS_RUN(template_demo);
