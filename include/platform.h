#ifndef PLATFORM_H
#define PLATFORM_H

#include <string.h>
#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>

#ifndef rt_memset
#define rt_memset memset
#endif

#define BIT_TO_BYTE(BIT0, BIT1, BIT2, BIT3, BIT4, BIT5, BIT6, BIT7) \
    ((BIT0) | ((BIT1) << 1) | ((BIT2) << 2) | ((BIT3) << 3) |       \
     ((BIT4) << 4) | ((BIT5) << 5) | ((BIT6) << 6) | ((BIT7) << 7))

#endif // PLATFORM_H
