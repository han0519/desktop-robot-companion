# 网易云音乐代理 — 部署与登录说明

固件本身**不包含任何网易云账号信息**，也不直接访问网易接口（网易直链带防盗链，
而且需要登录 Cookie 才能拿到完整歌曲）。所有网易云请求都经过你**自己部署**的代理。

```
ESP32 固件 ──HTTP──▶ 你的代理 (server/music_proxy.js) ──HTTPS──▶ 网易云
                        └─ 登录 Cookie 存在这里 (data/cookie.txt)
```

## 一、部署

### 本地 / 家里电脑

```bash
cd server
node music_proxy.js          # 默认 3000 端口
# 换端口: PORT=8080 node music_proxy.js
```

**零依赖**：只需要 Node.js 18 以上，不需要 `npm install`。

### 云服务器

```bash
# 1. 上传 server/music_proxy.js 到服务器
scp server/music_proxy.js root@<服务器IP>:/opt/music_proxy/

# 2. 用 systemd 常驻
cat >/etc/systemd/system/music-proxy.service <<'EOF'
[Unit]
Description=NetEase music proxy for ESP32 robot
After=network.target

[Service]
WorkingDirectory=/opt/music_proxy
ExecStart=/usr/bin/node /opt/music_proxy/music_proxy.js
Restart=always

[Install]
WantedBy=multi-user.target
EOF

systemctl daemon-reload
systemctl enable --now music-proxy

# 3. 防火墙放行端口
ufw allow 3000/tcp
```

> ⚠️ 安全提醒：代理接口**没有鉴权**，拿到地址的人都能用你的网易云账号听歌。
> 建议只放行家里的 IP，或者加一层 nginx basic-auth / 只在内网使用。

## 二、扫码登录（获取 Token）

固件烧录后**不需要**在固件里配置任何账号，登录在服务器侧完成：

1. 浏览器打开代理首页：`http://<服务器IP>:3000/`
2. 用**网易云音乐 APP**（手机）右上角「扫一扫」扫描页面上的二维码
3. 手机上点「确认登录」
4. 页面显示「登录成功：你的昵称」，Cookie 自动保存到 `server/data/cookie.txt`

```
状态码含义 (页面会自动提示):
  801 = 等待扫码
  802 = 已扫码, 等手机确认
  803 = 登录成功
  800 = 二维码过期, 点「刷新二维码」重来
```

### 查看登录状态 / 退出登录

```bash
curl http://127.0.0.1:3000/login/status
# {"code":200,"login":true,"nickname":"...","vipType":11}

# 退出登录: 删除 Cookie 文件后重启服务
rm server/data/cookie.txt && systemctl restart music-proxy
```

### Cookie 说明

- `data/cookie.txt` 里就是你的网易云登录态（`MUSIC_U=...`），**等同于账号密码**
- 不要提交到 git（本仓库 `.gitignore` 已排除）、不要发给别人
- Cookie 会过期，过期后重复上面的扫码流程即可
- 换账号 = 删掉文件重新扫码

## 三、固件侧配置

1. 网页控制台 →「音乐」→「代理地址」填 `http://<服务器IP>:3000` → 保存
   （存在设备 NVS 里，掉电不丢；也可以说「把音乐服务器设置为 xxx」让 AI 自己调 MCP 工具改）
2. 「搜索」输入歌名点播，或打开「我的歌单」（需已扫码登录）

## 四、代理接口一览（固件调用的就是这些）

| 接口 | 说明 | 返回 |
|---|---|---|
| `GET /api/search?keyword=<词>&limit=<N>` | 搜歌 | `{code,data:[{id,name,artist}]}` |
| `GET /api/user/playlists` | 我的歌单（需登录） | `{code,data:[{id,name,count}]}` |
| `GET /api/playlist/songs?id=<id>&limit=<N>&offset=0` | 歌单歌曲 | `{code,name,data:[{id,name,artist}]}` |
| `GET /api/stream?id=<歌曲id>&br=<码率>` | 音频流，支持 Range（断点续传） | 音频字节 / 404=无版权 |
| `GET /login/qr/key` | 取扫码 key | `{code,unikey}` |
| `GET /login/qr/check?key=<key>` | 轮询扫码结果 | `{code:801/802/803/800}` |
| `GET /login/status` | 登录状态 | `{code,login,nickname}` |

> VIP / 版权受限的歌拿不到播放地址，代理回 404，固件会自动跳到搜索结果里的下一首。

## 五、常见问题

| 现象 | 原因 / 处理 |
|---|---|
| 网页提示"未设置音乐代理地址" | 固件里没填代理地址，去「音乐」页填 |
| 搜得到歌但放不出声 | 未扫码登录，或该歌是 VIP/版权受限 |
| 「我的歌单」为空 | 未登录，或账号下没有歌单 |
| `二维码已过期` | 正常，点「刷新二维码」 |
| Cookie 失效 | 重新扫码 |
| 服务器公网部署后被人蹭 | 加 nginx 鉴权或限制来源 IP |
