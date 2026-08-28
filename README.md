# 会说话的宠物猫｜TALKING PET

Talking Pet 把 AI Passport 变成一只会认真听你说话、再用俏皮声音复述的随身精灵。角色会随着倾听、思考和回应切换动作与表情，适合模仿台词、朋友聚会或随手制造一点笑声。

![Talking Pet on AI Passport](assets/images/community-cover.png)

## 怎么玩

1. 长按确认键开始说话。
2. 松开确认键，宠物会立即复述；单次最长 8 秒。
3. 按上键提高音量，按下键降低音量。

录音保存在运行内存中，每次新录音会覆盖上一段，不会持续占用存储空间。

## How to play

1. Hold the confirm button and speak.
2. Release it to hear the pet mimic you; each recording can be up to eight seconds.
3. Use the up and down buttons to adjust the volume.

The recording stays in working memory and is replaced by the next one, so it does not accumulate in storage.

## 特色 / Highlights

- 精致的原创像素角色与多状态动画
- 更高、更俏皮但保持清晰的复述声音
- 按住即录、松开即播，交互直接且延迟低
- 10%–100% 音量调节
- 无需联网即可游玩

## Build and flash

The project uses ESP-IDF 5.5.3. From an initialized ESP-IDF terminal:

```sh
idf.py build
idf.py -p PORT flash monitor
```

The target is the FoloToy AI Passport. A merged community-release image is generated separately and is not committed to this repository.

## Project structure

- `main/main.c` — interaction, recording, playback, and UI state flow
- `main/pet_sprites.c` — embedded character frames
- `components/bsp/` — device support used by the application
- `assets/images/` — source artwork, processed frames, and community cover

## License

Released under the MIT License. See [LICENSE](LICENSE).
