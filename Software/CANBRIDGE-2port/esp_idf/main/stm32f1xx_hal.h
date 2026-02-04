#include <stdint.h>

/* A stub */
struct can_handle {
	uint8_t h;
};

/* Not used anywhere in the core logic. Exist just to silence the compiler */
typedef struct can_handle CAN_HandleTypeDef;
