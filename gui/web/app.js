// krnlexec GUI front-end. All UI runs in WebView2; native side is reached
// through window.chrome.webview.postMessage({cmd, ...payload}) and
// chrome.webview.addEventListener('message', ...).

(() => {

// ---------------------------------------------------------- native bridge

const bridge = (() => {
    const pending = new Map();
    let seq = 1;

    const send = (cmd, args = {}) => {
        const id = seq++;
        return new Promise((resolve, reject) => {
            pending.set(id, { resolve, reject });
            window.chrome.webview.postMessage(JSON.stringify({ id, cmd, ...args }));
        });
    };

    const onMessage = (e) => {
        let msg;
        try { msg = typeof e.data === 'string' ? JSON.parse(e.data) : e.data; }
        catch { return; }
        if (msg.event === 'status') { Status.apply(msg.data); return; }
        if (msg.event === 'log')    { Log.push(msg.level || 'info', msg.line); return; }
        if (msg.id && pending.has(msg.id)) {
            const p = pending.get(msg.id);
            pending.delete(msg.id);
            if (msg.error) p.reject(new Error(msg.error));
            else           p.resolve(msg.result);
        }
    };

    if (window.chrome && window.chrome.webview) {
        window.chrome.webview.addEventListener('message', onMessage);
    } else {
        console.warn('webview2 bridge missing; running in browser preview mode');
    }

    return { send };
})();

// ---------------------------------------------------------- log

const Log = {
    el: null,
    init() { this.el = document.getElementById('log'); },
    push(level, line) {
        const ts = new Date().toTimeString().slice(0, 8);
        const li = document.createElement('li');
        li.className = level;
        li.innerHTML = `<span class="ts">[${ts}]</span>${escapeHtml(line)}`;
        this.el.appendChild(li);
        this.el.scrollTop = this.el.scrollHeight;
    },
    clear() { this.el.innerHTML = ''; }
};

const escapeHtml = (s) => String(s).replace(/[&<>"']/g, c => ({
    '&':'&amp;','<':'&lt;','>':'&gt;','"':'&quot;',"'":'&#39;'
}[c]));

// ---------------------------------------------------------- status

const Status = {
    drv: false, att: false, pid: 0, loadString: 0, scriptCtx: 0, dataModel: 0,
    apply(s) {
        this.drv = !!s.driver_loaded;
        this.att = !!s.ready;
        this.pid = s.pid|0;
        this.loadString = s.load_string || 0;
        this.scriptCtx  = s.script_ctx  || 0;
        this.dataModel  = s.data_model  || 0;
        const d = document.getElementById('st-drv');
        const a = document.getElementById('st-att');
        d.className = 'dot ' + (this.drv ? 'good' : 'bad');
        d.textContent = this.drv ? '● driver' : '○ driver';
        a.className = 'dot ' + (this.att ? 'good' : 'bad');
        a.textContent = this.att ? '● attached ' + this.pid : '○ not attached';

        const info = document.getElementById('s-drvinfo');
        if (info) {
            if (this.att) {
                info.textContent =
`pid:            ${this.pid}
LoadString:     0x${this.loadString.toString(16).padStart(16, '0')}
ScriptContext:  0x${this.scriptCtx.toString(16).padStart(16, '0')}
DataModel:      0x${this.dataModel.toString(16).padStart(16, '0')}`;
            } else {
                info.textContent = this.drv ? '(attached driver, no target)' : '(driver not loaded)';
            }
        }
    }
};

// ---------------------------------------------------------- settings

const defaultSettings = {
    name: 'krnlexec',
    identity: '8',
    autoAttach: true,
    pollMs: 500,
    theme: 'vs-dark',
    fontSize: 14,
    hubUrl: ''
};

const Settings = {
    cur: { ...defaultSettings },
    load() {
        try {
            const raw = localStorage.getItem('krnlexec.settings');
            if (raw) this.cur = { ...defaultSettings, ...JSON.parse(raw) };
        } catch {}
        this.populateForm();
    },
    save() {
        this.cur = {
            name:       document.getElementById('s-name').value || 'krnlexec',
            identity:   document.getElementById('s-identity').value,
            autoAttach: document.getElementById('s-autoattach').checked,
            pollMs:     parseInt(document.getElementById('s-pollms').value, 10) || 500,
            theme:      document.getElementById('s-theme').value,
            fontSize:   parseInt(document.getElementById('s-fontsize').value, 10) || 14,
            hubUrl:     document.getElementById('s-huburl').value.trim()
        };
        try { localStorage.setItem('krnlexec.settings', JSON.stringify(this.cur)); } catch {}
        Log.push('ok', 'settings saved');
        Editor.applyTheme(this.cur.theme, this.cur.fontSize);
    },
    reset() { this.cur = { ...defaultSettings }; this.populateForm(); this.save(); },
    populateForm() {
        const s = this.cur;
        document.getElementById('s-name').value       = s.name;
        document.getElementById('s-identity').value   = s.identity;
        document.getElementById('s-autoattach').checked = s.autoAttach;
        document.getElementById('s-pollms').value     = s.pollMs;
        document.getElementById('s-theme').value      = s.theme;
        document.getElementById('s-fontsize').value   = s.fontSize;
        document.getElementById('s-huburl').value     = s.hubUrl;
    }
};

// ---------------------------------------------------------- views

const Views = {
    active: 'editor',
    set(name) {
        document.querySelectorAll('.view').forEach(v => v.classList.toggle('visible', v.id === 'view-' + name));
        document.querySelectorAll('.tabs button').forEach(b =>
            b.classList.toggle('active', b.dataset.view === name));
        this.active = name;
        if (name === 'editor') setTimeout(() => Editor.layout(), 0);
    }
};

// ---------------------------------------------------------- monaco

const Editor = {
    inst: null,
    models: new Map(),       // tab id -> monaco model
    tabs: [],                // { id, name, path, dirty, model }
    activeId: null,
    nextId: 1,

    init() {
        require.config({ paths: { vs: 'https://cdn.jsdelivr.net/npm/monaco-editor@0.45.0/min/vs' } });
        require(['vs/editor/editor.main'], () => {
            this.inst = monaco.editor.create(document.getElementById('monaco'), {
                value: '',
                language: 'lua',
                theme: Settings.cur.theme,
                fontSize: Settings.cur.fontSize,
                fontFamily: '"JetBrains Mono","Fira Code","Cascadia Mono",Consolas,monospace',
                minimap: { enabled: false },
                automaticLayout: false,
                scrollBeyondLastLine: false,
                tabSize: 4,
                renderWhitespace: 'selection',
                contextmenu: true
            });

            this.inst.onDidChangeModelContent(() => {
                const t = this.tabs.find(t => t.id === this.activeId);
                if (t && !t.dirty) { t.dirty = true; this.renderTabs(); }
            });

            window.addEventListener('resize', () => this.layout());
            this.newTab('scratch.lua', '', '-- krnlexec scratch\nprint(identifyexecutor())\n');
        });
    },

    applyTheme(theme, size) {
        if (!this.inst) return;
        monaco.editor.setTheme(theme);
        this.inst.updateOptions({ fontSize: size });
    },

    layout() {
        if (this.inst) this.inst.layout();
    },

    newTab(name = 'untitled.lua', path = '', body = '') {
        if (!monaco) return;
        const id = this.nextId++;
        const model = monaco.editor.createModel(body, 'lua');
        this.tabs.push({ id, name, path, dirty: false, model });
        this.switchTo(id);
    },

    switchTo(id) {
        const t = this.tabs.find(t => t.id === id);
        if (!t) return;
        this.activeId = id;
        this.inst.setModel(t.model);
        this.renderTabs();
    },

    closeTab(id) {
        const idx = this.tabs.findIndex(t => t.id === id);
        if (idx < 0) return;
        const [t] = this.tabs.splice(idx, 1);
        t.model.dispose();
        if (this.tabs.length === 0) this.newTab();
        else if (this.activeId === id) this.switchTo(this.tabs[Math.max(0, idx - 1)].id);
        this.renderTabs();
    },

    renderTabs() {
        const bar = document.getElementById('tabs-bar');
        bar.innerHTML = '';
        for (const t of this.tabs) {
            const el = document.createElement('div');
            el.className = 'tab' + (t.id === this.activeId ? ' active' : '');
            el.innerHTML = (t.dirty ? '<span class="dirty">●</span>' : '') +
                           escapeHtml(t.name) +
                           ' <span class="close" data-close="' + t.id + '">×</span>';
            el.addEventListener('click', (e) => {
                if (e.target.dataset.close) this.closeTab(parseInt(e.target.dataset.close, 10));
                else this.switchTo(t.id);
            });
            bar.appendChild(el);
        }
    },

    currentSource() {
        const t = this.tabs.find(t => t.id === this.activeId);
        return t ? t.model.getValue() : '';
    },

    currentTab() { return this.tabs.find(t => t.id === this.activeId); },

    loadIntoNewTab(name, body, path = '') {
        this.newTab(name, path, body);
    }
};

// ---------------------------------------------------------- workspace

const Workspace = {
    async rescan() {
        try {
            const files = await bridge.send('ws_list');
            const ul = document.getElementById('file-list');
            ul.innerHTML = '';
            if (!files || !files.length) {
                const li = document.createElement('li');
                li.className = 'empty';
                li.textContent = '(no scripts)';
                ul.appendChild(li);
                return;
            }
            for (const name of files) {
                const li = document.createElement('li');
                li.textContent = name;
                li.addEventListener('click', async () => {
                    const body = await bridge.send('ws_read', { name });
                    Editor.loadIntoNewTab(name, body, name);
                });
                ul.appendChild(li);
            }
        } catch (e) {
            Log.push('err', 'workspace rescan: ' + e.message);
        }
    },

    async save(name, body) {
        return bridge.send('ws_write', { name, body });
    }
};

// ---------------------------------------------------------- hub

const Hub = {
    all: [],

    async refresh() {
        const sel = document.getElementById('hub-source');
        const custom = Settings.cur.hubUrl;
        if (custom && sel.querySelector('option[value="custom"]') == null) {
            const opt = document.createElement('option');
            opt.value = 'custom'; opt.textContent = 'Custom URL';
            sel.appendChild(opt);
        }
        sel.value = custom ? 'custom' : 'builtin';

        try {
            let scripts;
            if (custom && sel.value === 'custom') {
                const r = await fetch(custom, { cache: 'no-cache' });
                scripts = (await r.json()).scripts || [];
            } else {
                const r = await fetch('catalog.json', { cache: 'no-cache' });
                scripts = (await r.json()).scripts || [];
            }
            this.all = scripts;
            this.render(document.getElementById('hub-search').value.trim().toLowerCase());
        } catch (e) {
            Log.push('err', 'hub refresh: ' + e.message);
        }
    },

    render(filter = '') {
        const grid = document.getElementById('hub-grid');
        const empty = document.getElementById('hub-empty');
        grid.innerHTML = '';
        const list = this.all.filter(s =>
            !filter ||
            (s.title || '').toLowerCase().includes(filter) ||
            (s.description || '').toLowerCase().includes(filter) ||
            (s.tags || []).some(t => t.toLowerCase().includes(filter))
        );
        if (!list.length) { empty.classList.add('empty-active'); return; }
        empty.classList.remove('empty-active');
        for (const s of list) {
            const card = document.createElement('div');
            card.className = 'hub-card';
            card.innerHTML = `
                <h3>${escapeHtml(s.title || s.id)}</h3>
                <div class="meta">
                    <span>by ${escapeHtml(s.author || 'unknown')}</span>
                    <span>${(s.tags || []).map(escapeHtml).join(' · ')}</span>
                </div>
                <p>${escapeHtml(s.description || '')}</p>
                <div class="row">
                    <button class="primary" data-act="exec">▶ Execute</button>
                    <button class="ghost" data-act="open">Open in editor</button>
                </div>`;
            card.querySelector('[data-act="exec"]').addEventListener('click', () => this.execute(s));
            card.querySelector('[data-act="open"]').addEventListener('click', () => this.openIn(s));
            grid.appendChild(card);
        }
    },

    async execute(s) {
        if (!Status.att) { Log.push('err', 'not attached'); return; }
        try {
            await bridge.send('exec', { source: s.source });
            Log.push('ok', 'hub: executed ' + (s.title || s.id));
        } catch (e) { Log.push('err', 'hub exec: ' + e.message); }
    },

    openIn(s) {
        const name = (s.id || 'hub') + '.lua';
        Editor.loadIntoNewTab(name, s.source);
        Views.set('editor');
    }
};

// ---------------------------------------------------------- bindings

function bindHeader() {
    document.querySelectorAll('.tabs button').forEach(b => {
        b.addEventListener('click', () => Views.set(b.dataset.view));
    });
    document.getElementById('btn-attach').addEventListener('click', async () => {
        try { await bridge.send('attach'); Log.push('ok', 'attach: requested'); }
        catch (e) { Log.push('err', 'attach: ' + e.message); }
    });
    document.getElementById('btn-release').addEventListener('click', async () => {
        try { await bridge.send('release'); Log.push('ok', 'released'); }
        catch (e) { Log.push('err', 'release: ' + e.message); }
    });
}

function bindEditor() {
    document.getElementById('btn-exec').addEventListener('click', () => executeCurrent());
    document.getElementById('btn-save').addEventListener('click', () => saveCurrent());
    document.getElementById('btn-new').addEventListener('click', () => Editor.newTab());
    document.getElementById('btn-rescan').addEventListener('click', () => Workspace.rescan());
    document.getElementById('btn-clear').addEventListener('click', () => Log.clear());

    window.addEventListener('keydown', (e) => {
        if (e.key === 'F5') { e.preventDefault(); executeCurrent(); }
        else if ((e.ctrlKey || e.metaKey) && e.key === 's') { e.preventDefault(); saveCurrent(); }
        else if ((e.ctrlKey || e.metaKey) && e.key === 'n') { e.preventDefault(); Editor.newTab(); }
    });
}

function bindHub() {
    document.getElementById('hub-refresh').addEventListener('click', () => Hub.refresh());
    document.getElementById('hub-search').addEventListener('input', (e) => {
        Hub.render(e.target.value.trim().toLowerCase());
    });
    document.getElementById('hub-source').addEventListener('change', () => Hub.refresh());
}

function bindSettings() {
    document.getElementById('s-save').addEventListener('click', () => Settings.save());
    document.getElementById('s-reset').addEventListener('click', () => Settings.reset());
    bridge.send('workspace_path').then(p => {
        document.getElementById('s-workspace').value = p || '';
    }).catch(() => {});
}

async function executeCurrent() {
    const src = Editor.currentSource();
    if (!src) { Log.push('err', 'empty script'); return; }
    if (!Status.att) { Log.push('err', 'not attached'); return; }
    try {
        await bridge.send('exec', { source: src });
        Log.push('ok', 'executed (' + src.length + ' bytes)');
    } catch (e) {
        Log.push('err', 'exec: ' + e.message);
    }
}

async function saveCurrent() {
    const t = Editor.currentTab();
    if (!t) return;
    try {
        await Workspace.save(t.name, t.model.getValue());
        t.dirty = false;
        Editor.renderTabs();
        Log.push('ok', 'saved: ' + t.name);
        Workspace.rescan();
    } catch (e) {
        Log.push('err', 'save: ' + e.message);
    }
}

// ---------------------------------------------------------- boot

window.addEventListener('DOMContentLoaded', () => {
    Log.init();
    Settings.load();
    bindHeader();
    bindEditor();
    bindHub();
    bindSettings();
    Editor.init();
    Workspace.rescan();
    Hub.refresh();
    if (Settings.cur.autoAttach) {
        setTimeout(() => bridge.send('attach').catch(() => {}), 500);
    }
});

})();
