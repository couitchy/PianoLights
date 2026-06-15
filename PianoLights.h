/*
 * PianoLights.h, interface de configuration pour Piano Lights, embarquée en PROGMEM
 * Page autonome (SPA)
 */
#pragma once
#include <pgmspace.h>

const char INDEX_HTML[] PROGMEM = R"rawliteral(<!DOCTYPE html>
<html lang="fr">
<head>
    <meta charset="utf-8">
    <meta name="viewport" content="width=device-width,initial-scale=1">
    <title>Piano Lights — Configuration</title>
    <style>
        :root {
            --bg: #0e0e13;
            --panel: #17171f;
            --line: #272732;
            --txt: #e9e6df;
            --mut: #8d8a96;
            --amber: #ffb648;
        }

        * {
            box-sizing: border-box;
        }

        body {
            margin: 0;
            background: var(--bg);
            color: var(--txt);
            font: 15px/1.5 system-ui, 'Segoe UI', Roboto, sans-serif;
        }

        header {
            display: flex;
            align-items: center;
            gap: 14px;
            padding: 14px 22px;
            border-bottom: 1px solid var(--line);
            flex-wrap: wrap;
        }

        h1 {
            font-size: 20px;
            margin: 0;
            letter-spacing: .05em;
            font-weight: 700;
        }

        h1 b {
            color: var(--amber);
            font-weight: 700;
        }

        #chips {
            margin-left: auto;
            display: flex;
            gap: 8px;
            font-size: 12px;
            flex-wrap: wrap;
        }

        .chip {
            padding: 3px 11px;
            border: 1px solid var(--line);
            border-radius: 999px;
            color: var(--mut);
            white-space: nowrap;
        }

        .chip.on {
            color: var(--amber);
            border-color: var(--amber);
        }

        main {
            max-width: 920px;
            margin: 0 auto;
            padding: 20px 16px 90px;
        }

        section {
            background: var(--panel);
            border: 1px solid var(--line);
            border-radius: 10px;
            padding: 18px 20px;
            margin-bottom: 16px;
        }

        h2 {
            font-size: 12px;
            text-transform: uppercase;
            letter-spacing: .14em;
            color: var(--mut);
            margin: 0 0 14px;
            font-weight: 600;
        }

        label {
            font-size: 12px;
            color: var(--mut);
            display: block;
            margin-bottom: 4px;
        }

        input[type=number],
        input[type=text],
        input[type=password],
        select {
            width: 100%;
            background: #101016;
            border: 1px solid var(--line);
            color: var(--txt);
            border-radius: 6px;
            padding: 7px 9px;
            font: inherit;
        }

        #ledPin {
            max-width: 370px;
        }

        input:focus,
        select:focus,
        button:focus-visible {
            outline: 2px solid var(--amber);
            outline-offset: 1px;
        }

        .grid {
            display: grid;
            grid-template-columns: repeat(auto-fit, minmax(150px, 1fr));
            gap: 12px;
        }

        .row {
            display: flex;
            gap: 10px;
            flex-wrap: wrap;
            align-items: center;
        }

        button {
            background: #101016;
            border: 1px solid var(--line);
            color: var(--txt);
            border-radius: 6px;
            padding: 8px 14px;
            cursor: pointer;
            font: inherit;
        }

        button:hover {
            border-color: var(--amber);
        }

        button.primary {
            background: var(--amber);
            border-color: var(--amber);
            color: #1a1206;
            font-weight: 600;
        }

        .note {
            font-size: 12px;
            color: var(--mut);
        }

        input[type=color] {
            width: 46px;
            height: 34px;
            padding: 2px;
            border: 1px solid var(--line);
            background: #101016;
            border-radius: 6px;
            cursor: pointer;
        }

        input[type=checkbox],
        input[type=radio] {
            accent-color: var(--amber);
        }

        input[type=range] {
            width: 100%;
            accent-color: var(--amber);
        }

        /* Aperçu ruban + clavier */
        #strip {
            display: flex;
            height: 13px;
            border-radius: 3px;
            overflow: hidden;
            background: #000;
            gap: 1px;
            margin-bottom: 3px;
        }

        #strip i {
            flex: 1;
            background: #1d1d25;
            min-width: 0;
        }

        #kb {
            position: relative;
            height: 110px;
            display: flex;
            user-select: none;
            touch-action: manipulation;
        }

        .wk {
            border: 1px solid #000;
            background: #f3f0e8;
            border-radius: 0 0 3px 3px;
            cursor: pointer;
            flex-shrink: 0;
        }

        .bk {
            position: absolute;
            top: 0;
            height: 60%;
            background: #15151a;
            border: 1px solid #000;
            z-index: 2;
            border-radius: 0 0 3px 3px;
            cursor: pointer;
        }

        #mapinfo {
            font: 12px ui-monospace, Consolas, monospace;
            color: var(--mut);
            min-height: 18px;
            margin-top: 8px;
        }

        footer {
            position: fixed;
            bottom: 0;
            left: 0;
            right: 0;
            background: var(--panel);
            border-top: 1px solid var(--line);
            padding: 12px 22px;
            display: flex;
            gap: 12px;
            align-items: center;
        }

        #msg {
            color: var(--amber);
            font-size: 13px;
            font-weight: 600;
        }

        @media (prefers-reduced-motion: no-preference) {
            .chip,
            button {
                transition: border-color .15s, color .15s;
            }
        }
    </style>
</head>
<body>

<header>
    <h1>Piano<b>Lights</b></h1>
    <div id="chips">
        <span class="chip" id="chipWifi">WiFi...</span>
        <span class="chip" id="chipBle">BLE...</span>
        <span class="chip" id="chipLeds">— LEDs</span>
    </div>
</header>

<main>
<section>
    <h2>Alignement</h2>
    <div id="strip"></div>
    <div id="kb"></div>
    <div id="mapinfo">Cliquer sur une touche pour l'allumer.</div>
    <div class="row" style="margin:10px 0 16px">
        <span class="note">Allumer selon :</span>
        <label style="display:inline;margin:0"><input type="radio" name="hand" value="L" checked> Canal main gauche</label>
        <label style="display:inline;margin:0"><input type="radio" name="hand" value="R"> Canal main droite</label>
        <label style="display:inline;margin:0"><input type="radio" name="hand" value="O"> Canal autre</label>
        <button onclick="allOff()">Tout éteindre</button>
    </div>
    <div class="grid">
        <div>
            <label for="keyCount">Nombre de touches</label>
            <input id="keyCount" type="number" min="1" max="108" onchange="rebuild()">
        </div>
        <div>
            <label for="firstNote">Première note (MIDI)</label>
            <input id="firstNote" type="number" min="0" max="120" onchange="rebuild()">
        </div>
        <div>
            <label for="ledsPerKey">LED par touche (densité)</label>
            <input id="ledsPerKey" type="number" min="0.1" max="10" step="0.001" onchange="rebuild()">
        </div>
        <div>
            <label for="ledOffset">Première LED (offset)</label>
            <input id="ledOffset" type="number" min="-50" max="100" onchange="rebuild()">
        </div>
        <div>
            <label for="reversed">Sens du ruban LED</label>
            <label style="display:flex;gap:8px;align-items:center;margin-top:12px;color:var(--txt)"><input id="reversed" type="checkbox" onchange="rebuild()"> Inversé</label>
        </div>
    </div>
    <div class="row" style="margin-top:12px">
        <span class="note">Preset de densité :</span>
        <button onclick="density(60)">60/m</button>
        <button onclick="density(96)">96/m</button>
        <button onclick="density(120)">120/m</button>
        <button onclick="density(144)">144/m</button>
        <button onclick="density(240)">240/m</button>
        <button onclick="density(332)">332/m</button>
    </div>
</section>

<section>
    <h2>Couleurs</h2>
    <div class="grid">
        <div>
            <label for="colorLeft">Main gauche</label>
            <div class="row"><input id="colorLeft" type="color" onchange="paintAll()"><span class="note">canal</span><input id="chLeft" type="number" min="1" max="16" style="width:64px"></div>
        </div>
        <div>
            <label for="colorRight">Main droite</label>
            <div class="row"><input id="colorRight" type="color" onchange="paintAll()"><span class="note">canal</span><input id="chRight" type="number" min="1" max="16" style="width:64px"></div>
        </div>
        <div>
            <label for="colorOther">Autre</label>
            <div class="row"><input id="colorOther" type="color" onchange="paintAll()"></div>
        </div>
        <div>
            <label for="brightness">Luminosité (<span id="briVal">—</span>)</label>
            <input id="brightness" type="range" min="5" max="255" oninput="briVal.textContent=this.value">
        </div>
    </div>
    <p class="note" style="margin-bottom:0">Les numéros de canaux correspondent à ceux transmis par Synthesia.</p>
</section>

<section>
    <h2>Matériel</h2>
    <div class="grid">
        <div>
            <label for="ledPin">GPIO auquel le ruban LED est connecté</label>
            <select id="ledPin"></select>
        </div>
        <div id="reboot" style="align-self:end" hidden><button onclick="reboot()">Redémarrer</button></div>
    </div>
    <p class="note" style="margin-bottom:0">Le changement de GPIO nécessite un redémarrage.</p>
</section>

<section>
    <h2>WiFi</h2>
    <div class="grid">
        <div>
            <label for="ssid">Réseau (SSID)</label>
            <input id="ssid" type="text" autocomplete="off">
        </div>
        <div>
            <label for="pass">Mot de passe</label>
            <input id="pass" type="password" autocomplete="off">
        </div>
        <div style="align-self:end"><button onclick="saveWifi()">Enregistrer et redémarrer</button></div>
    </div>
    <p class="note" style="margin-bottom:0">En cas d'échec de connexion au démarrage, un point d'accès « Piano-Lights-AP » hébergera cette page sur <a href="http://pianolights.local" target="_blank" rel="noopener">http://pianolights.local</a> ou <a href="http://192.168.4.1" target="_blank" rel="noopener">http://192.168.4.1</a>.</p>
</section>

<section>
    <h2>Mise à jour</h2>
    <div class="row">
        <input id="fw" type="file" accept=".bin">
        <button id="fwBtn" onclick="uploadFw()">Envoyer</button>
        <span class="note">Version actuelle : <b id="fwVer">—</b></span>
    </div>
    <div id="fwBar" hidden style="height:8px;background:#101016;border:1px solid var(--line);border-radius:5px;overflow:hidden;margin-top:14px">
        <div id="fwFill" style="height:100%;width:0;background:var(--amber);transition:width .15s"></div>
    </div>
    <p class="note" style="margin-bottom:0">Sélectionner le fichier <code>.bin</code> à flasher. Redémarrage automatique à la fin de l'opération.</p>
</section>
</main>

<footer>
    <button class="primary" onclick="save()">Enregistrer les préférences</button>
    <span id="msg"></span>
</footer>

<script>
const $ = i => document.getElementById(i);
const PINS = [16, 17, 18, 19, 21, 22, 23, 25, 26, 27, 32, 33];
const BLACK = [1, 3, 6, 8, 10];
const NAMES = ['C', 'C#', 'D', 'D#', 'E', 'F', 'F#', 'G', 'G#', 'A', 'A#', 'B'];
let on = {};           // note MIDI -> canal (état local des tests)

function api(p, b) {
    return fetch(p, b ? { method: 'POST', headers: { 'Content-Type': 'application/json' }, body: JSON.stringify(b) } : undefined).then(r => r.json());
}

function geo() {
    return {
        keyCount: +$('keyCount').value || 88,
        firstNote: +$('firstNote').value || 21,
        ledsPerKey: +$('ledsPerKey').value || 2,
        ledOffset: +$('ledOffset').value || 0,
        reversed: $('reversed').checked
    };
}

function noteName(n) {
    return NAMES[n % 12] + (Math.floor(n / 12) - 1);
}

function numLeds(g) {
    return Math.min(300, Math.max(1, Math.round(g.keyCount * g.ledsPerKey) + Math.max(0, g.ledOffset)));
}

function mapNote(n, g) {
    const i = n - g.firstNote, N = numLeds(g);
    let s = Math.round(i * g.ledsPerKey), e = Math.round((i + 1) * g.ledsPerKey);
    if (e <= s) e = s + 1;
    const r = [];
    for (let k = s; k < e; k++) {
        let L = k + g.ledOffset;
        if (L < 0 || L >= N) continue;
        r.push(g.reversed ? N - 1 - L : L);
    }
    return r;
}

function colFor(ch) {
    if (ch == +$('chLeft').value) return $('colorLeft').value;
    if (ch == +$('chRight').value) return $('colorRight').value;
    return $('colorOther').value;
}

function testCh() {
    const h = document.querySelector('input[name=hand]:checked').value;
    return h == 'L' ? +$('chLeft').value : h == 'R' ? +$('chRight').value : 16;
}

function buildStrip() {
    const g = geo(), st = $('strip');
    st.innerHTML = '';
    for (let i = 0; i < numLeds(g); i++) st.appendChild(document.createElement('i'));
}

function buildKb() {
    const g = geo(), kb = $('kb');
    kb.innerHTML = '';
    let whites = 0;
    for (let i = 0; i < g.keyCount; i++) if (!BLACK.includes((g.firstNote + i) % 12)) whites++;
    const ww = 100 / Math.max(1, whites);
    let wi = 0;
    for (let i = 0; i < g.keyCount; i++) {
        const n = g.firstNote + i, d = document.createElement('div');
        d.dataset.note = n;
        if (!BLACK.includes(n % 12)) {
            d.className = 'wk';
            d.style.width = ww + '%';
            wi++;
        } else {
            d.className = 'bk';
            d.style.left = (wi * ww - ww * 0.32) + '%';
            d.style.width = (ww * 0.64) + '%';
        }
        d.onclick = () => toggle(n);
        d.onmouseenter = () => {
            const m = mapNote(n, geo());
            $('mapinfo').textContent = 'Touche ' + noteName(n) + ' (MIDI ' + n + ') → LED' + (m.length > 1 ? 's ' : ' ') + (m.length ? Math.min(...m) + (m.length > 1 ? '–' + Math.max(...m) : '') : 'hors ruban');
        };
        kb.appendChild(d);
    }
}

function paintAll() {
    const g = geo(), cells = $('strip').children;
    for (const c of cells) c.style.background = '';
    for (const k of $('kb').children) {
        const n = +k.dataset.note, ch = on[n];
        k.style.background = ch ? colFor(ch) : '';
        if (ch) for (const L of mapNote(n, g)) if (cells[L]) cells[L].style.background = colFor(ch);
    }
}

function rebuild() {
    buildStrip();
    buildKb();
    paintAll();
    $('chipLeds').textContent = numLeds(geo()) + ' LEDs';
}

function density(d) {
    $('ledsPerKey').value = (d * 0.164 / 12).toFixed(3);
    rebuild();
}

async function toggle(n) {
    const c = on[n] ? 0 : testCh();
    if (c) on[n] = c; else delete on[n];
    paintAll();
    try {
        await api('/api/test', { note: n, on: !!c, ch: c || 1 });
    } catch (e) {
        msg('Erreur réseau');
    }
}

async function allOff() {
    on = {};
    paintAll();
    try {
        await api('/api/alloff', {});
    } catch (e) {}
}

function msg(t) {
    $('msg').textContent = t;
    clearTimeout(msg.t);
    msg.t = setTimeout(() => $('msg').textContent = '', 4000);
}

async function save() {
    const g = geo();
    const body = {
        ...g,
        ledPin: +$('ledPin').value,
        colorLeft: $('colorLeft').value,
        colorRight: $('colorRight').value,
        colorOther: $('colorOther').value,
        chLeft: +$('chLeft').value,
        chRight: +$('chRight').value,
        brightness: +$('brightness').value
    };
    try {
        const r = await api('/api/config', body);
        if (r.needsReboot) $('reboot').hidden = false;
        msg(r.needsReboot ? 'Préférences enregistrées — Redémarrage nécessaire' : 'Préférences enregistrées');
    } catch (e) {
        msg('Erreur réseau');
    }
}

async function saveWifi() {
    if (!$('ssid').value) return msg('SSID vide');
    try {
        await api('/api/wifi', { ssid: $('ssid').value, pass: $('pass').value });
        msg("Enregistré — Redémarrage...");
    } catch (e) {
        msg("Redémarrage...");
    }
}

async function reboot() {
    try {
        await api('/api/reboot', {});
    } catch (e) {}
    msg("Redémarrage...");
}

function uploadFw() {
    const f = $('fw').files[0];
    if (!f) return msg('Aucun fichier sélectionné');
    if (!f.name.toLowerCase().endsWith('.bin')) return msg('Fichier .bin attendu');

    const bar = $('fwBar'), fill = $('fwFill');
    bar.hidden = false;
    fill.style.width = '0';
    $('fwBtn').disabled = true;

    const fd = new FormData();
    fd.append('update', f, f.name);

    const xhr = new XMLHttpRequest();
    xhr.open('POST', '/api/update');
    xhr.upload.onprogress = e => {
        if (e.lengthComputable) fill.style.width = (e.loaded / e.total * 100).toFixed(1) + '%';
    };
    xhr.onload = () => {
        $('fwBtn').disabled = false;
        if (xhr.status === 200) {
            fill.style.width = '100%';
            msg('Mise à jour réussie — Redémarrage...');
            setTimeout(() => location.reload(), 9000);
        } else {
            msg('Échec de la mise à jour');
        }
    };
    xhr.onerror = () => { $('fwBtn').disabled = false; msg('Erreur réseau pendant l\'envoi'); };
    msg('Envoi en cours...');
    xhr.send(fd);
}

function status(s) {
    $('chipWifi').textContent = (s.mode == 'ap' ? 'AP ' : 'WiFi ') + s.ip;
    $('chipWifi').className = 'chip' + (s.mode == 'sta' ? ' on' : '');
    $('chipBle').textContent = s.ble ? 'BLE connecté' : 'BLE en attente';
    $('chipBle').className = 'chip' + (s.ble ? ' on' : '');
    $('chipLeds').textContent = s.numLeds + ' LEDs';
    $('fwVer').textContent = 'v' + s.version;
}

async function load() {
    const sel = $('ledPin');
    PINS.forEach(p => {
        const o = document.createElement('option');
        o.value = p;
        o.textContent = 'GPIO ' + p;
        sel.appendChild(o);
    });
    try {
        const c = await api('/api/config');
        $('keyCount').value = c.keyCount;
        $('firstNote').value = c.firstNote;
        $('ledsPerKey').value = c.ledsPerKey;
        $('ledOffset').value = c.ledOffset;
        $('reversed').checked = c.reversed;
        sel.value = c.ledPin;
        $('colorLeft').value = c.colorLeft;
        $('colorRight').value = c.colorRight;
        $('colorOther').value = c.colorOther;
        $('chLeft').value = c.chLeft;
        $('chRight').value = c.chRight;
        $('brightness').value = c.brightness;
        $('briVal').textContent = c.brightness;
        $('ssid').value = c.ssid || '';
        status(c.status);
    } catch (e) {
        msg('Impossible de charger la configuration');
    }
    rebuild();
}

setInterval(async () => {
    try {
        const c = await api('/api/config');
        status(c.status);
    } catch (e) {}
}, 5000);

load();
</script>
</body>
</html>)rawliteral";
