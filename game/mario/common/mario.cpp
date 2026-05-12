#include "common/common_types.h"
#include "mario.h"

int32_t m_mario_id = -1;
void mario_init() {
  m_mario_id = sm64_mario_create(100, 100, 100);
};
