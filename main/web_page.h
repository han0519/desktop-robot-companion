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
<title>小纸壳控制台</title>
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
/* 列表盒子: 以前只有 270px 高, 歌单多的时候看着像"只有 20 个"(其实能滚) ——
   改成按屏幕比例给高度, 并显式提示可以滑动 */
.mlist{margin-top:9px;max-height:min(46vh,380px);overflow-y:auto;-webkit-overflow-scrolling:touch;
       border:1px solid #232c4a;border-radius:8px;padding:3px}
.mlist::-webkit-scrollbar{width:5px}
.mlist::-webkit-scrollbar-thumb{background:#2f3a63;border-radius:3px}
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
/* 最后一行只有 1 个格子时会留空白 —— 长条版横向铺满整行 */
.tile.wide{grid-column:1 / -1;flex-direction:row;align-items:center;gap:12px;
           padding:10px 16px;text-align:left;justify-content:flex-start}
.tile.wide .ic{margin:0}
.tile.wide .tn{font-size:13px}
.tile.wide .ts{display:inline;margin:0 0 0 10px;height:auto}
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
      <div class="hn">小纸壳</div>
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
    <div class="tile" onclick="go('music')"><div class="ic">🎵</div><div class="tn">音乐</div><div class="ts" id="t_mus">--</div></div>
    <div class="tile" onclick="go('face')"><div class="ic">😊</div><div class="tn">表情</div><div class="ts" id="t_emo">52 种</div></div>
    <div class="tile" onclick="go('light')"><div class="ic">💡</div><div class="tn">灯环</div><div class="ts" id="t_light">--</div></div>
    <div class="tile" onclick="go('servo')"><div class="ic">🤖</div><div class="tn">动作</div><div class="ts" id="t_servo">云台角度</div></div>
    <div class="tile" onclick="go('sound')"><div class="ic">🔊</div><div class="tn">声音</div><div class="ts" id="t_vol">--</div></div>
    <div class="tile" onclick="go('screen')"><div class="ic">🖥️</div><div class="tn">屏幕</div><div class="ts" id="t_flip">方向</div></div>
    <div class="tile" onclick="go('net')"><div class="ic">🌐</div><div class="tn">网络</div><div class="ts" id="t_net">--</div></div>
    <div class="tile" onclick="go('dp')"><div class="ic">📊</div><div class="tn">数据页</div><div class="ts" id="t_dp">时间+温湿</div></div>
    <div class="tile wide" onclick="go('sys')"><div class="ic">⚙️</div><div class="tn">系统</div><div class="ts">固件诊断 · 重启 · 恢复出厂</div></div>
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
    说话前先喊「你好小智」, 或在「声音」里打开免触摸对话。
  </div>
</section>

<!-- ==================================================================== -->
<!-- ============================ 音    乐 ============================== -->
<!-- ==================================================================== -->
  <!-- ============================ 数据页 ============================== -->
  <section class="page" id="p-dp">
  <div class="card">
    <h2>这是什么</h2>
    <div class="tip">屏幕上<b>短按 BOOT</b> 在「表情页 ↔ 数据页」之间切换。
      数据页显示<b>当前时间</b>(SNTP 自动对时)和 <b>DHT11 温湿度</b>(实时刷新)。
      下面选一个布局样式, <b>点选立即推送</b>到设备并记住(存 NVS, 重启不丢)。
      布局长什么样可以到 <b>docs/data_preview.html</b> 预览页里先看效果。</div>
  </div>
  <div class="card">
    <h2>布局样式</h2>
    <div class="row" style="flex-wrap:wrap;gap:6px">
      <div class="cbtn" onclick="dpStyle(0)">A 紧凑三行</div>
      <div class="cbtn" onclick="dpStyle(1)">B 大时钟</div>
      <div class="cbtn" onclick="dpStyle(2)">C 仪表盘</div>
      <div class="cbtn" onclick="dpStyle(3)">D 环境监控</div>
      <div class="cbtn" onclick="dpStyle(4)">E 极简居中</div>
    </div>
  </div>
  </section>

<section class="page" id="p-music">
  <div class="card" id="necard">
    <h2>网易云登录</h2>
    <div class="row" style="margin-bottom:8px"><span class="label">状态</span><span class="val" id="ne_st">未登录</span></div>
    <div id="ne_qrwrap" style="display:none;text-align:center;margin:6px 0 10px">
      <div id="ne_qrbox" style="display:inline-block;position:relative">
        <div id="ne_qr" style="background:#fff;padding:8px;border-radius:8px;color:#333;font-size:12px"></div>
        <div id="ne_qrs" style="display:none;position:absolute;left:8px;right:8px;top:8px;bottom:8px;
             background:rgba(10,14,30,.88);border-radius:8px;display:flex;flex-direction:column;
             align-items:center;justify-content:center;gap:4px">
          <div id="ne_qrs_ic" style="font-size:26px;line-height:1">✓</div>
          <div id="ne_qrs_tx" style="font-size:12px;font-weight:600"></div>
        </div>
      </div>
      <div style="font-size:11.5px;color:#8b95bb;margin-top:6px">用「网易云音乐」APP 右上角扫一扫</div>
      <div style="font-size:10.5px;color:#5a6488;margin-top:3px" id="ne_key"></div>
    </div>
    <div class="row" style="margin-bottom:6px">
      <button class="cbtn on" style="flex:1;padding:8px 0" id="ne_btn" onclick="neLogin()">扫码登录</button>
      <button class="cbtn" style="flex:0 0 64px;padding:8px 0"
              onclick="if(confirm('确定要退出网易云登录吗？(需要重新扫码)'))neLogout()">退出</button>
    </div>
    <div class="row">
      <input type="text" id="ne_ck" placeholder="或粘贴 Cookie: MUSIC_U=..." style="flex:1;font-size:11px">
      <button class="cbtn" style="flex:0 0 56px" onclick="nePasteCookie()">保存</button>
    </div>
    <div class="tip">扫码后手机上点「确认登录」即可，Cookie 存在设备里，换设备要重新扫。<br>
      不方便扫码时：浏览器 F12 → Application → Cookies → music.163.com，把 <b>MUSIC_U</b>（可连 __csrf 一起）粘到上面保存。</div>
  </div>
  <div class="card">
    <h2>搜索 / 点播</h2>
    <div class="row" style="margin-bottom:6px">
      <input type="text" id="msong" placeholder="歌名 或 歌手+歌名" style="flex:1"
             onkeydown="if(event.key==='Enter')musicSearch()">
      <button class="cbtn on" style="flex:0 0 58px;padding:8px 0" onclick="musicSearch()">搜索</button>
      <button class="cbtn" style="flex:0 0 58px;padding:8px 0" onclick="musicSearchArtist()">搜歌手</button>
      <button class="cbtn" style="flex:0 0 58px;padding:8px 0" onclick="musicPlayKw()">直接播</button>
    </div>
    <div class="row" style="margin-bottom:6px">
      <button class="cbtn" style="flex:1;padding:8px 0" onclick="loadPlaylists()">我的歌单</button>
      <button class="cbtn" style="flex:0 0 74px;padding:8px 0" onclick="hidePlaylists()">收起</button>
    </div>
    <!-- 我的歌单(账号里的歌单) -->
    <div class="row" style="font-size:11.5px;color:#8b95bb;margin:2px 0 4px;display:none" id="pl_hd"></div>
    <div class="mlist" id="plist" style="display:none;margin-bottom:10px"></div>
    <!-- 曲目列表(搜索结果 或 某个歌单): 点哪首播哪首 -->
    <div class="row" style="font-size:11.5px;color:#8b95bb;margin:0 0 5px" id="src_v">曲目列表</div>
    <div class="mlist" id="mlist"></div>
    <div class="tip" id="ml_hint" style="margin-top:6px">点「我的歌单」→ 点某个歌单的<b>「打开」</b>，
      它下面的<b>曲目列表</b>就会装进那个歌单的歌，点歌名即播放。</div>
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
    <h2>音色 <span class="v" id="voice_v" style="font-size:12px;color:#8b95bb"></span></h2>
    <div class="tip" style="margin-bottom:7px">设备端实时变声(xiaozhi.me 的官方音色在它控制台配)。
      切换后下一句播报立即生效, 断电记忆。说「用对讲机音说话」也行。</div>
    <div class="grid g3" id="voice_btns">
      <div class="cbtn" onclick="setVoice('original')">原声</div>
      <div class="cbtn" onclick="setVoice('metal')">机械金属</div>
      <div class="cbtn" onclick="setVoice('deep')">电子低沉</div>
      <div class="cbtn" onclick="setVoice('alien')">赛博外星</div>
      <div class="cbtn" onclick="setVoice('elec')">电流音</div>
      <div class="cbtn" onclick="setVoice('strong')">强电流</div>
      <div class="cbtn" onclick="setVoice('radio')">对讲机</div>
      <div class="cbtn" onclick="setVoice('space')">深空回声</div>
      <div class="cbtn" onclick="setVoice('elec2')">电流音2</div>
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
/* ==== 内嵌二维码生成器(经与参考实现逐模块比对, v1~v10 一致) ==== */
/* 精简 QR 编码器 — byte 模式 / 版本 1~10 / 纠错等级 M
   只为在设备网页里画网易云登录二维码, 不依赖任何外部库。 */
var QRMini = (function () {
  var EXP = new Uint8Array(256), LOG = new Uint8Array(256);
  (function () { var x = 1; for (var i = 0; i < 255; i++) { EXP[i] = x; LOG[x] = i; x = (x << 1) ^ (x & 0x80 ? 0x11d : 0); } EXP[255] = 1; })();
  function mul(a, b) { return (a === 0 || b === 0) ? 0 : EXP[(LOG[a] + LOG[b]) % 255]; }

  /* 每版本: 每块纠错码字数 ec, 块结构 [[块数, 每块数据码字数], ...] */
  var SPEC = {
    1:  { ec: 10, g: [[1, 16]] },
    2:  { ec: 16, g: [[1, 28]] },
    3:  { ec: 26, g: [[1, 44]] },
    4:  { ec: 18, g: [[2, 32]] },
    5:  { ec: 24, g: [[2, 43]] },
    6:  { ec: 16, g: [[4, 27]] },
    7:  { ec: 18, g: [[4, 31]] },
    8:  { ec: 22, g: [[2, 38], [2, 39]] },
    9:  { ec: 22, g: [[3, 36], [2, 37]] },
    10: { ec: 26, g: [[4, 43], [1, 44]] }
  };
  var ALIGN = { 1: [], 2: [6,18], 3: [6,22], 4: [6,26], 5: [6,30], 6: [6,34],
                7: [6,22,38], 8: [6,24,42], 9: [6,26,46], 10: [6,28,50] };

  function genPoly(n) {
    var p = [1];
    for (var i = 0; i < n; i++) {
      var q = p.slice(); q.push(0);
      for (var j = 0; j < p.length; j++) q[j + 1] ^= mul(p[j], EXP[i]);
      p = q;
    }
    return p;
  }
  function rs(data, ecLen) {
    var g = genPoly(ecLen), res = data.slice();
    for (var i = 0; i < ecLen; i++) res.push(0);
    for (i = 0; i < data.length; i++) {
      var c = res[i];
      if (c !== 0) for (var j = 0; j < g.length; j++) res[i + j] ^= mul(g[j], c);
    }
    return res.slice(data.length);
  }
  function utf8(s) {
    var b = [];
    for (var i = 0; i < s.length; i++) {
      var c = s.charCodeAt(i);
      if (c < 0x80) b.push(c);
      else if (c < 0x800) { b.push(0xc0 | (c >> 6), 0x80 | (c & 63)); }
      else { b.push(0xe0 | (c >> 12), 0x80 | ((c >> 6) & 63), 0x80 | (c & 63)); }
    }
    return b;
  }
  function totalData(v) { var t = 0; SPEC[v].g.forEach(function (g) { t += g[0] * g[1]; }); return t; }

  function encode(text) {
    var bytes = utf8(text), ver = 0;
    for (var v = 1; v <= 10; v++) {
      var cntBits = v < 10 ? 8 : 16;
      if (totalData(v) * 8 >= 4 + cntBits + bytes.length * 8) { ver = v; break; }
    }
    if (!ver) return null;
    var cap = totalData(ver) * 8, bits = [];
    function put(val, len) { for (var i = len - 1; i >= 0; i--) bits.push((val >> i) & 1); }
    put(4, 4);
    put(bytes.length, ver < 10 ? 8 : 16);
    for (var i = 0; i < bytes.length; i++) put(bytes[i], 8);
    for (i = 0; i < 4 && bits.length < cap; i++) bits.push(0);
    while (bits.length % 8) bits.push(0);
    var cws = [];
    for (i = 0; i < bits.length; i += 8) {
      var by = 0;
      for (var j = 0; j < 8; j++) by = (by << 1) | bits[i + j];
      cws.push(by);
    }
    var pad = [0xEC, 0x11], pi = 0;
    while (cws.length < totalData(ver)) cws.push(pad[pi++ % 2]);

    var blocks = [], ecs = [], pos = 0;
    SPEC[ver].g.forEach(function (g) {
      for (var k = 0; k < g[0]; k++) {
        var dd = cws.slice(pos, pos + g[1]); pos += g[1];
        blocks.push(dd); ecs.push(rs(dd, SPEC[ver].ec));
      }
    });
    var maxD = 0; blocks.forEach(function (b) { if (b.length > maxD) maxD = b.length; });
    var out = [];
    for (i = 0; i < maxD; i++) blocks.forEach(function (b) { if (i < b.length) out.push(b[i]); });
    for (i = 0; i < SPEC[ver].ec; i++) ecs.forEach(function (e) { out.push(e[i]); });
    return { ver: ver, codewords: out };
  }

  function versionBits(v) {
    var bch = v << 12;
    for (var i = 17; i >= 12; i--) if ((bch >>> i) & 1) bch ^= 0x1f25 << (i - 12);
    return (v << 12) | bch;
  }
  function formatBits(ec, mask) {          /* ec: L=1 M=0 Q=3 H=2 */
    var data = ((ec & 3) << 3) | mask, bch = data << 10;
    for (var i = 14; i >= 10; i--) if ((bch >>> i) & 1) bch ^= 0x537 << (i - 10);
    return (((data << 10) | bch) ^ 0x5412) & 0x7fff;
  }
  function placeFormat(m, fn, fmt, size) {
    var i;
    /* ★ 位序: 高位(bit14)放在序列的第一个位置 —— 放反了会差 8 个模块 */
    for (i = 0; i <= 5; i++) m[8][i] = (fmt >> (14 - i)) & 1;
    m[8][7] = (fmt >> 8) & 1;
    m[8][8] = (fmt >> 7) & 1;
    m[7][8] = (fmt >> 6) & 1;
    for (i = 9; i < 15; i++) m[14 - i][8] = (fmt >> (14 - i)) & 1;
    for (i = 0; i < 7; i++) m[size - 1 - i][8] = (fmt >> (14 - i)) & 1;
    for (i = 7; i < 15; i++) m[8][size - 15 + i] = (fmt >> (14 - i)) & 1;
    m[size - 8][8] = 1;
  }
  function applyMask(m, fn, k, size) {
    for (var y = 0; y < size; y++) for (var x = 0; x < size; x++) {
      if (fn[y][x]) continue;
      var v;
      switch (k) {
        case 0: v = (y + x) % 2 === 0; break;
        case 1: v = y % 2 === 0; break;
        case 2: v = x % 3 === 0; break;
        case 3: v = (y + x) % 3 === 0; break;
        case 4: v = (Math.floor(y / 2) + Math.floor(x / 3)) % 2 === 0; break;
        case 5: v = ((y * x) % 2) + ((y * x) % 3) === 0; break;
        case 6: v = (((y * x) % 2) + ((y * x) % 3)) % 2 === 0; break;
        default: v = (((y + x) % 2) + ((y * x) % 3)) % 2 === 0; break;
      }
      if (v) m[y][x] ^= 1;
    }
  }
  function penalty(m, size) {
    var p = 0, i, j, run, last;
    for (i = 0; i < size; i++) {
      run = 1; last = m[i][0];
      for (j = 1; j < size; j++) {
        if (m[i][j] === last) run++;
        else { if (run >= 5) p += 3 + (run - 5); last = m[i][j]; run = 1; }
      }
      if (run >= 5) p += 3 + (run - 5);
    }
    for (j = 0; j < size; j++) {
      run = 1; last = m[0][j];
      for (i = 1; i < size; i++) {
        if (m[i][j] === last) run++;
        else { if (run >= 5) p += 3 + (run - 5); last = m[i][j]; run = 1; }
      }
      if (run >= 5) p += 3 + (run - 5);
    }
    for (i = 0; i < size - 1; i++) for (j = 0; j < size - 1; j++) {
      var v = m[i][j];
      if (v === m[i][j + 1] && v === m[i + 1][j] && v === m[i + 1][j + 1]) p += 3;
    }
    var p1 = [1,0,1,1,1,0,1,0,0,0,0], p2 = [0,0,0,0,1,0,1,1,1,0,1];
    function hit(a, off, pat) { for (var k = 0; k < 11; k++) if (a[off + k] !== pat[k]) return false; return true; }
    for (i = 0; i < size; i++) for (j = 0; j + 11 <= size; j++)
      if (hit(m[i], j, p1) || hit(m[i], j, p2)) p += 40;
    for (j = 0; j < size; j++) {
      var col = []; for (i = 0; i < size; i++) col.push(m[i][j]);
      for (i = 0; i + 11 <= size; i++) if (hit(col, i, p1) || hit(col, i, p2)) p += 40;
    }
    var dark = 0;
    for (i = 0; i < size; i++) for (j = 0; j < size; j++) dark += m[i][j];
    var pct = dark * 100 / (size * size);
    p += Math.floor(Math.abs(pct - 50) / 5) * 10;
    return p;
  }

  function build(text) {
    var e = encode(text);
    if (!e) return null;
    var ver = e.ver, size = ver * 4 + 17, i, j;
    var m = [], fn = [];
    for (i = 0; i < size; i++) { m.push(new Uint8Array(size)); fn.push(new Uint8Array(size)); }
    function setFn(y, x, v) { m[y][x] = v; fn[y][x] = 1; }
    function finder(r, c) {
      for (var dy = -1; dy <= 7; dy++) for (var dx = -1; dx <= 7; dx++) {
        var y = r + dy, x = c + dx;
        if (y < 0 || x < 0 || y >= size || x >= size) continue;
        var dd = Math.max(Math.abs(dy - 3), Math.abs(dx - 3));
        setFn(y, x, (dd !== 2 && dd <= 3) ? 1 : 0);
      }
    }
    finder(0, 0); finder(0, size - 7); finder(size - 7, 0);
    for (i = 8; i < size - 8; i++) { setFn(6, i, i % 2 === 0 ? 1 : 0); setFn(i, 6, i % 2 === 0 ? 1 : 0); }
    var ap = ALIGN[ver];
    for (i = 0; i < ap.length; i++) for (j = 0; j < ap.length; j++) {
      var cy = ap[i], cx = ap[j];
      if ((cy === 6 && cx === 6) || (cy === 6 && cx === size - 7) || (cy === size - 7 && cx === 6)) continue;
      for (var dy2 = -2; dy2 <= 2; dy2++) for (var dx2 = -2; dx2 <= 2; dx2++)
        setFn(cy + dy2, cx + dx2, Math.max(Math.abs(dy2), Math.abs(dx2)) !== 1 ? 1 : 0);
    }
    for (i = 0; i <= 8; i++) { if (i !== 6) { setFn(8, i, 0); setFn(i, 8, 0); } }
    for (i = 0; i < 8; i++) { setFn(8, size - 1 - i, 0); setFn(size - 1 - i, 8, 0); }
    setFn(size - 8, 8, 1);
    if (ver >= 7) {
      var vi = versionBits(ver);
      for (i = 0; i < 18; i++) {
        var b = (vi >> i) & 1, r2 = Math.floor(i / 3), c2 = size - 11 + (i % 3);
        setFn(r2, c2, b); setFn(c2, r2, b);
      }
    }
    var idx = 0, total = e.codewords.length * 8;
    for (var right = size - 1; right >= 1; right -= 2) {
      if (right === 6) right = 5;
      for (var vert = 0; vert < size; vert++) {
        var up = ((right + 1) & 2) === 0;
        for (var k = 0; k < 2; k++) {
          var x2 = right - k, y2 = up ? size - 1 - vert : vert;
          if (fn[y2][x2]) continue;
          var bit = 0;
          if (idx < total) bit = (e.codewords[idx >> 3] >> (7 - (idx & 7))) & 1;
          m[y2][x2] = bit; idx++;
        }
      }
    }
    var best = 0, bestScore = 1e9, bestM = null;
    for (var mk = 0; mk < 8; mk++) {
      var mm = m.map(function (row) { return row.slice(); });
      applyMask(mm, fn, mk, size);
      placeFormat(mm, fn, formatBits(0, mk), size);   /* 0 = 纠错等级 M */
      var sc = penalty(mm, size);
      if (sc < bestScore) { bestScore = sc; best = mk; bestM = mm; }
    }
    return { size: size, ver: ver, mask: best, modules: bestM, fn: fn, codewords: e.codewords };
  }
  return { build: build, encode: encode };
})();
if (typeof module !== 'undefined' && module.exports) module.exports = QRMini;




</script>
<script>
/* ==================================================================
 * 路由: #/home 是主页面, 其余是子页面。用 location.hash 而不是多文件,
 * 好处是无请求瞬时切换 + 浏览器/安卓返回键天然可用。
 * ================================================================== */
const PAGES={home:'控制台', music:'网易云音乐', face:'表情', light:'灯环',
             servo:'头部动作', sound:'声音', screen:'屏幕', net:'网络', sys:'系统', dp:'数据页'};
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
const LIGHTS=["关灯","单色常亮","呼吸","彩虹环","彩虹呼吸","追光","双点对撞","镜像呼吸","脉冲扩散","火焰","星空闪烁","电平环","音乐律动","音乐频谱","警车爆闪"];
const lgrid=document.getElementById('lightGrid');
LIGHTS.forEach((n,i)=>{const b=document.createElement('div');
  b.className='cbtn';b.id='L'+i;b.textContent=n;b.onclick=()=>setLight(i);lgrid.appendChild(b);});
function setLight(i){ fetch('/light?effect='+i); }
const DP_NAMES=['A 紧凑三行','B 大时钟','C 仪表盘','D 环境监控','E 极简居中'];
function dpStyle(n){
  fetch('/datapage?style='+n).then(r=>r.json()).then(d=>{
    const el=document.getElementById('t_dp');
    if(el) el.textContent = DP_NAMES[d.style] || ('样式 '+d.style);
    localStorage.setItem('dpstyle', d.style);
  }).catch(()=>{});
}
function dpStylePage(){ go('dp'); }
document.getElementById('t_dp').textContent =
  DP_NAMES[localStorage.getItem('dpstyle')||0] || '时间+温湿';
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
/* ★ 按歌手名搜歌: 搜到歌手 → 热门歌曲 30 首装进下面的曲目列表, 点歌名即播 */
function musicSearchArtist(){
  const s=document.getElementById('msong').value.trim();
  if(!s){ alert('请输入歌手名'); return; }
  document.getElementById('mlist').innerHTML='<div class="mitem" style="cursor:default;color:#6b76a0">正在搜索歌手「'+esc(s)+'」的热门歌曲…（约几秒）</div>';
  mlistHTML='';
  fetch('/music?artist='+encodeURIComponent(s));
}
function playId(id){ fetch('/music?play='+encodeURIComponent(id)); }
function musicCtl(c){ fetch('/music?ctl='+c); }
function saveMusicBase(){
  const b=document.getElementById('mbase').value.trim();
  if(!b){ alert('请输入代理地址, 例: http://192.168.1.10:3000'); return; }
  fetch('/music?serve='+encodeURIComponent(b)).then(()=>alert('已保存'));
}
/* ---------- 我的歌单 ---------- */
let plHTML='', plShown=false, lastSrc='';
function loadPlaylists(){
  plShown=true;
  const box=document.getElementById('plist');
  box.style.display='block';
  box.innerHTML='<div class="mitem" style="cursor:default;color:#6b76a0">加载中…（歌单多时要 10~20 秒）</div>';
  const hd=document.getElementById('pl_hd');
  hd.style.display='block'; hd.textContent='正在读取你的歌单…';
  plHTML='';
  fetch('/music?plsreq=1');     /* 触发一次; 结果由 /music?pls=1 轮询读取 */
}
function hidePlaylists(){
  plShown=false;
  document.getElementById('plist').style.display='none';
  document.getElementById('pl_hd').style.display='none';
}
/* 打开某个歌单: 把它的歌装进曲目列表, 之后就能点播。
   传元素本身(能从 data-nm 拿到歌单名, 名字里有引号也不会出错), 也兼容直接传 id。 */
function openPlaylist(el){
  const id   = (typeof el==='string') ? el : (el.dataset.pid||'');
  const name = (typeof el==='string') ? ''   : (el.dataset.nm||'');
  const ml=document.getElementById('mlist');
  ml.innerHTML='<div class="mitem" style="cursor:default;color:#6b76a0">正在读取'
              +(name?'「'+esc(name)+'」':'歌单')+'…（几秒, 请稍候）</div>';
  mlistHTML='';
  /* ★ 把曲目列表滚到眼前: 以前点完页面一动不动, 用户以为"没反应" */
  ml.scrollIntoView({block:'center'});
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
      return '<div class="mitem" data-pid="'+it.id+'" data-nm="'+esc(it.name)+'" onclick="openPlaylist(this)">'
           + '<span class="no">'+it.count+'</span>'
           + '<span class="nm">'+esc(it.name)+'</span>'
           + '<span class="pl">打开</span></div>';
    }).join('');
    /* 明确告诉用户一共多少个(以前只显示一屏, 看着像"只有 20 个") */
    const hd=document.getElementById('pl_hd');
    hd.style.display='block';
    hd.textContent='我的歌单 · 已载入 '+d.count+' 个'
                  +(d.count>=20?'（只取前 20 个，省流量）':'')
                  +'　点「打开」装进下面的曲目列表';
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
      d.src ? ('曲目列表 · 来自「'+d.src+'」共 '+d.count+' 首（点歌名即播放）') : '曲目列表';
    /* ★ 来源变了(刚打开一个歌单) → 滚到眼前并闪一下边框, 让用户确实看到 */
    if(d.src && d.src!==lastSrc){
      lastSrc=d.src;
      box.scrollIntoView({block:'center'});
      box.style.borderColor='#00d4ff';
      setTimeout(function(){ box.style.borderColor=''; },1500);
    }
  }).catch(function(){});
}

/* ---------- 声音 ---------- */
function setVol(v){ document.getElementById('vol_v').textContent=v; mcp('self.audio_speaker.set_volume',{volume:parseInt(v)}); }
function beep(){ fetch('/beep?f=1000&ms=250'); }
function setWakeBeep(on){ fetch('/wakebeep?on='+(on?1:0)); }
function setVad(on){ fetch('/vad?on='+(on?1:0)); }
/* 让设备自己播一段测试语音(不经过服务端), 用来单独验证"能不能出声" */
function ttsTest(){ fetch('/ttstest?sec=3'); }

/* ---------- 音色(设备端 DSP 变声) ---------- */
const VOICE_IDS=['original','metal','deep','alien','elec','strong','radio','space','elec2'];
function markVoice(v){
  document.querySelectorAll('#voice_btns .cbtn').forEach(function(b,i){
    b.style.borderColor = (VOICE_IDS[i]===v) ? '#00d4ff' : '';
  });
}
function setVoice(v){
  mcp('self.audio_voice.set',{voice:v}).then(function(t){
    const m=(t||'').match(/音色已切换: ([^"\\]+)/);
    if(m) document.getElementById('voice_v').textContent='当前: '+m[1];
    markVoice(v);
  });
}
/* 进声音页时同步一次当前音色 */
mcp('self.audio_voice.get').then(function(t){
  const m=(t||'').match(/当前音色: ([^"\\]+)/);
  if(m){
    document.getElementById('voice_v').textContent='当前: '+m[1];
    const map={'原声':'original','机械金属':'metal','电子低沉':'deep','赛博外星':'alien',
               '电流音':'elec','强电流':'strong','对讲机':'radio','深空回声':'space','电流音2':'elec2'};
    if(map[m[1]]) markVoice(map[m[1]]);
  }
});

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
    $('k_env').textContent=d.env_valid?(d.temp.toFixed(0)+'° '+d.hum.toFixed(0)+'%'):'--';

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
/* ==================== 网易云扫码登录 ====================
   流程完全在设备本地: 设备向网易云要 unikey → 这里画二维码 → 手机扫 →
   设备后台轮询到 803 → Cookie 存进 NVS。全程不需要任何服务器。
   （二维码是页面里【内嵌】的生成器画的 —— 不依赖任何 CDN/外部请求,
     所以在"设备热点模式"(没有外网)下也能用。） */
let NE_TIMER=null, NE_LASTKEY='';
function neRender(k){
  const box=$('ne_qr');
  if(!k){ box.innerHTML=''; $('ne_qrwrap').style.display='none'; NE_LASTKEY=''; return; }
  $('ne_qrwrap').style.display='block';
  if(k===NE_LASTKEY) return;
  NE_LASTKEY=k; box.innerHTML='';
  try{
    const qr=QRMini.build('https://music.163.com/login?codekey='+k);
    if(!qr){ box.innerHTML='二维码生成失败'; return; }
    const s=Math.max(3,Math.floor(172/qr.size)), pad=8;
    const cv=document.createElement('canvas');
    cv.width=cv.height=qr.size*s+pad*2;
    const g=cv.getContext('2d');
    g.fillStyle='#fff'; g.fillRect(0,0,cv.width,cv.height);
    g.fillStyle='#000';
    for(let y=0;y<qr.size;y++) for(let x=0;x<qr.size;x++)
      if(qr.modules[y][x]) g.fillRect(pad+x*s,pad+y*s,s,s);
    box.appendChild(cv);
  }catch(e){ box.innerHTML='二维码生成失败: '+e.message; }
}
/* 二维码浮层: 801 等待(不盖)/802 已扫码(盖上)/800 过期(盖上)/成功(绿色盖上) */
function neOverlay(state, tx, color){
  const ov=$('ne_qrs');
  if(!ov) return;
  if(!state){ ov.style.display='none'; return; }
  ov.style.display='flex';
  ov.style.background='rgba(10,14,30,.88)';
  $('ne_qrs_ic').textContent = state==='ok' ? '✓' : '📱';
  $('ne_qrs_ic').style.color = color;
  $('ne_qrs_tx').textContent = tx;
  $('ne_qrs_tx').style.color = color;
}
async function nePoll(){
  try{
    const d=await (await fetch('/nlogin')).json();
    if(d.login){
      $('ne_st').textContent='已登录: '+(d.nick||'')+' · 音乐功能已可用';
      $('ne_st').style.color='#3ddc84';
      $('ne_btn').textContent='已登录';
      neOverlay('ok','登录成功 · 音乐已可用','#3ddc84');
      /* 成功画面停 2.5 秒再收起二维码, 让用户确实看到"成功了" */
      setTimeout(function(){ if($('ne_btn').textContent==='已登录') neRender(''); }, 2500);
      if(NE_TIMER){ clearInterval(NE_TIMER); NE_TIMER=null; }
      return;
    }
    $('ne_st').style.color='';
    /* 设备会去网易云验证 Cookie 真伪: ckok=0 表示"存着但已失效", 必须明说,
       否则用户会像之前那样看着"已登录"却怎么都搜不到歌。 */
    if(d.ckok===0){
      $('ne_st').textContent='登录已失效 —— 网易云不认这张 Cookie, 请重新登录';
      neOverlay('',''); 
    } else if(d.code===802){
      $('ne_st').textContent='已扫码 · 请在手机上点「确认登录」';
      neOverlay('scan','请在手机上确认授权','#ffd54a');
    } else if(d.code===800){
      $('ne_st').textContent='二维码已过期, 正在自动换一张…';
      neOverlay('exp','二维码已过期 · 自动换新','#ff8a80');
    } else {
      $('ne_st').textContent =
        d.code===801 ? '等待扫码…' :
        d.code===-1  ? '取二维码失败, 正在自动重试…' : '正在获取二维码…';
      neOverlay('','');
    }
    /* 显示二维码 ID 前 8 位: 出问题时能一眼看出"你扫的"和"设备在轮的"是不是同一张 */
    $('ne_key').textContent = d.key ? ('二维码 ID ' + d.key.slice(0,8) + (d.busy?' · 查询中':'')) : '';
    neRender(d.key||'');
  }catch(e){}
}
function neLogin(){
  if($('ne_btn').textContent==='已登录') return;
  $('ne_btn').textContent='获取中…';
  /* ★ 只有点这个按钮才会去网易云取新码; 平时的 nePoll 只是读设备缓存的状态,
     一次网络都不打 —— 否则开着页面就等于让设备每 2 秒做一次 TLS 握手。 */
  fetch('/nlogin?new=1').then(()=>{ $('ne_btn').textContent='刷新二维码'; nePoll(); });
  neStartTimer();
}
/* 页面切到后台就停止轮询(省设备资源); ★ 切回来必须恢复 ——
   否则"切去别的标签页再回来"会导致页面卡在一张【过期的二维码】上:
   设备那边已经换了新 key, 页面却还在显示旧的, 扫了当然没反应。 */
function neStartTimer(){ if(!NE_TIMER) NE_TIMER=setInterval(nePoll,2000); }
document.addEventListener('visibilitychange',function(){
  if(document.hidden){
    if(NE_TIMER){ clearInterval(NE_TIMER); NE_TIMER=null; }
  } else if($('ne_qrwrap').style.display==='block' && $('ne_btn').textContent!=='已登录'){
    neStartTimer(); nePoll();
  }
});
async function neLogout(){
  if(NE_TIMER){ clearInterval(NE_TIMER); NE_TIMER=null; }
  await fetch('/nlogin?logout=1');
  NE_LASTKEY=''; neRender('');
  $('ne_btn').textContent='扫码登录';
  $('ne_st').textContent='已退出登录';
}
async function nePasteCookie(){
  const v=($('ne_ck').value||'').trim();
  if(!v){ $('ne_st').textContent='请先粘贴 Cookie'; return; }
  try{
    const r=await (await fetch('/nlogin',{method:'POST',body:'cookie='+encodeURIComponent(v)})).json();
    $('ne_ck').value='';
    if(r.login){ $('ne_st').textContent='已登录: '+(r.nick||''); $('ne_btn').textContent='已登录'; neRender(''); }
    else if(r.ckok===0) $('ne_st').textContent='Cookie 已失效或无效 —— 网易云账号接口不认它';
    else $('ne_st').textContent='Cookie 无效(必须含 MUSIC_U)';
  }catch(e){ $('ne_st').textContent='保存失败'; }
}

setInterval(refresh,500);
setInterval(refreshMusic,1500);
setInterval(refreshPlaylists,1500);
route();
refresh(); refreshMusic();
nePoll();
</script>
</body>
</html>
)HTML";

