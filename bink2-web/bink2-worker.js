let core = null;
let initializing = null;
let converting = false;

function send(type, payload = {}, transfer = []) {
  self.postMessage({ type, ...payload }, transfer);
}

async function init() {
  if (core) return core;
  if (initializing) return initializing;
  initializing = (async () => {
    try {
      importScripts('./core/bink2-core.js');
      if (typeof createBink2Core !== 'function') throw new Error('Bink2 WASM loader did not define createBink2Core().');
      core = await createBink2Core({
        locateFile(path) {
          return new URL('./core/' + path, self.location.href).href;
        },
        print(text) { send('log', { text }); },
        printErr(text) { send('log', { text: '[stderr] ' + text }); }
      });
      send('ready');
      return core;
    } catch (err) {
      send('error', { message: 'Could not load the Bink2 decoder: ' + (err?.message || String(err)) });
      throw err;
    }
  })();
  return initializing;
}

async function convert(message) {
  if (converting) throw new Error('A conversion is already running.');
  converting = true;
  const m = await init();
  const input = '/input.bk2';
  const output = '/output.webm';
  try {
    try { m.FS.unlink(input); } catch (_) {}
    try { m.FS.unlink(output); } catch (_) {}

    m.FS.writeFile(input, new Uint8Array(message.data));
    const crf = Number.isFinite(message.crf) ? message.crf : 18;
    const cpuUsed = Number.isFinite(message.cpuUsed) ? message.cpuUsed : 5;
    const frames = m.ccall(
      'transcode_bk2',
      'number',
      ['string', 'string', 'number', 'number'],
      [input, output, crf, cpuUsed]
    );

    if (frames < 0) {
      const ptr = m._bink2_last_error();
      const reason = ptr ? m.UTF8ToString(ptr) : 'Unknown decoder error';
      throw new Error(reason || 'Bink2 conversion failed.');
    }

    const bytes = m.FS.readFile(output);
    const result = bytes.slice().buffer;
    send('done', { data: result, frames }, [result]);
  } finally {
    try { m.FS.unlink(input); } catch (_) {}
    try { m.FS.unlink(output); } catch (_) {}
    converting = false;
  }
}

self.onmessage = async event => {
  const message = event.data || {};
  if (message.type !== 'convert') return;
  try {
    await convert(message);
  } catch (err) {
    send('error', { message: err?.message || String(err) });
  }
};

init().catch(() => {});
