#pragma once

typedef enum
{
    GPIO_NUM_NC = -1,
    GPIO_NUM_4 = 4,
    GPIO_NUM_5 = 5,
    GPIO_NUM_7 = 7
} gpio_num_t;

/* Test double only: actual target validation comes from ESP-IDF's header. */
#define GPIO_IS_VALID_OUTPUT_GPIO(pin) ((pin) >= 0 && (pin) <= 21)
