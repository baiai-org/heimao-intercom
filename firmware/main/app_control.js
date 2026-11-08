/* No control lease any more: this board is used by one operator at a time, and
 * the lease kept refusing perfectly valid writes (a web page left open, a phone
 * that reconnected).  The bar now only shows identity and links to setup; every
 * page may read and write directly.  Kept as a file so existing pages that load
 * /app-control.js and read ORLControl still work. */
(() => {
  'use strict';
  const WEB_LABEL = '黑猫控制面板';
  let device;
  const identity = window.fetch('/api/device', {cache:'no-store'}).then(async r => {
    if (!r.ok) throw Error('设备身份读取失败');
    const d = await r.json();
    if (!d.device_id || d.api_version !== 2) throw Error('设备接口版本不匹配');
    device = d.device_id; return d;
  });
  identity.catch(() => {});

  const bar = document.createElement('div');
  bar.style.cssText='position:sticky;top:0;z-index:1000;display:flex;gap:10px;align-items:center;padding:4px 12px;background:rgba(10,16,26,.9);border-bottom:1px solid #23384c;font:12.5px system-ui;color:#9fb3c6';
  const status = document.createElement('span');
  status.style.cssText='flex:1;min-width:0;overflow:hidden;text-overflow:ellipsis;white-space:nowrap';
  status.textContent='连接中…';
  const net = document.createElement('a'); net.href='/setup'; net.textContent='网络管理';
  net.style.cssText='color:#89c8ff;text-decoration:none;white-space:nowrap';
  bar.append(status, net);
  document.body.prepend(bar);

  identity.then(() => { status.textContent = WEB_LABEL + ' · ' + (device || ''); })
          .catch(() => { status.textContent = '设备身份读取失败'; status.style.color = '#f87171'; });

  /* A page that opens /ws/sdr no longer needs a token; the wrapper stays so the
   * URL keeps its absolute form for older code paths. */
  const NativeWebSocket = window.WebSocket;
  window.WebSocket = class extends NativeWebSocket {
    constructor(url, protocols) {
      const u = new URL(url, location.href);
      if (protocols === undefined) super(u.toString()); else super(u.toString(), protocols);
    }
  };

  window.ORLControl = {
    identity,
    ensure: async () => {},
    get device() { return device; },
    get token() { return ''; },
    get generation() { return 0; },
    async authorizeXhr() {},
  };
})();
