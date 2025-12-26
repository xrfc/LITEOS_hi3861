/**
 ****************************************************************************************************
 * @file        rock_paper_scissors.c
 * @brief       LiteOS剪刀石头布游戏（修复事件阻塞问题，全英文输出）
 ****************************************************************************************************
 */

#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <time.h>

#include "ohos_init.h"
#include "cmsis_os2.h"

// 游戏动作枚举
typedef enum {
    ROCK,       // 石头
    PAPER,      // 布
    SCISSORS    // 剪刀
} GameAction;

// 玩家消息结构体
typedef struct {
    uint8_t player_id;   // 1=PlayerA, 2=PlayerB
    GameAction action;   // 选择的动作
} PlayerMsg;

// 全局ID声明
osThreadId_t Referee_ID;
osThreadId_t PlayerA_ID;
osThreadId_t PlayerB_ID;
osEventFlagsId_t gameEvent_ID;
osMessageQueueId_t msgQueue_ID;

// 事件标志
#define GAME_START_EVENT 0x00000001U

// 常量配置
#define TASK_STACK_SIZE 1024
#define MSG_QUEUE_SIZE 8
#define ROUND_DELAY 3
#define MAX_ROUNDS 5

// 函数声明
void RefereeTask(void);
void PlayerATask(void);
void PlayerBTask(void);
const char* ActionToString(GameAction action);
void JudgeResult(GameAction a, GameAction b);

/**
 * @brief 动作转字符串
 */
const char* ActionToString(GameAction action) {
    switch(action) {
        case ROCK: return "Rock";
        case PAPER: return "Paper";
        case SCISSORS: return "Scissors";
        default: return "Unknown";
    }
}

/**
 * @brief 裁判判断胜负
 */
void JudgeResult(GameAction a, GameAction b) {
    printf("Player A chose %s, Player B chose %s: ", ActionToString(a), ActionToString(b));
    if (a == b) {
        printf("Draw!\n");
    } else if ((a == ROCK && b == SCISSORS) || 
               (a == PAPER && b == ROCK) || 
               (a == SCISSORS && b == PAPER)) {
        printf("Player A Wins!\n");
    } else {
        printf("Player B Wins!\n");
    }
}

/**
 * @brief 裁判任务（核心修复：每轮结束清除事件）
 */
void RefereeTask(void) {
    osStatus_t status;
    PlayerMsg msgA, msgB;
    uint32_t round = 0;
    
    printf("Referee Task Started\n");
    
    while (round < MAX_ROUNDS) {
        round++;
        printf("\n===== Round %d Start =====\n", round);
        
        // 发送游戏开始事件（所有玩家都能收到）
        osEventFlagsSet(gameEvent_ID, GAME_START_EVENT);
        printf("Referee: Game Start! Players please choose your action...\n");
        
        // 等待接收玩家A的消息
        status = osMessageQueueGet(msgQueue_ID, &msgA, NULL, osWaitForever);
        if (status == osOK && msgA.player_id == 1) {
            printf("Referee received Player A's choice: %s\n", ActionToString(msgA.action));
        }
        
        // 等待接收玩家B的消息
        status = osMessageQueueGet(msgQueue_ID, &msgB, NULL, osWaitForever);
        if (status == osOK && msgB.player_id == 2) {
            printf("Referee received Player B's choice: %s\n", ActionToString(msgB.action));
        }
        
        // 判断结果
        JudgeResult(msgA.action, msgB.action);
        
        // 关键修复：手动清除事件标志，避免下一轮残留
        osEventFlagsClear(gameEvent_ID, GAME_START_EVENT);
        
        // 延迟进入下一轮
        printf("Waiting %d seconds for next round...\n", ROUND_DELAY);
        sleep(ROUND_DELAY);
    }
    
    printf("\n===== Game Over =====\n");
}

/**
 * @brief 玩家A任务（核心修复：事件等待添加osFlagsNoClear）
 */
void PlayerATask(void) {
    osStatus_t status;
    PlayerMsg msg;
    msg.player_id = 1;
    // 嵌入式随机数优化：基于系统tick生成种子
    unsigned int seed = osKernelGetTickCount();
    srand(seed);
    
    printf("Player A Task Started\n");
    
    while (1) {
        // 关键修复：添加osFlagsNoClear，事件不会被自动清除
        osEventFlagsWait(gameEvent_ID, GAME_START_EVENT, osFlagsWaitAll | osFlagsNoClear, osWaitForever);
        
        // 随机选动作
        msg.action = rand() % 3;
        
        // 发送消息给裁判
        status = osMessageQueuePut(msgQueue_ID, &msg, 0, osWaitForever);
        if (status != osOK) {
            printf("Player A: Failed to send action!\n");
        }
    }
}

/**
 * @brief 玩家B任务（同玩家A，修复事件等待参数）
 */
void PlayerBTask(void) {
    osStatus_t status;
    PlayerMsg msg;
    msg.player_id = 2;
    // 不同的随机数种子
    unsigned int seed = osKernelGetTickCount() + 100;
    srand(seed);
    
    printf("Player B Task Started\n");
    
    while (1) {
        // 关键修复：osFlagsNoClear
        osEventFlagsWait(gameEvent_ID, GAME_START_EVENT, osFlagsWaitAll | osFlagsNoClear, osWaitForever);
        
        // 随机选动作
        msg.action = rand() % 3;
        
        // 发送消息给裁判
        status = osMessageQueuePut(msgQueue_ID, &msg, 0, osWaitForever);
        if (status != osOK) {
            printf("Player B: Failed to send action!\n");
        }
    }
}

/**
 * @brief 游戏初始化
 */
static void GameDemoInit(void) {
    printf("LiteOS Rock-Paper-Scissors Game Start!\n");
    
    // 创建事件标志
    gameEvent_ID = osEventFlagsNew(NULL);
    if (gameEvent_ID == NULL) {
        printf("Failed to create game event!\n");
        return;
    }
    
    // 创建消息队列
    msgQueue_ID = osMessageQueueNew(MSG_QUEUE_SIZE, sizeof(PlayerMsg), NULL);
    if (msgQueue_ID == NULL) {
        printf("Failed to create message queue!\n");
        return;
    }
    
    // 任务属性配置
    osThreadAttr_t taskOptions;
    taskOptions.attr_bits = 0;
    taskOptions.cb_mem = NULL;
    taskOptions.cb_size = 0;
    taskOptions.stack_mem = NULL;
    taskOptions.stack_size = TASK_STACK_SIZE;
    taskOptions.priority = osPriorityNormal;
    
    // 创建裁判任务
    taskOptions.name = "RefereeTask";
    Referee_ID = osThreadNew((osThreadFunc_t)RefereeTask, NULL, &taskOptions);
    if (Referee_ID == NULL) {
        printf("Failed to create Referee Task!\n");
    }
    
    // 创建玩家A任务
    taskOptions.name = "PlayerATask";
    PlayerA_ID = osThreadNew((osThreadFunc_t)PlayerATask, NULL, &taskOptions);
    if (PlayerA_ID == NULL) {
        printf("Failed to create Player A Task!\n");
    }
    
    // 创建玩家B任务
    taskOptions.name = "PlayerBTask";
    PlayerB_ID = osThreadNew((osThreadFunc_t)PlayerBTask, NULL, &taskOptions);
    if (PlayerB_ID == NULL) {
        printf("Failed to create Player B Task!\n");
    }
}

SYS_RUN(GameDemoInit);