#pragma once
#include "lvgl.h"

typedef enum {
    PET_SPRITE_IDLE = 0,
    PET_SPRITE_LISTEN,
    PET_SPRITE_THINK,
    PET_SPRITE_TALK,
    PET_SPRITE_COUNT
} pet_sprite_id_t;

extern const lv_image_dsc_t g_pet_sprites[PET_SPRITE_COUNT];