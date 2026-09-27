#pragma once

/*
 * web_page.h — 设备自带控制台 (配网完成后 http://<设备IP>/ 打开)
 *
 * 结构: 【一个主页面 + 8 个子页面】
 *   主页面  #/home   状态总览(当前表情/小智/音乐/联网/传感器) + 功能入口宫格 + 快捷操作
 *   子页面  #/music  网易云音乐(搜索/歌单/曲目列表/播放控制/代理地址)
 *           #/face   表情(51 种 + 自动播放开关)
 *           #/light  灯环(6 种效果 + 亮度 + 取色)
 *           #/servo  头部动作(转头/点头 + 8 个预设)
 *           #/sound  声音(音量/唤醒应答/免触摸对话/TTS 自检)
 *           #/screen 屏幕(方向翻转/方向测试图)
 *           #/net    网络(WiFi 账号/重新配网)
 *           #/sys    系统(激活码/运行统计/诊断信息)
 *
 * 为什么用 hash 路由而不是多文件:
 *   固件里没有文件系统, 页面上所有东西都在同一个字符串里。用 location.hash 切
 *   换显示的 <section>, 好处是: ① 不用多发一个请求, 切换是瞬时的; ② 浏览器
 *   (还有安卓的返回手势/键) 的后退天然就能回到主页面; ③ 可以直接把
 *   http://IP/#/music 存成书签。
 *
 * 一如既往的设计要点:
 *   1. 所有"动作类"操作都走 /mcp?name=<工具名>, 也就是【和 AI 完全相同的
 *      代码路径】—— AI 能做的网页都能做, 以后新增 MCP 工具网页自动就有,
 *      不用为每个功能单独写接口。
 *   2. 滑块类(亮度/角度/音量)走各自的专用接口, 因为要连续拖动。
 *   3. 状态每 500ms 从 /status 拉一次。所有子页面的 DOM 都一直存在(只是
 *      隐藏), 所以轮询可以直接更新, 不用等页面显示才去拉。
 */
static const char *WEB_PAGE = R"HTML(
<!DOCTYPE html>
<html lang="zh">
<head>
<meta charset="UTF-8">
<meta name="viewport" content="width=device-width,initial-scale=1,viewport-fit=cover">
<meta name="theme-color" content="#0f0f1a">
<title>喵伴控制台</title>
<style>
*{box-sizing:border-box;-webkit-tap-highlight-color:transparent}
body{font-family:-apple-system,"PingFang SC","Microsoft YaHei",sans-serif;background:#0f0f1a;color:#eee;margin:0;padding:0 14px 34px;max-width:620px;margin:0 auto}

/* ---------- 顶栏: 主页面是标题, 子页面变成"返回 + 标题" ---------- */
#hdr{position:sticky;top:0;z-index:9;display:flex;align-items:center;gap:10px;
  padding:12px 0 11px;background:#0f0f1a;border-bottom:1px solid #1e1e33}
#hdr .back{flex:0 0 34px;height:34px;line-height:31px;text-align:center;font-size:26px;
  color:#00d4ff;border:1px solid #2a2a44;border-radius:10px;cursor:pointer;
  visibility:hidden;font-weight:300}
#hdr .back:active{background:#16213e}
#hdr .ttl{flex:1;font-size:17px;font-weight:600;color:#e8f4ff;overflow:hidden;
  text-overflow:ellipsis;white-space:nowrap}
#hdr .pill{flex:0 0 auto;font-size:11px;padding:4px 10px;border-radius:20px;
  background:#16213e;border:1px solid #2a2a44;color:#8b95bb;max-width:44%;
  overflow:hidden;text-overflow:ellipsis;white-space:nowrap}
#hdr .pill.on{background:#0d2b3a;border-color:#00d4ff;color:#00d4ff}

/* ---------- 页面切换 ---------- */
.page{display:none;padding-top:4px}
.page.show{display:block;animation:fade .18s ease-out}
@keyframes fade{from{opacity:0;transform:translateY(5px)}to{opacity:1;transform:none}}

h1{text-align:center;color:#00d4ff;font-size:21px;margin:8px 0 14px}
.sec{font-size:12px;color:#6b76a0;letter-spacing:1px;margin:18px 0 9px}
.card{background:#1a1a2e;border-radius:12px;padding:14px;margin:12px 0}
.card h2{font-size:14px;color:#8b95bb;margin:0 0 10px;border-bottom:1px solid #2a2a44;padding-bottom:7px;letter-spacing:.5px}
.row{display:flex;justify-content:space-between;align-items:center;padding:5px 0;font-size:14px;gap:8px}
.row .label{color:#8b95bb;flex:0 0 auto}
.row .val{color:#00d4ff;font-weight:bold;text-align:right;overflow:hidden;text-overflow:ellipsis;white-space:nowrap}
.grid{display:grid;gap:7px}
.g4{grid-template-columns:repeat(4,1fr)}
.g3{grid-template-columns:repeat(3,1fr)}
.g2{grid-template-columns:repeat(2,1fr)}
.cbtn{padding:9px 2px;background:#16213e;color:#ccd;border:1px solid #2a2a44;border-radius:8px;font-size:12px;cursor:pointer;text-align:center;transition:.15s}
.cbtn:active{transform:scale(.95)}
.cbtn.on{background:#00d4ff;color:#001018;border-color:#00d4ff;font-weight:bold}
.emo-btn{padding:8px 2px;background:#16213e;color:#ccd;border:1px solid #2a2a44;border-radius:6px;font-size:11px;cursor:pointer;text-align:center}
.emo-btn.active{background:#00d4ff;color:#001018;border-color:#00d4ff}
.slider-row{margin:9px 0}
.slider-row label{font-size:12.5px;color:#8b95bb;display:block;margin-bottom:5px}
.slider-row .v{float:right;color:#00d4ff;font-weight:bold}
input[type=range]{width:100%;height:6px;-webkit-appearance:none;background:#2a2a44;border-radius:3px;outline:none}
input[type=range]::-webkit-slider-thumb{-webkit-appearance:none;width:22px;height:22px;border-radius:50%;background:#00d4ff;cursor:pointer}
input[type=text],input[type=password]{width:60%;padding:7px;background:#0f0f1a;color:#eee;border:1px solid #2a2a44;border-radius:7px;font-size:13px}
.sw{position:relative;display:inline-block;width:46px;height:25px;flex:0 0 auto}
.sw input{display:none}
.sw i{position:absolute;cursor:pointer;inset:0;background:#2a2a44;border-radius:13px;transition:.25s}
.sw i:before{content:"";position:absolute;height:19px;width:19px;left:3px;bottom:3px;background:#fff;border-radius:50%;transition:.25s}
.sw input:checked+i{background:#00d4ff}
.sw input:checked+i:before{transform:translateX(21px)}
.tip{font-size:11.5px;color:#6b76a0;line-height:1.55;margin-top:6px}
.chip{display:inline-block;padding:3px 9px;background:#16213e;border:1px solid #2a2a44;border-radius:20px;font-size:11px;color:#8b95bb;margin:0 4px 4px 0;cursor:pointer}
.chip:active{background:#00d4ff;color:#001018}
.mlist{margin-top:9px;max-height:270px;overflow-y:auto}
.mitem{display:flex;align-items:center;gap:8px;padding:8px 10px;background:#16213e;border:1px solid #2a2a44;border-radius:8px;margin-bottom:5px;cursor:pointer;font-size:13px;transition:.15s}
.mitem:active{transform:scale(.98)}
.mitem.cur{border-color:#00d4ff;background:#0d2b3a}
.mitem .no{color:#6b76a0;flex:0 0 20px;font-size:11px}
.mitem .nm{flex:1;overflow:hidden;text-overflow:ellipsis;white-space:nowrap}
.mitem .ar{color:#8b95bb;font-size:11px;max-width:34%;overflow:hidden;text-overflow:ellipsis;white-space:nowrap}
.mitem .pl{color:#00d4ff;font-size:11px;flex:0 0 auto}
.mitem.cur .pl{font-weight:bold}

/* ---------- 主页面 ---------- */
.hero{display:flex;align-items:center;gap:14px;background:linear-gradient(135deg,#16213e,#1a1a2e);
  border:1px solid #2a2a44;border-radius:14px;padding:16px;margin-top:12px}
.hero .avatar{flex:0 0 62px;height:62px;border-radius:50%;background:#0d2b3a;border:2px solid #00d4ff;
  display:flex;align-items:center;justify-content:center;font-size:13px;color:#00d4ff;font-weight:600}
.hero .hn{font-size:19px;font-weight:700;color:#e8f4ff;letter-spacing:.5px}
.hero .hs{font-size:12.5px;color:#8b95bb;margin-top:4px;line-height:1.5}
.kpis{display:grid;grid-template-columns:repeat(4,1fr);gap:8px;margin-top:10px}
.kpi{background:#1a1a2e;border:1px solid #2a2a44;border-radius:11px;padding:10px 4px;text-align:center}
.kpi b{display:block;font-size:13px;color:#00d4ff;font-weight:700;overflow:hidden;text-overflow:ellipsis;white-space:nowrap}
.kpi span{display:block;font-size:10.5px;color:#6b76a0;margin-top:4px}
.tiles{display:grid;grid-template-columns:repeat(4,1fr);gap:9px}
.tile{background:#1a1a2e;border:1px solid #2a2a44;border-radius:12px;padding:13px 4px 11px;
  text-align:center;cursor:pointer;transition:.15s}
.tile:active{transform:scale(.95);background:#16213e}
.tile .ic{width:34px;height:34px;margin:0 auto 7px;border-radius:10px;background:#16213e;
  border:1px solid #2a2a44;display:flex;align-items:center;justify-content:center;
  font-size:15px;color:#00d4ff;font-weight:600}
.tile .tn{font-size:12px;color:#ccd}
.tile .ts{font-size:10px;color:#6b76a0;margin-top:3px;height:12px;overflow:hidden}
</style>
</head>
<body>

<header id="hdr">
  <div class="back" id="back" onclick="goBack()">&#8249;</div>
  <div class="ttl" id="ttl">控制台</div>
  <div class="pill" id="pill">--</div>
</header>

<!-- ==================================================================== -->
<!-- ============================ 主  页  面 ============================ -->
<!-- ==================================================================== -->
<section class="page show" id="p-home">

  <div class="hero">
    <div class="avatar" id="hero_emo">平静</div>
    <div>
      <div class="hn">喵伴</div>
      <div class="hs" id="hero_ai">正在读取状态…</div>
    </div>
  </div>

  <div class="kpis">
    <div class="kpi"><b id="k_net">--</b><span>联网</span></div>
    <div class="kpi"><b id="k_mus">--</b><span>音乐</span></div>
    <div class="kpi"><b id="k_light">--</b><span>灯光</span></div>
    <div class="kpi"><b id="k_env">--</b><span>温湿度</span></div>
  </div>

  <div class="sec">功能</div>
  <div class="tiles">
    <div class="tile" onclick="go('music')"><div class="ic">音</div><div class="tn">音乐</div><div class="ts" id="t_mus">--</div></div>
    <div class="tile" onclick="go('face')"><div class="ic">貌</div><div class="tn">表情</div><div class="ts" id="t_emo">52 种</div></div>
    <div class="tile" onclick="go('light')"><div class="ic">光</div><div class="tn">灯环</div><div class="ts" id="t_light">--</div></div>
    <div class="tile" onclick="go('servo')"><div class="ic">动</div><div class="tn">动作</div><div class="ts" id="t_servo">--</div></div>
    <div class="tile" onclick="go('sound')"><div class="ic">声</div><div class="tn">声音</div><div class="ts" id="t_vol">--</div></div>
    <div class="tile" onclick="go('screen')"><div class="ic">屏</div><div class="tn">屏幕</div><div class="ts" id="t_flip">--</div></div>
    <div class="tile" onclick="go('net')"><div class="ic">网</div><div class="tn">网络</div><div class="ts" id="t_net">--</div></div>
    <div class="tile" onclick="go('sys')"><div class="ic">系</div><div class="tn">系统</div><div class="ts">诊断</div></div>
  </div>

  <div class="sec">快捷操作</div>
  <div class="grid g4">
    <div class="cbtn" onclick="musicCtl('pause')">暂停音乐</div>
    <div class="cbtn" onclick="musicCtl('resume')">继续</div>
    <div class="cbtn" onclick="musicCtl('stop')">停止</div>
    <div class="cbtn" onclick="beep()">测试音</div>
  </div>

  <div class="sec">传感器</div>
  <div class="card" style="margin-top:0">
    <div class="row"><span class="label">头部触摸</span><span class="val" id="touch_h">--</span></div>
    <div class="row"><span class="label">红外 左/右</span><span class="val"><span id="ir_l">--</span> / <span id="ir_r">--</span></span></div>
    <div class="row"><span class="label">跟手状态</span><span class="val" id="track">--</span></div>
    <div class="row"><span class="label">麦克风能量</span><span class="val" id="mic_v">--</span></div>
  </div>

  <div class="tip" style="margin-top:14px">
    摸头顶 1 下 = 撸猫, 2 下 = 换灯效, 3 下 = 音乐暂停/继续。<br>
    说话前先喊「小智小智」, 或在「声音」里打开免触摸对话。
  </div>
</section>

<!-- ==================================================================== -->
<!-- ============================ 音    乐 ============================== -->
<!-- ==================================================================== -->
<section class="page" id="p-music">
  <div class="card">
    <h2>搜索 / 点播</h2>
    <div class="row" style="margin-bottom:6px">
      <input type="text" id="msong" placeholder="歌名 或 歌手+歌名" style="flex:1"
             onkeydown="if(event.key==='Enter')musicSearch()">
      <button class="cbtn on" style="flex:0 0 58px;padding:8px 0" onclick="musicSearch()">搜索</button>
      <button class="cbtn" style="flex:0 0 58px;padding:8px 0" onclick="musicPlayKw()">直接播</button>
    </div>
    <div class="row" style="margin-bottom:6px">
      <button class="cbtn" style="flex:1;padding:8px 0" onclick="loadPlaylists()">我的歌单</button>
      <button class="cbtn" style="flex:0 0 74px;padding:8px 0" onclick="hidePlaylists()">收起</button>
    </div>
    <!-- 我的歌单(账号里的歌单) -->
    <div class="mlist" id="plist" style="display:none;margin-bottom:10px"></div>
    <!-- 曲目列表(搜索结果 或 某个歌单): 点哪首播哪首 -->
    <div class="row" style="font-size:11.5px;color:#8b95bb;margin:0 0 5px" id="src_v">曲目列表</div>
    <div class="mlist" id="mlist"></div>
  </div>

  <div class="card">
    <h2>播放控制</h2>
    <div class="row" style="margin-bottom:8px"><span class="label">当前</span><span class="val" id="mus_v2">空闲</span></div>
    <div class="grid g4">
      <div class="cbtn" onclick="musicCtl('pause')">暂停</div>
      <div class="cbtn" onclick="musicCtl('resume')">继续</div>
      <div class="cbtn" onclick="musicCtl('next')">下一首</div>
      <div class="cbtn" onclick="musicCtl('stop')">停止</div>
    </div>
    <div class="row" style="margin-top:10px">
      <span class="label">代理地址</span>
      <input type="text" id="mbase" placeholder="http://ip:3000" style="flex:1;font-size:12px">
      <button class="cbtn" style="flex:0 0 54px" onclick="saveMusicBase()">保存</button>
    </div>
    <div class="tip">
      ★ 网易云需要<b>扫码登录</b>: 部署好音乐代理后, 用浏览器打开上面的<b>代理地址</b>,
      用网易云音乐 APP 扫二维码登录(Cookie 保存在服务器, 固件不用配置)。「我的歌单」需登录后才能用。<br>
      搜出来的列表就是歌单, 点任意一首即播; 「下一首」会顺着这个歌单往下放。放歌时灯环会跟着节奏律动。
      摸头顶 3 下可直接暂停/继续。代理部署见仓库 <b>docs/MUSIC_PROXY.md</b>。
    </div>
  </div>
</section>

<!-- ==================================================================== -->
<!-- ============================ 表    情 ============================== -->
<!-- ==================================================================== -->
<section class="page" id="p-face">
  <div class="card">
    <h2>表情</h2>
    <div class="row">
      <span class="label">自动播放表情</span>
      <label class="sw"><input type="checkbox" id="auto" checked onchange="toggleAuto()"><i></i></label>
    </div>
    <div class="tip">关掉之后表情就定在下面点的那一个, 不再自己换。</div>
    <div class="grid g4" id="emoGrid" style="margin-top:10px"></div>
  </div>
</section>

<!-- ==================================================================== -->
<!-- ============================ 灯    环 ============================== -->
<!-- ==================================================================== -->
<section class="page" id="p-light">
  <div class="card">
    <h2>灯效</h2>
    <div class="grid g3" id="lightGrid"></div>
    <div class="slider-row" style="margin-top:11px">
      <label>亮度 <span class="v" id="lbr_v">--</span></label>
      <input type="range" id="lbr" min="0" max="100" value="50" oninput="setLightBr(this.value)">
    </div>
    <div style="margin-top:4px">
      <span class="chip" onclick="setColor(255,0,0)">红</span>
      <span class="chip" onclick="setColor(0,255,0)">绿</span>
      <span class="chip" onclick="setColor(0,80,255)">蓝</span>
      <span class="chip" onclick="setColor(255,180,0)">黄</span>
      <span class="chip" onclick="setColor(180,0,255)">紫</span>
      <span class="chip" onclick="setColor(0,255,220)">青</span>
      <span class="chip" onclick="setColor(255,255,255)">白</span>
      <span class="chip" onclick="nextLight()">下一个 &#9656;</span>
    </div>
    <div class="tip">取色会自动切到「单色常亮」。</div>
  </div>
</section>

<!-- ==================================================================== -->
<!-- ============================ 头部动作 ============================== -->
<!-- ==================================================================== -->
<section class="page" id="p-servo">
  <div class="card">
    <h2>手动调整</h2>
    <div class="slider-row">
      <label>左右转头 <span class="v" id="pan_s">90°</span></label>
      <input type="range" id="pan" min="0" max="180" value="90" oninput="setServo()">
    </div>
    <div class="slider-row">
      <label>上下点头 <span class="v" id="tilt_s">90°</span></label>
      <input type="range" id="tilt" min="0" max="180" value="90" oninput="setServo()">
    </div>
  </div>
  <div class="card">
    <h2>预设动作</h2>
    <div class="grid g4">
      <div class="cbtn" onclick="mcp('self.servo.look_center')">正前方</div>
      <div class="cbtn" onclick="mcp('self.servo.look_left')">看左边</div>
      <div class="cbtn" onclick="mcp('self.servo.look_right')">看右边</div>
      <div class="cbtn" onclick="mcp('self.servo.nod')">点头</div>
      <div class="cbtn" onclick="mcp('self.servo.shake_head')">摇头</div>
      <div class="cbtn" onclick="mcp('self.servo.look_center');setSlider(90,90)">回正</div>
      <div class="cbtn" onclick="mcp('self.servo.get_state')">查角度</div>
      <div class="cbtn" onclick="setSlider(90,95)">低头</div>
    </div>
    <div class="tip">跟手(手靠近就转向)状态在主页面的「传感器」里看。</div>
  </div>
</section>

<!-- ==================================================================== -->
<!-- ============================ 声    音 ============================== -->
<!-- ==================================================================== -->
<section class="page" id="p-sound">
  <div class="card">
    <h2>音量</h2>
    <div class="slider-row">
      <label>音量 <span class="v" id="vol_v">--</span></label>
      <input type="range" id="vol" min="0" max="100" oninput="setVol(this.value)">
    </div>
    <div class="grid g3" style="margin-top:8px">
      <div class="cbtn" onclick="beep()">测试音</div>
      <div class="cbtn" onclick="mcp('self.audio_speaker.get_volume')">查音量</div>
      <div class="cbtn" onclick="ttsTest()">TTS 自检</div>
    </div>
  </div>
  <div class="card">
    <h2>对话</h2>
    <div class="row">
      <span class="label">唤醒应答音「叮咚」</span>
      <label class="sw"><input type="checkbox" id="wbeep" onchange="setWakeBeep(this.checked)"><i></i></label>
    </div>
    <div class="row">
      <span class="label">免触摸对话(直接说话就触发)</span>
      <label class="sw"><input type="checkbox" id="vad" onchange="setVad(this.checked)"><i></i></label>
    </div>
    <div class="tip">免触摸开启时: 直接说话自动开始, 停约 0.8 秒自动结束。误触发就关掉, 改用<b>身体触摸</b>说话。</div>
  </div>
</section>

<!-- ==================================================================== -->
<!-- ============================ 屏    幕 ============================== -->
<!-- ==================================================================== -->
<section class="page" id="p-screen">
  <div class="card">
    <h2>显示方向</h2>
    <div class="row"><span class="label">当前方向</span><span class="val" id="flip_v">--</span></div>
    <div class="grid g4">
      <div class="cbtn" onclick="flip(0,0)">正常</div>
      <div class="cbtn" onclick="flip(0,1)">上下翻</div>
      <div class="cbtn" onclick="flip(1,0)">左右翻</div>
      <div class="cbtn" onclick="flip(1,1)">180°</div>
    </div>
    <div class="grid g2" style="margin-top:8px">
      <div class="cbtn" id="tbtn" onclick="testPattern()">显示方向测试图</div>
      <div class="cbtn" onclick="mcp('self.light.next_effect')">切灯效(AI 同款)</div>
    </div>
    <div class="tip">测试图: 大三角朝上、方块在左上角、横条在底部。任一项不对就换方向。</div>
  </div>
</section>

<!-- ==================================================================== -->
<!-- ============================ 网    络 ============================== -->
<!-- ==================================================================== -->
<section class="page" id="p-net">
  <div class="card">
    <h2>联网状态</h2>
    <div class="row"><span class="label">当前</span><span class="val" id="net_now">--</span></div>
    <div class="row"><span class="label">IP 地址</span><span class="val" id="net_ip">--</span></div>
    <div class="tip">改完 WiFi 会立刻重连, 这时页面可能短暂打不开 —— 等设备报出新 IP 后改用新地址访问。</div>
  </div>
  <div class="card">
    <h2>更换 WiFi</h2>
    <div class="row"><span class="label">名称</span><input type="text" id="w_ssid" placeholder="家里路由 SSID"></div>
    <div class="row" style="margin-top:6px"><span class="label">密码</span><input type="password" id="w_pass" placeholder="密码"></div>
    <div class="grid g2" style="margin-top:9px">
      <div class="cbtn on" onclick="saveWifi()">保存并重连</div>
      <div class="cbtn" onclick="if(confirm('清空 WiFi 并回到配网模式?'))fetch('/reprov')">重新配网</div>
    </div>
    <div class="tip">「重新配网」会开热点 <b>Robot-Config</b>, 手机连上后随便打开一个网页就会弹配置页。</div>
  </div>
</section>

<!-- ==================================================================== -->
<!-- ============================ 系    统 ============================== -->
<!-- ==================================================================== -->
<section class="page" id="p-sys">
  <div class="card">
    <h2>设备</h2>
    <div class="row"><span class="label">激活码</span><span class="val" id="act_v">--</span></div>
    <div class="row"><span class="label">小智状态</span><span class="val" id="ai_v">--</span></div>
    <div class="row"><span class="label">唤醒词</span><span class="val" id="wake_v">--</span></div>
    <div class="row"><span class="label">当前表情</span><span class="val" id="emo_v">--</span></div>
  </div>
  <div class="card">
    <h2>运行统计</h2>
    <div class="row"><span class="label">上行 / 下行帧</span><span class="val"><span id="tx_v">--</span> / <span id="rx_v">--</span></span></div>
    <div class="row"><span class="label">麦克风能量</span><span class="val" id="mic_v2">--</span></div>
    <div class="row"><span class="label">跟手状态</span><span class="val" id="track_v">--</span></div>
    <div class="row"><span class="label">头部角度</span><span class="val"><span id="pan_v">--</span> / <span id="tilt_v">--</span></span></div>
    <div class="grid g3" style="margin-top:9px">
      <div class="cbtn" onclick="mcp('self.sensor.get_temperature_humidity')">读温湿度</div>
      <div class="cbtn" onclick="go('net')">网络设置</div>
      <div class="cbtn" onclick="location.reload()">刷新页面</div>
    </div>
  </div>
</section>

<script>
/* ==================================================================
 * 路由: #/home 是主页面, 其余是子页面。用 location.hash 而不是多文件,
 * 好处是无请求瞬时切换 + 浏览器/安卓返回键天然可用。
 * ================================================================== */
const PAGES={home:'控制台', music:'网易云音乐', face:'表情', light:'灯环',
             servo:'头部动作', sound:'声音', screen:'屏幕', net:'网络', sys:'系统'};
function cur(){
  const h=(location.hash||'').replace(/^#\/?/,'');
  return PAGES[h]?h:'home';
}
function route(){
  const p=cur();
  document.querySelectorAll('.page').forEach(function(e){ e.classList.remove('show'); });
  document.getElementById('p-'+p).classList.add('show');
  document.getElementById('ttl').textContent=PAGES[p];
  document.getElementById('back').style.visibility=(p==='home')?'hidden':'visible';
  window.scrollTo(0,0);
  /* 从子页面回到主页面时, 立刻刷一次状态, 不用等下一个 500ms 周期 */
  if(p==='home') refresh();
}
function go(p){ location.hash='#/'+p; }
/* 只有两级(主页 → 子页), 所以"返回"就是回主页 —— 行为可预期,
   不依赖 history 长度(直接输入 http://IP/#/music 进来时 history 会退到站外)。 */
function goBack(){ location.hash='#/home'; }
window.addEventListener('hashchange',route);

/* ---------- 通用: 直接调用 MCP 工具 (和 AI 同一条路径) ---------- */
function mcp(name,args){
  let u='/mcp?name='+encodeURIComponent(name);
  if(args) u+='&args='+encodeURIComponent(JSON.stringify(args));
  return fetch(u).then(r=>r.text());
}

/* ---------- 灯光 ---------- */
const LIGHTS=["关灯","单色常亮","呼吸","渐变","彩虹","警灯","律动"];
const lgrid=document.getElementById('lightGrid');
LIGHTS.forEach((n,i)=>{const b=document.createElement('div');
  b.className='cbtn';b.id='L'+i;b.textContent=n;b.onclick=()=>setLight(i);lgrid.appendChild(b);});
function setLight(i){ fetch('/light?effect='+i); }
function nextLight(){ fetch('/light?next=1'); }
function setLightBr(v){ document.getElementById('lbr_v').textContent=v+'%'; fetch('/light?br='+v); }
function setColor(r,g,b){ mcp('self.light.set_color',{color:'#'+[r,g,b].map(x=>x.toString(16).padStart(2,'0')).join('')}); }

/* ---------- 音乐: 搜索 → 歌单 → 点播 ---------- */
let mlistHTML='';
function esc(s){
  return String(s==null?'':s).replace(/[<>&]/g,function(c){
    return c==='<'?'&lt;':(c==='>'?'&gt;':'&amp;');
  });
}
function musicSearch(){
  const s=document.getElementById('msong').value.trim();
  if(!s){ alert('请输入歌名'); return; }
  document.getElementById('mlist').innerHTML='<div class="mitem" style="cursor:default;color:#6b76a0">搜索中…</div>';
  mlistHTML='';
  fetch('/music?search='+encodeURIComponent(s));
}
function musicPlayKw(){
  const s=document.getElementById('msong').value.trim();
  if(!s){ alert('请输入歌名'); return; }
  fetch('/music?q='+encodeURIComponent(s));
}
function playId(id){ fetch('/music?play='+encodeURIComponent(id)); }
function musicCtl(c){ fetch('/music?ctl='+c); }
function saveMusicBase(){
  const b=document.getElementById('mbase').value.trim();
  if(!b){ alert('请输入代理地址, 例: http://192.168.1.10:3000'); return; }
  fetch('/music?serve='+encodeURIComponent(b)).then(()=>alert('已保存'));
}
/* ---------- 我的歌单 ---------- */
let plHTML='', plShown=false;
function loadPlaylists(){
  plShown=true;
  const box=document.getElementById('plist');
  box.style.display='block';
  box.innerHTML='<div class="mitem" style="cursor:default;color:#6b76a0">加载中…</div>';
  plHTML='';
  fetch('/music?plsreq=1');     /* 触发一次; 结果由 /music?pls=1 轮询读取 */
}
function hidePlaylists(){
  plShown=false;
  document.getElementById('plist').style.display='none';
}
/* 打开某个歌单: 把它的歌装进曲目列表, 之后就能点播 */
function openPlaylist(id){
  document.getElementById('mlist').innerHTML='<div class="mitem" style="cursor:default;color:#6b76a0">读取歌单中…</div>';
  mlistHTML='';
  fetch('/music?plsload='+encodeURIComponent(id)).then(function(){ setTimeout(refreshMusic,300); });
}
function refreshPlaylists(){
  if(!plShown) return;
  fetch('/music?pls=1').then(function(r){return r.json();}).then(function(d){
    const box=document.getElementById('plist');
    if(!d.ready){ return; }                 /* 还在拉, 保持"加载中…" */
    if(!d.count){
      if(plHTML!=='empty'){ box.innerHTML='<div class="mitem" style="cursor:default;color:#6b76a0">没拿到歌单 —— 检查代理地址与网易云登录状态</div>'; plHTML='empty'; }
      return;
    }
    const html=d.items.map(function(it){
      return '<div class="mitem" onclick="openPlaylist(\''+it.id+'\')">'
           + '<span class="no">'+it.count+'</span>'
           + '<span class="nm">'+esc(it.name)+'</span>'
           + '<span class="pl">打开</span></div>';
    }).join('');
    if(html!==plHTML){ box.innerHTML=html; plHTML=html; }
  }).catch(function(){});
}
/* 拉当前曲目列表并渲染; 只有内容变了才重建 DOM, 免得正在点的按钮跳走 */
function refreshMusic(){
  fetch('/music?list=1').then(function(r){return r.json();}).then(function(d){
    const box=document.getElementById('mlist');
    let html;
    if(!d.items || !d.items.length){
      html='<div class="mitem" style="cursor:default;color:#6b76a0">还没有搜索结果 —— 上面输入歌名点「搜索」</div>';
    }else{
      html=d.items.map(function(it,i){
        const cur2=(i===d.idx && d.busy);
        return '<div class="mitem'+(cur2?' cur':'')+'" onclick="playId(\''+it.id+'\')">'
             + '<span class="no">'+(i+1)+'</span>'
             + '<span class="nm">'+esc(it.name)+'</span>'
             + '<span class="ar">'+esc(it.artist)+'</span>'
             + '<span class="pl">'+(cur2?'▶ 播放中':'播放')+'</span></div>';
      }).join('');
    }
    if(html!==mlistHTML){ box.innerHTML=html; mlistHTML=html; }
    document.getElementById('src_v').textContent =
      d.src ? ('曲目来源: '+d.src+'  (共 '+d.count+' 首)') : '曲目列表';
  }).catch(function(){});
}

/* ---------- 声音 ---------- */
function setVol(v){ document.getElementById('vol_v').textContent=v; mcp('self.audio_speaker.set_volume',{volume:parseInt(v)}); }
function beep(){ fetch('/beep?f=1000&ms=250'); }
function setWakeBeep(on){ fetch('/wakebeep?on='+(on?1:0)); }
function setVad(on){ fetch('/vad?on='+(on?1:0)); }
/* 让设备自己播一段测试语音(不经过服务端), 用来单独验证"能不能出声" */
function ttsTest(){ fetch('/ttstest?sec=3'); }

/* ---------- 表情 ---------- */
const EMOS=["平静","开心","难过","生气","惊讶","困倦","喜爱","眨眼","哭泣","晕眩","兴奋","酷","害羞","得意","困惑","思考",
"微笑","大笑","不耐烦","震惊","惊恐","慌乱","熟睡","打哈欠","疲惫","亲亲","感动","左眨","右眨","骄傲","好奇","怀疑",
"无聊","松口气","坚毅","严肃","蠢萌","不舒服","离线","打招呼","再见","聆听","听歌","加载中","错误","冷","热","饿",
"崇拜","心虚","成功","醒来"];
const grid=document.getElementById('emoGrid');
EMOS.forEach((n,i)=>{const b=document.createElement('div');
  b.className='emo-btn';b.textContent=n;b.id='e'+i;b.onclick=()=>setEmotion(i);grid.appendChild(b);});
function setEmotion(i){ fetch('/emotion?e='+i); document.getElementById('auto').checked=false; fetch('/auto?enable=0'); }
function toggleAuto(){ fetch('/auto?enable='+(document.getElementById('auto').checked?1:0)); }

/* ---------- 头部 / 屏幕 ---------- */
function setServo(){
  const p=document.getElementById('pan').value,t=document.getElementById('tilt').value;
  document.getElementById('pan_s').textContent=p+'°';
  document.getElementById('tilt_s').textContent=t+'°';
  fetch('/servo?pan='+p+'&tilt='+t);
}
function setSlider(p,t){document.getElementById('pan').value=p;document.getElementById('tilt').value=t;setServo();}
function flip(x,y){fetch('/flip?x='+x+'&y='+y);}
let testOn=false;
function testPattern(){testOn=!testOn;fetch('/test?on='+(testOn?1:0));
  document.getElementById('tbtn').textContent=testOn?'退出测试图':'显示方向测试图';}

/* ---------- WiFi ---------- */
function saveWifi(){
  const s=document.getElementById('w_ssid').value.trim(), p=document.getElementById('w_pass').value;
  if(!s){alert('请填 WiFi 名称');return;}
  fetch('/wifi?ssid='+encodeURIComponent(s)+'&pass='+encodeURIComponent(p))
    .then(()=>alert('已保存, 正在重连…\n「网络」页里的 IP 变了就说明通了'));
}

/* ---------- 状态轮询 (所有页面的 DOM 都在, 直接更新即可) ----------
   $ 做成"取不到就返回一个空壳": refresh() 里几十个字段, 万一哪个 id 打错或被
   删掉, 原生写法会抛异常并【中断整个刷新】—— 表现为整页数据全都不动, 很难查。
   这里降级成控制台一条警告, 其余字段照常刷新。 */
const NULL_EL={textContent:'',innerHTML:'',value:'',checked:false,
               style:{},classList:{toggle:function(){},add:function(){},remove:function(){}}};
const $=id=>document.getElementById(id)||(console.warn('元素缺失: '+id),NULL_EL);
let vadInit=false,volInit=false;
function refresh(){
  fetch('/status').then(r=>r.json()).then(d=>{
    const emoName=EMOS[d.emotion]||d.emotion;
    const netTxt=d.sta?('已联网 '+d.ip):'未联网';
    const lightTxt=(d.light>0?LIGHTS[d.light]:'关灯')+' · '+d.light_br+'%';
    const envTxt=d.env_valid?(d.temp.toFixed(1)+'° / '+d.hum.toFixed(0)+'%'):'--';
    const musTxt=d.mus||'空闲';

    /* --- 顶栏状态胶囊: 每一页都能看到, 不用回主页 --- */
    const pill=$('pill');
    pill.textContent=d.sta?(d.ai||'已联网'):'未联网';
    pill.className='pill'+(d.sta?' on':'');

    /* --- 主页面: 头像 / 概览 --- */
    $('hero_emo').textContent=emoName;
    $('hero_ai').textContent=d.sta?((d.ai||'--')+' · 音乐 '+musTxt):'未联网 —— 去「网络」页设置 WiFi';
    $('k_net').textContent=d.sta?'在线':'离线';
    $('k_mus').textContent=d.mus_playing?'播放中':(d.mus_on?'已暂停':'空闲');
    $('k_light').textContent=d.light>0?('亮度'+d.light_br):'关灯';
    $('k_env').textContent=d.env_valid?(d.temp.toFixed(1)+'°'):'--';

    /* --- 主页面: 功能入口的副标题(一眼看到每个模块的状态) --- */
    $('t_mus').textContent=d.mus_playing?'播放中':(d.mus_on?'已暂停':'空闲');
    $('t_light').textContent=d.light>0?LIGHTS[d.light]:'关灯';
    $('t_servo').textContent=d.pan+'°/'+d.tilt+'°';
    $('t_vol').textContent='音量 '+d.vol;
    $('t_flip').textContent=((d.flip_x?'左右翻 ':'')+(d.flip_y?'上下翻':''))||'正常';
    $('t_net').textContent=d.sta?'在线':'离线';

    /* --- 传感器 --- */
    $('touch_h').textContent=d.touch_head?'摸到':'无';
    $('ir_l').textContent=d.ir_left?'有手':'无';
    $('ir_r').textContent=d.ir_right?'有手':'无';
    $('track').textContent=d.tracking?'追踪中':'空闲';
    $('track_v').textContent=d.tracking?'追踪中':'空闲';
    $('mic_v').textContent=(d.mic|0)+'';
    $('mic_v2').textContent=(d.mic|0)+'';

    /* --- 网络页 --- */
    $('net_now').textContent=netTxt;
    $('net_ip').textContent=d.sta?d.ip:'--';

    /* --- 系统页 --- */
    $('act_v').textContent=d.act?d.act:'(已激活)';
    $('ai_v').textContent=d.ai||'--';
    $('wake_v').textContent=d.wake||'--';
    $('emo_v').textContent=emoName;
    $('tx_v').textContent=d.tx;
    $('rx_v').textContent=d.rx;
    $('pan_v').textContent=d.pan+'°';
    $('tilt_v').textContent=d.tilt+'°';

    /* --- 头部动作页 --- */
    $('pan').value=d.pan; $('tilt').value=d.tilt;
    $('pan_s').textContent=d.pan+'°'; $('tilt_s').textContent=d.tilt+'°';
    /* --- 表情页 --- */
    $('auto').checked=d.auto;
    /* --- 屏幕页 --- */
    $('flip_v').textContent=((d.flip_x?'左右翻 ':'')+(d.flip_y?'上下翻':''))||'正常';
    /* --- 灯光页 --- */
    $('lbr').value=d.light_br; $('lbr_v').textContent=d.light_br+'%';
    for(let i=0;i<LIGHTS.length;i++) $('L'+i).classList.toggle('on', i===d.light);
    /* --- 音乐页 --- */
    $('mus_v2').textContent=musTxt;
    /* --- 音量 / 开关 (只在第一次同步, 免得用户拖到一半被拉回去) --- */
    if(!volInit){ $('vol').value=d.vol; $('vol_v').textContent=d.vol; volInit=true; }
    $('wbeep').checked=!!d.wbeep;
    if(!vadInit){ $('vad').checked=!!d.vad; vadInit=true; }
    document.querySelectorAll('.emo-btn').forEach((b,i)=>b.classList.toggle('active',i===d.emotion));
  }).catch(()=>{});
}
/* 表情数量直接从表里数出来, 以后加表情不用改文案 */
$('t_emo').textContent=EMOS.length+' 种';

setInterval(refresh,500);
setInterval(refreshMusic,1500);
setInterval(refreshPlaylists,1500);
route();
refresh(); refreshMusic();
</script>
</body>
</html>
)HTML";
