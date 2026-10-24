/* 桌面小纸壳网页烧录 - 自定义固件模式逻辑
 * 模式一（bundled）：直接用静态 manifest.json（内置 v1.1.1 固件）
 * 模式二（custom）：用户选芯片 + 本地 bin 文件 + 烧录地址，动态生成 manifest（blob URL）
 */
(function () {
  'use strict';

  var $ = function (s) { return document.querySelector(s); };

  /* ---------- 浏览器兼容检查 ---------- */
  var supported = ('serial' in navigator) && window.isSecureContext;
  if (!supported) {
    var w = $('#browserWarn');
    if (w) { w.classList.add('show'); }
    var b = $('#installBtn');
    if (b) { b.setAttribute('install-unsupported', ''); b.style.display = 'none'; }
  }

  /* ---------- 模式切换 ---------- */
  var installBtn = $('#installBtn');
  var bundledPanel = $('#bundledPanel');
  var customPanel = $('#customPanel');
  var modeBundled = $('#modeBundled');
  var modeCustom = $('#modeCustom');
  var flashBtn = $('#flashBtn');

  function setMode(m) {
    modeBundled.classList.toggle('active', m === 'bundled');
    modeCustom.classList.toggle('active', m === 'custom');
    bundledPanel.style.display = (m === 'bundled') ? '' : 'none';
    customPanel.classList.toggle('show', m === 'custom');
    hideError();
    if (m === 'bundled') {
      installBtn.setAttribute('manifest', 'manifest.json');
    }
  }
  modeBundled.addEventListener('click', function () { setMode('bundled'); });
  modeCustom.addEventListener('click', function () { setMode('custom'); });

  /* ---------- 自定义模式 ---------- */
  var rowsEl = $('#fileRows');
  var addBtn = $('#addRowBtn');
  var chipSel = $('#chipSel');
  var totalLine = $('#totalLine');
  var errBox = $('#customErr');
  var LS_CHIP = 'rcw_chip';
  var LS_OFF = 'rcw_offsets';
  var rows = [];

  function fmtSize(n) {
    if (n >= 1048576) { return (n / 1048576).toFixed(2) + ' MB'; }
    if (n >= 1024) { return (n / 1024).toFixed(1) + ' KB'; }
    return n + ' B';
  }

  function parseOffset(s) {
    s = (s || '').trim();
    if (!s) { return null; }
    var m = /^(0[xX])?([0-9a-fA-F]+)$/.exec(s);
    if (!m) { return null; }
    var v = parseInt(m[2], m[1] ? 16 : 10);
    if (!isFinite(v) || v < 0) { return null; }
    return v;
  }

  function showError(msg) { errBox.textContent = msg; errBox.classList.add('show'); }
  function hideError() { errBox.classList.remove('show'); }

  function persist() {
    try {
      localStorage.setItem(LS_OFF, JSON.stringify(rows.map(function (r) { return r.offset; })));
      localStorage.setItem(LS_CHIP, chipSel.value);
    } catch (e) { /* 隐私模式下忽略 */ }
  }

  function updateTotal() {
    var n = 0, bytes = 0;
    rows.forEach(function (r) { if (r.file) { n++; bytes += r.size; } });
    totalLine.innerHTML = '共 <b>' + n + '</b> 个文件，<b>' + fmtSize(bytes) + '</b>';
  }

  function renderRows() {
    rowsEl.innerHTML = '';
    rows.forEach(function (r, idx) {
      var row = document.createElement('div');
      row.className = 'frow' + (r.file ? ' hasfile' : '');

      var btn = document.createElement('button');
      btn.type = 'button'; btn.className = 'fbtn'; btn.textContent = '选择文件';

      var name = document.createElement('span');
      name.className = 'fname' + (r.file ? '' : ' empty');
      name.textContent = r.file ? r.name : '未选择文件';
      name.title = r.file ? r.name : '';

      var off = document.createElement('input');
      off.type = 'text'; off.className = 'foff'; off.placeholder = '0x10000';
      off.value = r.offset || '';

      var sz = document.createElement('span');
      sz.className = 'fsize';
      sz.textContent = r.file ? fmtSize(r.size) : '';

      var del = document.createElement('button');
      del.type = 'button'; del.className = 'fdel'; del.title = '删除本行'; del.textContent = '✕';

      var fi = document.createElement('input');
      fi.type = 'file'; fi.style.display = 'none';

      btn.addEventListener('click', function () { fi.click(); });
      fi.addEventListener('change', function () {
        if (fi.files && fi.files[0]) {
          rows[idx].file = fi.files[0];
          rows[idx].name = fi.files[0].name;
          rows[idx].size = fi.files[0].size;
          renderRows();
          persist();
        }
      });
      off.addEventListener('input', function () {
        rows[idx].offset = off.value;
        persist();
      });
      del.addEventListener('click', function () {
        rows.splice(idx, 1);
        renderRows();
        persist();
      });

      row.appendChild(btn);
      row.appendChild(name);
      row.appendChild(off);
      row.appendChild(sz);
      row.appendChild(del);
      row.appendChild(fi);
      rowsEl.appendChild(row);
    });
    updateTotal();
  }

  addBtn.addEventListener('click', function () {
    rows.push({ file: null, name: '', size: 0, offset: '' });
    renderRows();
    persist();
  });

  chipSel.addEventListener('change', persist);

  /* 生成 manifest 并返回 blob URL；校验失败返回 null 并提示 */
  function buildManifestUrl() {
    hideError();
    var chip = chipSel.value;
    var parts = [];
    for (var i = 0; i < rows.length; i++) {
      var r = rows[i];
      if (!r.file) { continue; }
      var off = parseOffset(r.offset);
      if (off === null) {
        showError('第 ' + (i + 1) + ' 个文件「' + r.name + '」的烧录地址无效：' +
                  (r.offset || '(空)') + '。请填 0x 开头的十六进制或十进制数字。');
        return null;
      }
      parts.push({ file: r.file, name: r.name, size: r.size, offset: off });
    }
    if (parts.length === 0) {
      showError('请先添加至少一个固件文件（点「选择文件」）。');
      return null;
    }
    parts.sort(function (a, b) { return a.offset - b.offset; });
    for (var j = 1; j < parts.length; j++) {
      var prevEnd = parts[j - 1].offset + parts[j - 1].size;
      if (parts[j].offset < prevEnd) {
        showError('文件地址重叠：「' + parts[j - 1].name + '」(0x' + parts[j - 1].offset.toString(16) +
                  ' ~ 0x' + (prevEnd - 1).toString(16) + ') 与「' + parts[j].name +
                  '」(0x' + parts[j].offset.toString(16) + ') 重叠，请检查地址。');
        return null;
      }
    }
    var mparts = parts.map(function (p) {
      return { path: URL.createObjectURL(p.file), offset: p.offset, name: p.name };
    });
    var manifest = {
      name: '自定义固件 (' + chip + ')',
      version: 'custom',
      new_install_prompt_erase: true,
      builds: [{ chipFamily: chip, parts: mparts }]
    };
    var blob = new Blob([JSON.stringify(manifest)], { type: 'application/json' });
    return URL.createObjectURL(blob);
  }

  /* 点击烧录按钮：自定义模式下先构建 manifest 再交给组件。
   * 本监听器注册在按钮（事件目标）上，先于组件内部 slot 上的监听器执行；
   * 校验失败时阻止事件继续传播，组件就不会打开串口弹窗。 */
  flashBtn.addEventListener('click', function (e) {
    if (modeBundled.classList.contains('active')) {
      return; /* 内置模式：组件直接用静态 manifest.json */
    }
    var url = buildManifestUrl();
    if (!url) {
      e.preventDefault();
      e.stopPropagation();
      return;
    }
    installBtn.setAttribute('manifest', url);
  });

  /* ---------- 初始化：恢复上次的芯片/地址，预置 3 行 ---------- */
  var initOffsets = ['0x0', '0x8000', ''];
  try {
    var savedChip = localStorage.getItem(LS_CHIP);
    if (savedChip) {
      for (var i = 0; i < chipSel.options.length; i++) {
        if (chipSel.options[i].value === savedChip) { chipSel.selectedIndex = i; break; }
      }
    }
    var saved = JSON.parse(localStorage.getItem(LS_OFF) || 'null');
    if (Array.isArray(saved) && saved.length > 0) { initOffsets = saved; }
  } catch (e) { /* ignore */ }
  initOffsets.forEach(function (o) {
    rows.push({ file: null, name: '', size: 0, offset: o });
  });
  renderRows();
})();
