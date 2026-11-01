# hyprhdr

在不修改 Hyprland 源码的前提下，让 HDR 输出把自身的 HDR 像素交给
`wlr-screencopy` 客户端；只交给绑定了 `wp_color_manager_v1` 的客户端，其他客户端
仍拿到原来的 8-bit sRGB。

同时复刻上游 fork 的 SDR 白点修复：HDR 输出发布的 `reference` 亮度改为该输出上
SDR 白实际渲染的亮度（`sdr_max_luminance × sdrbrightness`），而不是写死的 203 cd/m²。

## 适用版本

插件按运行中 Hyprland 的头文件编译，ABI 严格绑定。当前目标：

- Hyprland 0.57.0（`0aa6745f705f2c4b0a5004eb019ef76e5bbdb426`）
- 系统头文件位于 `/usr/include/hyprland`

升级 Hyprland 后必须重新编译；版本不匹配时插件会拒绝加载。

## 构建

```bash
git clone https://github.com/Haku-taku/hyprhdr.git
cd hyprhdr
make
```

产物：`./hyprhdr.so`。也可以用 hyprpm：

```bash
hyprpm add https://github.com/Haku-taku/hyprhdr
hyprpm enable hyprhdr
```

## 加载 / 卸载

```bash
hyprctl plugin load "$PWD/hyprhdr.so"
hyprctl plugin list
hyprctl plugin unload "$PWD/hyprhdr.so"
```

插件加载后默认不改变截屏行为（`screencopy_hdr = false`），先确认加载不崩，再打开开关。

## 配置

Hyprland 0.57 的 Lua 配置：

```lua
hl.config({
    plugin = {
        hyprhdr = {
            screencopy_hdr    = true,  -- 总开关
            debug             = false, -- 把决策写到 Hyprland 的 stderr / journal
            fix_hdr_reference = true,  -- 发布真实 SDR 白点
        },
    },
})
```

运行时修改（0.57 的 `hyprctl keyword` 在 Lua 配置下不可用，用 `eval`）：

```bash
hyprctl eval 'hl.config({plugin={hyprhdr={screencopy_hdr=true, debug=true}}})'
hyprctl getoption plugin:hyprhdr:screencopy_hdr
```

## 实现方式

全部通过 `HyprlandAPI::createFunctionHook` 挂到 0.57 的内部函数上：

| Hook | 作用 |
|---|---|
| `CScreenshareSession::calculateConstraints()` | HDR 输出 + CM-aware 客户端时，把 offer 的格式改成 10-bit `XBGR2101010` |
| `CScreenshareFrame::beginCopy()` | 同条件下把 capture framebuffer 的 image description 设成输出自己的 HDR 描述 |
| `CMonitorResources::mirrorFB()` | HDR 输出时把 mirror framebuffer 建成 10-bit，并写输出的 HDR 描述 |
| `CHyprOpenGLImpl::saveBufferForMirror()` | 从 linear work buffer 取源，并临时关闭 no-shader-CM，保证 mirror 真正写成 PQ |
| `CMonitor::getHDRImageDescription()` | 把 `luminances.reference` 改成真实 SDR 白 |
| `CScreenshareSession::stop()` | 清理该 session 的 HDR 决策缓存 |

格式 offer 和 capture 描述共用同一个按 session 缓存的决策（在 `calculateConstraints`
时记录 client），所以两者不会因为客户端中途绑定 `wp_color_manager_v1` 而分歧。

## 真实会话测试

下面的命令以输出 `DP-6` 为例，换成你自己的输出名（`hyprctl monitors`）。
验证客户端以 [VShot](https://github.com/Haku-taku/VShot) 为例：它绑定了
`wp_color_manager_v1`，会按输出自己的编码读取 10-bit 帧。

```bash
# 1. 构建
make

# 2. 先以默认关闭状态加载，确认 compositor 正常
hyprctl plugin load "$PWD/hyprhdr.so"
hyprctl plugin list
hyprctl rollinglog | grep hyprhdr

# 3. 打开开关与调试
hyprctl eval 'hl.config({plugin={hyprhdr={screencopy_hdr=true, debug=true}}})'
hyprctl getoption plugin:hyprhdr:screencopy_hdr

# 4. CM-aware 客户端：应看到 Xbgr2101010 / PQ / reference = sdr_max_luminance
VSHOT_HDR_DEBUG=1 vshot monitor DP-6 -o /tmp/hyprhdr-test.png --sdr-copy false

# 5. 非 CM-aware 客户端：应仍是 8-bit sRGB
grim -o DP-6 /tmp/hyprhdr-grim.png
identify -format '%wx%h depth=%z colorspace=%[colorspace]\n' /tmp/hyprhdr-grim.png

# 6. 查看插件决策
hyprctl rollinglog | grep hyprhdr

# 7. 关闭开关
hyprctl eval 'hl.config({plugin={hyprhdr={screencopy_hdr=false}}})'

# 8. 卸载
hyprctl plugin unload "$PWD/hyprhdr.so"
```

期望：

- VShot 日志出现 `Xbgr2101010 ... read as the output's own encoding`，
  输出颜色为 `Pq, Bt2020, reference_nits` 等于该输出的 `sdr_max_luminance × sdrbrightness`；
- grim 输出 `depth=8 colorspace=sRGB`；
- 开关关闭或插件卸载后，VShot 回到 `fourcc=0x34325241 ... offered no HDR buffer`。

## 已知限制

- 只处理 `wlr-screencopy`；workspace capture（`hyprland-workspace-image-capture-source-v1`）保持原样。
- “CM-aware” 只表示客户端绑定了 `wp_color_manager_v1`，不保证它真的按 HDR 解读像素。
- 非 CM-aware 客户端在 HDR 输出上仍走现有的 HDR→SDR 路径，高光会按当前 shader 行为裁剪。
- 运行中切换开关时，已存在的 capture session 要等下一次 constraints 重算才改变格式。
- 内部函数 hook 不受 API 稳定性保证；Hyprland 升级后若符号变化，插件会拒绝加载并报错。

## 许可证

[MIT](LICENSE)
