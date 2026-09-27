#!/usr/bin/env node
/*
 * music_proxy.js — 网易云音乐代理服务端 (零依赖, 只要 Node.js 18+)
 *
 * 给桌面摆件机器人 (ESP32-S3 固件) 用的音乐代理。固件只会调下面 4 个接口,
 * 本服务端把它们翻译成网易云音乐的真实请求, 并代为携带登录 Cookie。
 *
 *   GET /api/search?keyword=<关键词>&limit=<N>          搜歌
 *   GET /api/user/playlists                             我的歌单(需登录)
 *   GET /api/playlist/songs?id=<歌单id>&limit=<N>       歌单里的歌
 *   GET /api/stream?id=<歌曲id>&br=<码率>               音频流(支持 Range 断点续传)
 *
 * ★★ 本文件【不含任何账号信息】★★
 *   首次使用必须先用网易云 APP 扫码登录 (浏览器打开 http://<本机IP>:3000/),
 *   登录成功后 Cookie 保存在 data/cookie.txt —— 这个文件【不要】提交到 git。
 *
 * 运行:  node music_proxy.js            (默认端口 3000)
 *        PORT=8080 node music_proxy.js
 */
'use strict'

const http = require('http')
const https = require('https')
const crypto = require('crypto')
const fs = require('fs')
const path = require('path')

const PORT = Number(process.env.PORT || 3000)
const COOKIE_FILE = path.join(__dirname, 'data', 'cookie.txt')

/* ============================================================
 * 网易云接口加密 (weapi / eapi)
 * 这些常量是网易云音乐网页端公开使用的固定值, 属于公开知识。
 * ============================================================ */
const IV = Buffer.from('0102030405060708')
const PRESET_KEY = Buffer.from('0CoJUm6Qyw8W8jud')
const BASE62 = 'abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789'
const PUBLIC_KEY =
  '-----BEGIN PUBLIC KEY-----\n' +
  'MIGfMA0GCSqGSIb3DQEBAQUAA4GNADCBiQKBgQDgtQn2JZ34ZC28NWYpAUd98iZ3' +
  '7BUrX/aKzmFbt7clFSs6sXqHauqKWqdtLkF2KexO40H1YTX8z2lSgBBOAxLsvakl' +
  'V8k4cBFK9snQXE9/DDaFt6Rr7iVZMldczhC0JNgTz+SHXT6CBHuX3e9SdB1Ua44o' +
  'ncaTWz7OBGLbCiK45wIDAQAB\n' +
  '-----END PUBLIC KEY-----'
const EAPI_KEY = 'e82ckenh8dichen8'

function aesEncrypt (buf, mode, key, iv) {
  const cipher = crypto.createCipheriv('aes-128-' + mode, key, iv)
  return Buffer.concat([cipher.update(buf), cipher.final()])
}

function rsaEncrypt (buf) {
  const padded = Buffer.concat([Buffer.alloc(128 - buf.length), buf])
  return crypto.publicEncrypt({ key: PUBLIC_KEY, padding: crypto.constants.RSA_NO_PADDING }, padded)
}

/* weapi: 两层 AES-CBC + RSA 包裹随机密钥 */
function weapi (obj) {
  const text = JSON.stringify(obj)
  const secretKey = crypto.randomBytes(16).map(n => BASE62.charCodeAt(n % 62))
  return {
    params: aesEncrypt(Buffer.from(aesEncrypt(Buffer.from(text), 'cbc', PRESET_KEY, IV).toString('base64')), 'cbc', secretKey, IV).toString('base64'),
    encSecKey: rsaEncrypt(Buffer.from(secretKey.reverse())).toString('hex')
  }
}

/* eapi: AES-ECB(主要用于取歌曲播放地址) */
function eapi (urlPath, obj) {
  const text = JSON.stringify(obj)
  const message = `nobody${urlPath}use${text}md5forencrypt`
  const digest = crypto.createHash('md5').update(message).digest('hex')
  const data = `${urlPath}-36cd479b6b5-${text}-36cd479b6b5-${digest}`
  return { params: aesEncrypt(Buffer.from(data), 'ecb', EAPI_KEY, '').toString('hex').toUpperCase() }
}

/* ============================================================
 * 登录 Cookie (服务端保存, 固件不需要知道它)
 * ============================================================ */
const baseCookies = {
  os: 'pc',
  appver: '2.9.7',
  channel: 'netease',
  /* ★ osver 必须带上 —— 实测缺了它, 取歌曲地址的 eapi 接口直接回"参数错误" */
  osver: 'Microsoft-Windows-10--build-22631-64bit'
}

function loadCookieStr () {
  try { return fs.readFileSync(COOKIE_FILE, 'utf8').trim() } catch (e) { return '' }
}

/* 把 Set-Cookie 数组合并进现有 cookie 串 */
function mergeSetCookies (oldStr, setCookies) {
  const map = {}
  const eat = s => String(s || '').split(';').forEach(kv => {
    const i = kv.indexOf('=')
    if (i > 0) map[kv.slice(0, i).trim()] = kv.slice(i + 1).trim()
  })
  eat(oldStr)
  ;(setCookies || []).forEach(sc => eat(sc.split(';')[0]))
  return Object.entries(map).map(([k, v]) => `${k}=${v}`).join('; ')
}

function saveCookie (str) {
  fs.mkdirSync(path.dirname(COOKIE_FILE), { recursive: true })
  fs.writeFileSync(COOKIE_FILE, str)
}

function cookieHeader (extra) {
  return mergeSetCookies(mergeSetCookies(
    Object.entries(baseCookies).map(([k, v]) => `${k}=${v}`).join('; '),
    loadCookieStr()), extra)
}

const hasLogin = () => /MUSIC_U=[^;\s]+/.test(loadCookieStr())

/* ============================================================
 * 向网易云音乐发请求
 * ============================================================ */
function httpsPost (host, urlPath, body, headers, timeoutMs = 12000) {
  return new Promise((resolve, reject) => {
    const req = https.request({
      host, path: urlPath, method: 'POST',
      headers: Object.assign({
        'User-Agent': 'Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36',
        'Content-Type': 'application/x-www-form-urlencoded',
        Referer: 'https://music.163.com/',
        Origin: 'https://music.163.com'
      }, headers)
    }, res => {
      const chunks = []
      res.on('data', c => chunks.push(c))
      res.on('end', () => resolve({ status: res.statusCode, headers: res.headers, body: Buffer.concat(chunks) }))
    })
    req.setTimeout(timeoutMs, () => req.destroy(new Error('timeout')))
    req.on('error', reject)
    req.end(body)
  })
}

/* 宽松 JSON 解析: 网易云个别接口(实测是取歌地址的 eapi)在出错时会把
   JSON 正文【连发两遍】, 比如 {"code":400}{"code":400}, 直接 JSON.parse
   会抛 "Unexpected non-whitespace character after JSON"。
   所以这里做"取第一个完整 JSON 对象"的扫描: 从第一个 '{' 开始, 记录
   深度并跳过字符串字面量, 括号归零即为对象结束。 */
function parseJsonLoose (buf) {
  const s = buf.toString('utf8')
  const a = s.indexOf('{')
  if (a < 0) throw new Error('响应不是 JSON: ' + s.slice(0, 80))
  let depth = 0, inStr = false, esc = false
  for (let i = a; i < s.length; i++) {
    const c = s[i]
    if (inStr) {
      if (esc) esc = false
      else if (c === '\\') esc = true
      else if (c === '"') inStr = false
      continue
    }
    if (c === '"') inStr = true
    else if (c === '{' || c === '[') depth++
    else if (c === '}' || c === ']') {
      depth--
      if (depth === 0) return JSON.parse(s.slice(a, i + 1))
    }
  }
  throw new Error('响应 JSON 不完整: ' + s.slice(0, 80))
}

/* weapi 调用 */
async function callWeapi (urlPath, data) {
  const { params, encSecKey } = weapi(data)
  const body = `params=${encodeURIComponent(params)}&encSecKey=${encodeURIComponent(encSecKey)}`
  const r = await httpsPost('music.163.com', urlPath.replace(/^\/api/, '/weapi'), body, {
    Cookie: cookieHeader()
  })
  return parseJsonLoose(r.body)
}

/* eapi 调用 (需要 cookie 才能拿到完整歌曲地址) */
async function callEapi (urlPath, data) {
  const { params } = eapi(urlPath, data)
  const body = `params=${params}`
  const r = await httpsPost('interface3.music.163.com', urlPath, body, {
    Cookie: cookieHeader()
  })
  return parseJsonLoose(r.body)
}

/* ============================================================
 * 业务接口
 * ============================================================ */
function json (res, code, obj) {
  const buf = Buffer.from(JSON.stringify(obj))
  res.writeHead(code, { 'Content-Type': 'application/json; charset=utf-8', 'Content-Length': buf.length })
  res.end(buf)
}

/* 歌手名: 新接口是 ar[], 老接口是 artists[] */
const artistOf = t => Array.isArray(t.ar) ? t.ar.map(a => a.name).join('/')
  : (Array.isArray(t.artists) ? t.artists.map(a => a.name).join('/') : '')

async function apiSearch (q) {
  const kw = q.keyword || ''
  const limit = Math.min(Number(q.limit) || 20, 100)
  /* ★ 用老的 /weapi/search/get: 实测 cloudsearch 未登录会回 code 50000005 */
  const d = await callWeapi('/api/search/get', { s: kw, type: 1, limit, offset: Number(q.offset) || 0 })
  const songs = (d.result && d.result.songs) || []
  return {
    code: 200,
    data: songs.map(s => ({ id: String(s.id), name: s.name || '', artist: artistOf(s) }))
  }
}

async function apiPlaylists () {
  const acc = await callWeapi('/api/w/nuser/account/get', {})
  const uid = acc.profile && acc.profile.userId
  if (!uid) return { code: 301, message: '未登录 —— 请先扫码登录(浏览器打开本服务端首页)' }
  const d = await callWeapi('/api/user/playlist', { uid, limit: 100, offset: 0 })
  const list = (d.playlist || []).filter(p => p.creator && p.creator.userId === uid)  // 只要自己创建/收藏的
  return {
    code: 200,
    data: list.map(p => ({ id: String(p.id), name: p.name, count: p.trackCount || 0 }))
  }
}

async function apiPlaylistSongs (q) {
  const id = q.id
  const limit = Math.min(Number(q.limit) || 20, 300)
  const d = await callWeapi('/api/v6/playlist/detail', { id, n: limit, s: 0 })
  const pl = d.playlist || {}
  const tracks = pl.tracks || []
  return {
    code: 200,
    name: pl.name || '',
    data: tracks.slice(0, limit).map(t => ({
      id: String(t.id),
      name: t.name || '',
      artist: artistOf(t)
    }))
  }
}

/* 取真实播放地址; 拿不到( VIP/版权 )返回 null */
async function apiSongUrl (id, br) {
  const d = await callEapi('/api/song/enhance/player/url',
    { ids: `[${id}]`, br: Number(br) || 128000, header: { os: 'pc', appver: '2.9.7' } })
  const item = d.data && d.data[0]
  if (!item || !item.url || item.code !== 200) return null
  return item.url
}

/* ============================================================
 * 扫码登录 (网易云 APP 扫二维码)
 *   /login/qr/key            取 key
 *   /login/qr/check?key=     轮询: 800=过期 801=等待扫码 802=待确认 803=成功
 * ============================================================ */
async function apiQrKey () {
  /* ★ 必须带 type:1 —— 实测传 {} 网易云回"参数错误" */
  const d = await callWeapi('/api/login/qrcode/unikey', { type: 1 })
  return { code: 200, unikey: d.unikey || (d.data && d.data.unikey) || '' }
}

async function apiQrCheck (q) {
  const key = q.key || ''
  if (!key) return { code: 400, message: 'missing key' }
  /* 这个请求是"登录动作", 网易云在 803 时通过 Set-Cookie 发回登录态 */
  const { params, encSecKey } = weapi({ key, type: 1 })
  const body = `params=${encodeURIComponent(params)}&encSecKey=${encodeURIComponent(encSecKey)}`
  const r = await httpsPost('music.163.com', '/weapi/login/qrcode/client/login', body, { Cookie: cookieHeader() })
  const setCookies = r.headers['set-cookie']
  if (setCookies && setCookies.length) {
    const merged = mergeSetCookies(loadCookieStr(), setCookies)
    if (/MUSIC_U=[^;\s]+/.test(merged)) { saveCookie(merged); console.log('[login] 已保存登录 Cookie ->', COOKIE_FILE) }
  }
  let d = {}
  try { d = JSON.parse(r.body.toString('utf8') || '{}') } catch (e) {}
  return {
    code: d.code || 0,
    message: d.code === 803 ? '登录成功' : (d.message || ''),
    nickname: (d.profile && d.profile.nickname) || ''
  }
}

async function apiLoginStatus () {
  if (!hasLogin()) return { code: 200, login: false }
  const d = await callWeapi('/api/w/nuser/account/get', {})
  return { code: 200, login: !!d.profile, nickname: (d.profile && d.profile.nickname) || '', vipType: (d.profile && d.profile.vipType) || 0 }
}

/* ============================================================
 * 登录页 (扫码用, 无任何外部依赖的纯静态页)
 * ============================================================ */
const LOGIN_PAGE = `<!DOCTYPE html>
<html lang="zh"><head><meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1">
<title>网易云音乐 · 扫码登录</title>
<style>
body{font-family:-apple-system,"PingFang SC","Microsoft YaHei",sans-serif;background:#12121e;color:#eee;
     display:flex;align-items:center;justify-content:center;min-height:100vh;margin:0}
.card{background:#1c1c2e;border:1px solid #2c2c44;border-radius:16px;padding:28px 30px;width:320px;text-align:center}
h1{font-size:18px;margin:0 0 6px;color:#fff}
p.sub{font-size:12.5px;color:#8b95bb;margin:0 0 18px;line-height:1.6}
#qr{width:220px;height:220px;margin:0 auto 14px;background:#fff;border-radius:10px;padding:10px;display:flex;align-items:center;justify-content:center}
#qr canvas{width:200px;height:200px}
#st{font-size:14px;margin:0 0 16px;min-height:20px}
button{background:#dd001b;color:#fff;border:0;border-radius:10px;padding:10px 22px;font-size:14px;cursor:pointer}
button:disabled{opacity:.45}
.tip{font-size:11.5px;color:#6b76a0;margin-top:14px;line-height:1.6;text-align:left}
</style></head><body>
<div class="card">
  <h1>网易云音乐登录</h1>
  <p class="sub">用<b>网易云音乐 APP</b> 右上角「扫一扫」<br>扫描下面的二维码</p>
  <div id="qr"><span style="color:#888;font-size:13px">生成中…</span></div>
  <p id="st">正在获取二维码…</p>
  <button onclick="start()" id="btn">刷新二维码</button>
  <p class="tip">· 登录成功后本机自动保存 Cookie, 固件无需任何配置<br>
     · Cookie 保存在 data/cookie.txt, 请勿泄露<br>
     · 遇到"二维码过期"点上面的按钮刷新即可</p>
</div>
<script src="https://cdn.jsdelivr.net/npm/qrcode@1.5.3/build/qrcode.min.js"></script>
<script>
let key = '', timer = null;
function setStatus(t){ document.getElementById('st').textContent = t; }
async function start(){
  clearInterval(timer); setStatus('正在获取二维码…');
  try{
    const k = await (await fetch('/login/qr/key')).json();
    if(!k.unikey){ setStatus('获取失败: '+(k.message||'')); return; }
    key = k.unikey;
    const link = 'https://music.163.com/login?codekey=' + encodeURIComponent(key);
    if(window.QRCode){
      document.getElementById('qr').innerHTML = '';
      QRCode.toCanvas(document.createElement('canvas'), link, {width:200, margin:0}, (e,c)=>{
        if(e){ setStatus('二维码生成失败'); return; }
        document.getElementById('qr').appendChild(c);
      });
    }else{
      document.getElementById('qr').innerHTML =
        '<img style="width:200px;height:200px" src="https://api.qrserver.com/v1/create-qr-code/?size=200x200&data='+encodeURIComponent(link)+'">';
    }
    setStatus('等待扫码…');
    timer = setInterval(poll, 2000);
  }catch(e){ setStatus('网络错误: '+e.message); }
}
async function poll(){
  try{
    const d = await (await fetch('/login/qr/check?key='+encodeURIComponent(key))).json();
    if(d.code === 802){ setStatus('已扫码, 请在手机上确认'); }
    else if(d.code === 803){
      clearInterval(timer); setStatus('登录成功: '+(d.nickname||''));
      document.getElementById('qr').innerHTML = '<span style="color:#0a0;font-size:26px">&#10004;</span>';
    }
    else if(d.code === 800){ clearInterval(timer); setStatus('二维码已过期, 请点「刷新二维码」'); }
    else { setStatus('等待扫码…'); }
  }catch(e){}
}
start();
</script></body></html>`

/* ============================================================
 * HTTP 服务
 * ============================================================ */
const server = http.createServer(async (req, res) => {
  const u = new URL(req.url, `http://localhost:${PORT}`)
  const q = Object.fromEntries(u.searchParams.entries())
  const p = u.pathname

  try {
    /* ---- 首页 = 登录页 ---- */
    if (p === '/' || p === '/login') {
      const buf = Buffer.from(LOGIN_PAGE)
      res.writeHead(200, { 'Content-Type': 'text/html; charset=utf-8', 'Content-Length': buf.length })
      return res.end(buf)
    }

    /* ---- 扫码登录 ---- */
    if (p === '/login/qr/key')    return json(res, 200, await apiQrKey())
    if (p === '/login/qr/check')  return json(res, 200, await apiQrCheck(q))
    if (p === '/login/status')    return json(res, 200, await apiLoginStatus())

    /* ---- 固件用的 4 个接口 ---- */
    if (p === '/api/search')         return json(res, 200, await apiSearch(q))
    if (p === '/api/user/playlists') return json(res, 200, await apiPlaylists())
    if (p === '/api/playlist/songs') return json(res, 200, await apiPlaylistSongs(q))

    if (p === '/api/stream') {
      const real = await apiSongUrl(q.id, q.br)
      if (!real) return json(res, 404, { code: 404, message: '无可用播放链接 (多为 VIP/版权受限, 或未登录)' })
      /* 透传 Range(固件的断点续传依赖它) */
      const h = { Referer: 'https://music.163.com/', 'User-Agent': 'Mozilla/5.0' }
      if (req.headers.range) h.Range = req.headers.range
      const up = https.get(real, { headers: h }, up => {
        const head = { 'Content-Type': up.headers['content-type'] || 'application/octet-stream' }
        if (up.headers['content-length']) head['Content-Length'] = up.headers['content-length']
        if (up.headers['content-range'])  head['Content-Range'] = up.headers['content-range']
        if (up.headers['accept-ranges'])  head['Accept-Ranges'] = up.headers['accept-ranges']
        res.writeHead(up.statusCode, head)
        up.pipe(res)
      })
      up.on('error', () => { try { res.destroy() } catch (e) {} })
      return
    }

    json(res, 404, { code: 404, message: 'no such api: ' + p })
  } catch (e) {
    console.error('[error]', p, e.message)
    json(res, 500, { code: 500, message: e.message })
  }
})

server.listen(PORT, () => {
  console.log(`音乐代理已启动: http://0.0.0.0:${PORT}/`)
  console.log(`  扫码登录:  浏览器打开 http://<本机IP>:${PORT}/  (用网易云 APP 扫码)`)
  console.log(`  登录状态:  curl http://127.0.0.1:${PORT}/login/status`)
  console.log(`  Cookie 存: ${COOKIE_FILE}  ${hasLogin() ? '(已登录)' : '(尚未登录)'}`)
})
