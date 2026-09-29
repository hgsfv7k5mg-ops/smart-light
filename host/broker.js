#!/usr/bin/env node
/**
 * ============================================================================
 *  本地 MQTT Broker + HTTP 服务
 * ----------------------------------------------------------------------------
 *  基于 Node.js 的 aedes 实现一个标准 MQTT Broker，用于本地调试与演示。
 *
 *  对外提供两样东西：
 *    1) TCP 1883  —— 给 ESP32 用（标准 MQTT over TCP）
 *    2) HTTP 8083 —— 给浏览器用（WebSocket MQTT + 大屏页面 + 状态 API）
 *
 *  用法：  npm install && node broker.js   （Windows 下可双击 start-broker.cmd）
 *  大屏：  http://localhost:8083
 *
 *  端口可用 MQTT_PORT / WEB_PORT 环境变量覆盖。
 * ============================================================================
 */
'use strict';

const path = require('path');
const fs = require('fs');
const os = require('os');
const http = require('http');
const net = require('net');
const { createRequire } = require('module');

/* --------------------------------------------------------------------------
 *  依赖解析
 *  优先使用本目录的 node_modules（npm install 生成）；
 *  找不到时可用 IOT_NODE_MODULES 指向其它 node_modules 目录。
 * ------------------------------------------------------------------------ */
const LOCAL_WS = __dirname;
const FALLBACK_WS = process.env.IOT_NODE_MODULES || '';

function makeRequire() {
    const candidates = [LOCAL_WS, FALLBACK_WS].filter(Boolean);
    for (const ws of candidates) {
        try {
            const r = createRequire(path.join(ws, 'package.json'));
            r.resolve('aedes');
            return r;
        } catch (e) { /* 换下一个候选 */ }
    }
    return require;      // 最后回退：依赖装在当前目录
}
const req = makeRequire();

const TCP_PORT = Number(process.env.MQTT_PORT || 1883);
const WEB_PORT = Number(process.env.WEB_PORT || 8083);
const DIR = __dirname;

/* --------------------------------------------------------------------------
 *  运行统计 / 最近数据缓存（刷新页面立刻有数据，不用等下一次上报）
 * ------------------------------------------------------------------------ */
const stats = { bootAt: Date.now(), pubs: 0, clients: 0, peak: 0 };
const latest = Object.create(null);

/** light/<id>/<kind> → <id> */
function deviceIdOf(topic) {
    const m = /^light\/([^/]+)\/(telemetry|state|status)$/.exec(topic);
    return m ? m[1] : null;
}

function touch(id) {
    if (!latest[id]) latest[id] = { status: 'unknown', at: 0 };
    latest[id].at = Date.now();
    return latest[id];
}

function lanIPs() {
    const out = [];
    const ifaces = os.networkInterfaces();
    for (const name of Object.keys(ifaces)) {
        for (const ni of ifaces[name] || []) {
            if (ni.family === 'IPv4' && !ni.internal) {
                out.push({ iface: name, address: ni.address });
            }
        }
    }
    return out;
}

/* --------------------------------------------------------------------------
 *  HTTP 静态服务 / 状态 API
 * ------------------------------------------------------------------------ */
const MIME = {
    '.html': 'text/html; charset=utf-8',
    '.js': 'application/javascript; charset=utf-8',
    '.css': 'text/css; charset=utf-8',
};

function sendJSON(res, obj) {
    const body = Buffer.from(JSON.stringify(obj), 'utf8');
    res.writeHead(200, {
        'Content-Type': 'application/json; charset=utf-8',
        'Content-Length': body.length,
        'Cache-Control': 'no-store',
    });
    res.end(body);
}

function sendFile(res, file, mime) {
    fs.readFile(file, (err, buf) => {
        if (err) { res.writeHead(404); res.end('not found'); return; }
        res.writeHead(200, {
            'Content-Type': mime || 'application/octet-stream',
            'Content-Length': buf.length,
            'Cache-Control': 'no-cache',
        });
        res.end(buf);
    });
}

function handleRequest(r, res) {
    const url = (r.url || '/').split('?')[0];

    if (url === '/' || url === '/index.html') {
        return sendFile(res, path.join(DIR, 'dashboard.html'), MIME['.html']);
    }
    if (url === '/mqtt.min.js') {
        // 浏览器端 mqtt.js 从本地 node_modules 提供，不依赖 CDN
        // 依赖可能装在本目录，也可能在 IOT_NODE_MODULES 指向的目录
        const cands = [
            path.join(DIR, 'node_modules/mqtt/dist/mqtt.min.js'),
            path.join(DIR, 'mqtt.min.js'),
        ];
        for (const c of cands) if (fs.existsSync(c)) return sendFile(res, c, MIME['.js']);
        res.writeHead(500); return res.end('mqtt.min.js not found');
    }
    /* ---- OTA：把固件 .bin 放进本目录的 ota/ 子文件夹即可被设备拉取 ---- */
    if (url.startsWith('/ota/')) {
        const name = path.basename(url);
        if (!/\.bin$/i.test(name)) {
            res.writeHead(403); return res.end('only .bin allowed');
        }
        // 固件体积大，禁用缓存头，避免设备拿到旧包
        return sendFile(res, path.join(DIR, 'ota', name), 'application/octet-stream');
    }

    if (url === '/api/ota') {
        const dir = path.join(DIR, 'ota');
        let files = [];
        try {
            files = fs.readdirSync(dir)
                .filter((f) => /\.bin$/i.test(f))
                .map((f) => {
                    const st = fs.statSync(path.join(dir, f));
                    return { name: f, size: st.size, mtime: st.mtime.toISOString() };
                });
        } catch (e) { /* ota 目录还不存在，返回空列表 */ }
        return sendJSON(res, { ok: true, dir, files });
    }

    if (url === '/api/info') {
        return sendJSON(res, {
            ok: true,
            tcpPort: TCP_PORT,
            webPort: WEB_PORT,
            ips: lanIPs(),
            clients: stats.clients,
            peak: stats.peak,
            pubs: stats.pubs,
            uptimeSec: Math.round((Date.now() - stats.bootAt) / 1000),
            latest,
        });
    }
    res.writeHead(404); res.end('not found');
}

/* --------------------------------------------------------------------------
 *  主流程
 * ------------------------------------------------------------------------ */
async function main() {
    const { Aedes } = req('aedes');
    const { WebSocketServer, createWebSocketStream } = req('ws');

    const aedes = await Aedes.createBroker();

    /* ---- 事件日志 ---- */
    aedes.on('client', (c) => {
        stats.clients++;
        stats.peak = Math.max(stats.peak, stats.clients);
        console.log(`[连接] ${c.id}   (在线 ${stats.clients})`);
    });

    aedes.on('clientDisconnect', (c) => {
        stats.clients = Math.max(0, stats.clients - 1);
        console.log(`[断开] ${c.id}   (在线 ${stats.clients})`);
    });

    aedes.on('clientError', (c, err) => {
        console.log(`[错误] ${(c && c.id) || '?'}: ${err && err.message}`);
    });

    aedes.on('publish', (packet, client) => {
        if (!client || !packet.topic || packet.topic.startsWith('$SYS')) return;

        stats.pubs++;
        const payload = packet.payload ? packet.payload.toString('utf8') : '';
        const id = deviceIdOf(packet.topic);

        if (id) {
            const rec = touch(id);
            if (packet.topic.endsWith('/telemetry')) {
                try { rec.telemetry = JSON.parse(payload); } catch (e) { /* 非 JSON 忽略 */ }
            } else if (packet.topic.endsWith('/status')) {
                rec.status = payload.trim();
            } else if (packet.topic.endsWith('/state')) {
                rec.state = payload.trim();
            }
        }

        // 只对关键主题刷日志，避免心跳刷屏
        if (packet.topic.endsWith('/status') || packet.topic.endsWith('/set')) {
            console.log(`[消息] ${client.id} → ${packet.topic}  ${payload.slice(0, 120)}`);
        }
    });

    /* ---- 1) TCP 1883：给 ESP32 ---- */
    const tcpServer = net.createServer((conn) => aedes.handle(conn));
    tcpServer.on('error', (err) => {
        if (err.code === 'EADDRINUSE') {
            console.error(`\n[致命] TCP ${TCP_PORT} 已被占用（可能已有别的 MQTT 服务在跑）。`);
            console.error(`       换个端口：  set MQTT_PORT=1884  &&  node broker.js\n`);
        } else {
            console.error('[致命] TCP 服务出错:', err.message);
        }
        process.exit(1);
    });

    /* ---- 2) HTTP + WebSocket 8083：给浏览器 ---- */
    const httpServer = http.createServer(handleRequest);
    const wss = new WebSocketServer({ server: httpServer, maxPayload: 1 << 20 });

    wss.on('connection', (conn) => {
        const stream = createWebSocketStream(conn);
        stream._socket = conn._socket;
        aedes.handle(stream);
    });

    httpServer.on('error', (err) => {
        if (err.code === 'EADDRINUSE') {
            console.error(`\n[致命] 网页端口 ${WEB_PORT} 已被占用。`);
            console.error(`       换个端口：  set WEB_PORT=8084  &&  node broker.js\n`);
            process.exit(1);
        }
        console.error('[致命] HTTP 服务出错:', err.message);
    });

    await new Promise((res) => tcpServer.listen(TCP_PORT, '0.0.0.0', res));
    await new Promise((res) => httpServer.listen(WEB_PORT, '0.0.0.0', res));

    /* ---- 启动横幅 ---- */
    const ips = lanIPs();
    console.log('');
    console.log('=========================================');
    console.log('   本地 MQTT Broker 已启动');
    console.log('=========================================');
    console.log(`   ESP32 请连这个：`);
    if (ips.length === 0) {
        console.log('     (没检测到局域网 IP，请检查网络连接)');
    }
    ips.forEach((i) => console.log(`     mqtt://${i.address}:${TCP_PORT}    [${i.iface}]`));
    console.log('');
    console.log(`   浏览器大屏：  http://localhost:${WEB_PORT}`);
    console.log(`   WebSocket ：  ws://<电脑IP>:${WEB_PORT}/mqtt`);
    console.log('');
    console.log('   Ctrl + C 停止');
    console.log('=========================================');
    console.log('');

    const bye = () => {
        console.log('\n正在停止...');
        try { tcpServer.close(); } catch (e) { }
        try { httpServer.close(); } catch (e) { }
        process.exit(0);
    };
    process.on('SIGINT', bye);
    process.on('SIGTERM', bye);
}

main().catch((e) => {
    console.error('\n[致命] 启动失败:', e && e.message);
    console.error(e && e.stack);
    process.exit(1);
});
