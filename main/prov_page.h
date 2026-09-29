/*
 * prov_page.h — 配网专用页面
 *
 * 只在"设备还没配过网"时显示(此时它开热点, 手机连上来看到的就是这个页面)。
 * 风格对齐小智官方: 深色底 + 青色主色(#58d5ff) + 激活码突出显示
 * —— 激活码必须一眼看到, 用户要用它去 xiaozhi.me 添加设备。
 *
 * 配好网之后访问 /prov 也能看到它(用于重新配网), 平时根目录是控制台页面。
 */
#pragma once

static const char *PROV_PAGE = R"HTML(
<!DOCTYPE html>
<html lang="zh">
<head>
<meta charset="UTF-8">
<meta name="viewport" content="width=device-width,initial-scale=1">
<title>喵伴 · 设备配网</title>
<style>
*{box-sizing:border-box}
body{margin:0;min-height:100vh;background:#0d1117;color:#e6edf3;
  font-family:-apple-system,BlinkMacSystemFont,"PingFang SC","Microsoft YaHei",sans-serif;
  display:flex;align-items:center;justify-content:center;padding:20px}
.box{width:100%;max-width:420px;text-align:center}
.logo{font-size:46px;line-height:1;margin-bottom:8px}
h1{font-size:20px;font-weight:600;margin:0 0 6px}
.sub{font-size:13px;color:#8b949e;margin-bottom:20px}
.code{background:#161b22;border:1px dashed #30363d;border-radius:12px;padding:16px;margin-bottom:16px}
.code .k{font-size:12px;color:#8b949e;margin-bottom:8px}
.code .v{font-size:30px;font-weight:700;letter-spacing:6px;color:#58d5ff;
  font-family:ui-monospace,Menlo,Consolas,monospace}
.form{background:#161b22;border:1px solid #30363d;border-radius:12px;padding:18px;text-align:left}
label{font-size:12px;color:#8b949e;display:block;margin:12px 0 6px}
label:first-child{margin-top:0}
input{width:100%;padding:12px;border-radius:8px;border:1px solid #30363d;
  background:#0d1117;color:#e6edf3;font-size:15px}
input:focus{outline:none;border-color:#58d5ff}
button{margin-top:18px;width:100%;padding:13px;border:none;border-radius:8px;
  background:#58d5ff;color:#05121b;font-size:16px;font-weight:600;cursor:pointer}
button:active{opacity:.85}
.msg{margin-top:14px;font-size:13px;min-height:20px;color:#58d5ff}
.hint{font-size:12px;color:#8b949e;margin-top:16px;line-height:1.7}
.card{background:#161b22;border:1px solid #30363d;border-radius:12px;padding:14px;
  text-align:left;font-size:13px;margin-bottom:16px}
.card .r{display:flex;justify-content:space-between;padding:3px 0}
.card .r span:last-child{color:#58d5ff}
.aplist{max-height:190px;overflow:auto;border:1px solid #30363d;border-radius:8px;background:#0d1117}
.ap{display:flex;justify-content:space-between;align-items:center;padding:11px 12px;font-size:14px;
  border-bottom:1px solid #21262d;cursor:pointer}
.ap:last-child{border-bottom:none}
.ap:active{background:#1f2937}
.ap .lvl{font-size:11px;color:#8b949e;flex:0 0 auto;margin-left:8px}
.apm{padding:12px;font-size:13px;color:#8b949e}
.rescan{margin-top:8px;width:100%;padding:9px;border-radius:8px;border:1px solid #30363d;
  background:#0d1117;color:#8b949e;font-size:13px;font-weight:400;cursor:pointer}
</style>
</head>
<body>
<div class="box">
  <div class="logo">🐱</div>
  <h1>喵伴 · 设备配网</h1>
  <div class="sub">连接你家 Wi-Fi，就能开始和小智对话</div>

  <div class="code">
    <div class="k">激活码 · 去 xiaozhi.me 添加设备时用（连上网后服务端才下发）</div>
    <div class="v" id="act">--</div>
  </div>

  <div class="card">
    <div class="r"><span>设备状态</span><span id="st">--</span></div>
    <div class="r"><span>已配网</span><span id="pvd">--</span></div>
  </div>

  <div class="form">
    <label>选择你家的 Wi-Fi（点一下自动填入）</label>
    <div class="aplist" id="aplist">
      <div class="apm" id="apmsg">正在扫描周围的 Wi-Fi…</div>
    </div>
    <button class="rescan" onclick="scan()">重新扫描</button>
    <label>Wi-Fi 名称</label>
    <input id="s" placeholder="点上面列表自动填入, 也可手打" autocomplete="off">
    <label>Wi-Fi 密码</label>
    <input id="p" type="password" placeholder="没有密码请留空">
    <button onclick="save()">连接 Wi-Fi</button>
    <div class="msg" id="m"></div>
  </div>

  <div class="hint">
    仅支持 2.4GHz Wi-Fi<br>
    提交后设备会尝试连接，成功后自动进入对话模式<br>
    连不上会自动回到本页，可重新填写
  </div>
</div>

<script>
var t=setInterval(poll,2000);
function poll(){
  fetch('/status').then(function(r){return r.json();}).then(function(d){
    /* ★ 空激活码 ≠ 已激活! 只表示"服务端还没下发"(比如还没连上网)。
       以前显示"（已激活）", 让新板子看起来已经激活了 —— 就是这个误导。 */
    document.getElementById('act').textContent=(d.act&&d.act!='')?d.act:'—（未下发）';
    document.getElementById('st').textContent=d.ai||'--';
    document.getElementById('pvd').textContent=(d.ip&&d.ip!='')?(d.ip):'未连接';
    if(d.ip&&d.ip!=''){ clearInterval(t); document.getElementById('m').textContent='已联网：'+d.ip; }
  }).catch(function(){});
}
poll();
/* ---- 扫描周围 Wi-Fi 并列出, 点一下就把 SSID 填进输入框(不用手打) ---- */
function esc(s){ return String(s).replace(/&/g,'&amp;').replace(/</g,'&lt;').replace(/>/g,'&gt;').replace(/"/g,'&quot;'); }
function lvl(r){ return r>=-60?'信号强':(r>=-75?'信号中':'信号弱'); }
function scan(){
  var box=document.getElementById('aplist');
  box.innerHTML='<div class="apm">正在扫描周围的 Wi-Fi…（约 2 秒）</div>';
  fetch('/scan').then(function(r){return r.json();}).then(function(d){
    if(!d.list||!d.list.length){ box.innerHTML='<div class="apm">没扫到 Wi-Fi —— 请确认路由器是 2.4GHz, 或直接在下面手打名称</div>'; return; }
    box.innerHTML=d.list.map(function(a){
      return '<div class="ap" onclick="pick(this)"><span>'+esc(a.ssid)+'</span>'+
             '<span class="lvl">'+lvl(a.rssi)+'</span></div>';
    }).join('');
    box.querySelectorAll('.ap').forEach(function(el){
      el.dataset.ssid = d.list[[].indexOf.call(box.querySelectorAll('.ap'), el)].ssid;
    });
  }).catch(function(){ box.innerHTML='<div class="apm">扫描失败, 请在下面手打 Wi-Fi 名称</div>'; });
}
function pick(el){
  document.getElementById('s').value = el.dataset.ssid || el.querySelector('span').textContent;
  document.getElementById('p').focus();
  document.getElementById('m').textContent = '已选择: ' + document.getElementById('s').value + ' ，请输入密码';
}
scan();
function save(){
  var s=document.getElementById('s').value.trim();
  var p=document.getElementById('p').value;
  if(!s){ document.getElementById('m').textContent='请先填写 Wi-Fi 名称'; return; }
  document.getElementById('m').textContent='已提交，正在连接…';
  fetch('/wifi?ssid='+encodeURIComponent(s)+'&pass='+encodeURIComponent(p))
    .then(function(){ setTimeout(poll,1500); t=setInterval(poll,2000); })
    .catch(function(){ document.getElementById('m').textContent='提交失败，请重试'; });
}
</script>
</body>
</html>
)HTML";
