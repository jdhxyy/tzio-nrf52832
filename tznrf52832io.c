// Copyright 2020-2020 The TZIOT Authors. All rights reserved.
// nrf52832的io驱动
// Authors: jdh99 <jdh821@163.com>

#include "tzio.h"
#include "nrf52.h"
#include "nrf52_bitfields.h"

#define PIN_VALUE_MAX 31

// 最大支持8个通道的中断
#define IRQ_CALLBACK_NUM_MAX 8

typedef struct {
    int pin;
    TZEmptyFunc callback;
} IrqCallback;

// 上拉模式
typedef enum {
    GPIO_NOPULL = 0,
    GPIO_PULLDOWN = 1,
    GPIO_PULLUP = 3
} GPIO_Pull_Mode;

// 输入触发模式
typedef enum {
    GPIOTE_NONE = 0,
    GPIOTE_LO_TO_HI = 1,
    GPIOTE_HI_TO_LO = 2,
    GPIOTE_TOGGLE = 3
} GPIOTE_POLARITY;

static IrqCallback gIrqCallback[IRQ_CALLBACK_NUM_MAX] = {0};
static int gIrqCallbackNum = 0;

static bool isPinUsed(int pin);
static void initGpiote(void);

// TZIOConfigOutput 设置为输出
void TZIOConfigOutput(int pin, TZIOPullMode pullMode, TZIOOutMode outMode) {
    if (pin < 0 || pin > PIN_VALUE_MAX) {
        return;
    }

    uint32_t pullModeGet;
    switch(pullMode) {
    case TZIO_NOPULL: pullModeGet = GPIO_PIN_CNF_PULL_Disabled; break;
    case TZIO_PULLDOWN: pullModeGet = GPIO_PIN_CNF_PULL_Pulldown; break;
    case TZIO_PULLUP: pullModeGet = GPIO_PIN_CNF_PULL_Pullup; break;
    }

    uint32_t outModeGet = (outMode == TZIO_OUT_PP) ? GPIO_PIN_CNF_DRIVE_S0S1 : GPIO_PIN_CNF_DRIVE_S0D1;

    NRF_P0->PIN_CNF[pin] = (GPIO_PIN_CNF_DIR_Output << GPIO_PIN_CNF_DIR_Pos) | 
        (GPIO_PIN_CNF_INPUT_Disconnect << GPIO_PIN_CNF_INPUT_Pos) | 
        (pullModeGet << GPIO_PIN_CNF_PULL_Pos) | 
        (outModeGet << GPIO_PIN_CNF_DRIVE_Pos) | 
        (GPIO_PIN_CNF_SENSE_Disabled << GPIO_PIN_CNF_SENSE_Pos);
}

// TZIOConfigInput 设置为输入
void TZIOConfigInput(int pin, TZIOPullMode pullMode) {
    if (pin < 0 || pin > PIN_VALUE_MAX) {
        return;
    }

    GPIO_Pull_Mode pullModeGet;
    switch(pullMode) {
    case TZIO_NOPULL: pullModeGet = GPIO_NOPULL; break;
    case TZIO_PULLDOWN: pullModeGet = GPIO_PULLDOWN; break;
    case TZIO_PULLUP: pullModeGet = GPIO_PULLUP; break;
    }

    NRF_P0->PIN_CNF[pin] = (GPIO_PIN_CNF_DIR_Input << GPIO_PIN_CNF_DIR_Pos) | 
        (GPIO_PIN_CNF_INPUT_Connect << GPIO_PIN_CNF_INPUT_Pos) | 
        (pullModeGet << GPIO_PIN_CNF_PULL_Pos) | 
        (GPIO_PIN_CNF_DRIVE_S0S1 << GPIO_PIN_CNF_DRIVE_Pos) | 
        (GPIO_PIN_CNF_SENSE_Disabled << GPIO_PIN_CNF_SENSE_Pos);
}

// TZIOSetHigh 输出高电平
void TZIOSetHigh(int pin) {
    NRF_P0->OUTSET = 1 << pin;
}

// TZIOSetLow 输出低电平
void TZIOSetLow(int pin) {
    NRF_P0->OUTCLR = 1 << pin;
}

// TZIOSet 输出电平
void TZIOSet(int pin, bool level) {
    if (level) {
        NRF_P0->OUTSET = 1 << pin;
    } else {
        NRF_P0->OUTCLR = 1 << pin;
    }
}

// TZIOToggle 输出跳变信号
void TZIOToggle(int pin) {
    //将寄存器值取出进行操作(不能直接对寄存器进行或/与/非等直接操作, NRF52832的寄存器只能被简单赋值)
    uint32_t pins_state = NRF_P0->OUT;

    NRF_P0->OUTSET = (~pins_state & (1UL << pin));
    NRF_P0->OUTCLR = (pins_state & (1UL << pin));
}

// TZIOReadInputPin 读取输入引脚电平
bool TZIOReadInputPin(int pin) {
    return ((NRF_P0->IN >> pin) & 0x1);
}

// TZIOReadOutputPin 读取输出引脚电平
bool TZIOReadOutputPin(int pin) {
    return ((NRF_P0->OUT >> pin) & 0x1);
}

// TZIOConfigIrq 配置中断模式
// 本函数会配置io为输入,不用提前配置.且配置完成后已经使能中断
void TZIOConfigIrq(int pin, TZIOIrqPolarity polarity, TZEmptyFunc callback) {
    if (gIrqCallbackNum >= IRQ_CALLBACK_NUM_MAX) {
        return;
    }
    if (isPinUsed(pin)) {
        return;
    }

    if (gIrqCallbackNum == 0) {
        initGpiote();
    }

    GPIOTE_POLARITY polarityGet;
    switch(polarity) {
    case TZIO_LO_TO_HI: polarityGet = GPIOTE_LO_TO_HI; break;
    case TZIO_HI_TO_LO: polarityGet = GPIOTE_HI_TO_LO; break;
    case TZIO_TOGGLE: polarityGet = GPIOTE_TOGGLE; break;
    }

    NRF_GPIOTE->CONFIG[gIrqCallbackNum] = (GPIOTE_CONFIG_MODE_Event << GPIOTE_CONFIG_MODE_Pos) | 
        (pin << GPIOTE_CONFIG_PSEL_Pos) | (polarityGet << GPIOTE_CONFIG_POLARITY_Pos);
    
    NRF_GPIOTE->EVENTS_IN[gIrqCallbackNum] = 0;
    NRF_GPIOTE->INTENSET = (1 << gIrqCallbackNum);

    gIrqCallback[gIrqCallbackNum].pin = pin;
    gIrqCallback[gIrqCallbackNum].callback = callback;
    gIrqCallbackNum++;
}

static bool isPinUsed(int pin) {
    for (int i = 0; i < gIrqCallbackNum; i++) {
        if (gIrqCallback[i].pin == pin) {
            return true;
        }
    }
    return false;
}

static void initGpiote(void) {
    for (int i = 0; i < 8; i++) {
        NRF_GPIOTE->EVENTS_IN[i] = 0;
    }
    
    NVIC_SetPriority(GPIOTE_IRQn, TZ_IRQ_PRIORITY_MIDDLE);
    NVIC_ClearPendingIRQ(GPIOTE_IRQn);
    NVIC_EnableIRQ(GPIOTE_IRQn);
}

void GPIOTE_IRQHandler(void) {   
    for (int i = 0; i < IRQ_CALLBACK_NUM_MAX; i++) {
        if (NRF_GPIOTE->EVENTS_IN[i]) {
            NRF_GPIOTE->EVENTS_IN[i] = 0;
            if (i < gIrqCallbackNum && gIrqCallback[i].callback) {
                gIrqCallback[i].callback();
            }
        }
    }
}

// TZIOIrqEnable 使能中断
// 本函数需要驱动定义
void TZIOIrqEnable(int pin) {
    if (pin >= gIrqCallbackNum) {
        return;
    }
    NRF_GPIOTE->TASKS_SET[pin] = 1;
}

// TZIOIrqDisable 禁止中断
// 本函数需要驱动定义
void TZIOIrqDisable(int pin) {
    if (pin >= gIrqCallbackNum) {
        return;
    }
    NRF_GPIOTE->TASKS_CLR[pin] = 1;
}
