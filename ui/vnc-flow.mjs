const PCV = globalThis.PCV = globalThis.PCV || {};
const PROTOCOL = 'pcv-vnc-flow-v1';
const INPUT_CAP = 4 * 1024 * 1024;
const BATCH_CAP = 256 * 1024;
const FRAME_CAP = 32 * 1024;

class FlowChannel {
  constructor(url, uuid, token, WebSocketImpl, signal) {
    this._ws = new WebSocketImpl(url, PROTOCOL);
    this._ws.binaryType = 'arraybuffer';
    this.binaryType = 'arraybuffer';
    this._state = 0;
    this._handler = null;
    this._pending = [];
    this._pendingBytes = 0;
    this._nonce = null;
    this._sequence = 0n;
    this.onopen = this.onclose = this.onerror = null;
    this._signal = signal;
    this._abort = () => this._fail('VNC connection cancelled');
    this.ready = new Promise((resolve, reject) => {
      this._resolve = resolve;
      this._reject = reject;
    });
    this._ws.onopen = () => {
      if (this._state !== 0 || this._ws.protocol !== PROTOCOL || !token || !/^[0-9a-f]{8}(?:-[0-9a-f]{4}){3}-[0-9a-f]{12}$/.test(uuid)) {
        this._fail('VNC authentication or protocol required');
        return;
      }
      this._ws.send(JSON.stringify({ type: 'auth', token, uuid }));
      token = '';
    };
    this._ws.onmessage = event => this._receive(event);
    this._ws.onerror = event => { if (this.onerror) this.onerror(event); this._fail('VNC transport failed'); };
    this._ws.onclose = event => {
      this._signal?.removeEventListener('abort', this._abort);
      this._state = 3;
      this._pending = []; this._pendingBytes = 0;
      this._reject(new Error('VNC connection closed'));
      if (this.onclose) this.onclose(event);
    };
    signal?.addEventListener('abort', this._abort, { once: true });
    if (signal?.aborted) this._abort();
  }
  get readyState() { return this._state; }
  get protocol() { return this._ws.protocol; }
  get bufferedAmount() { return this._ws.bufferedAmount; }
  get onmessage() { return this._handler; }
  set onmessage(handler) {
    this._handler = handler;
    if (handler) queueMicrotask(() => this._flush());
  }
  close(code = 1000, reason = '') {
    if (this._state >= 2) return;
    this._signal?.removeEventListener('abort', this._abort);
    this._reject(new Error(reason || 'VNC connection closed'));
    this._state = 2;
    this._pending = []; this._pendingBytes = 0;
    this._ws.close(code, reason);
  }
  _fail(reason) {
    this._reject(new Error(reason));

    this.close(4008, reason);
  }
  send(data) {
    if (this._state !== 1) throw new Error('VNC channel is not ready');
    const bytes = data instanceof ArrayBuffer ? new Uint8Array(data)
      : ArrayBuffer.isView(data) ? new Uint8Array(data.buffer, data.byteOffset, data.byteLength) : null;
    if (!bytes || bytes.length > INPUT_CAP - this._ws.bufferedAmount) {
      this._fail('VNC input limit'); return;
    }
    for (let offset = 0; offset < bytes.length; offset += FRAME_CAP) {
      this._ws.send(bytes.subarray(offset, offset + FRAME_CAP));
    }
  }
  _receive(event) {
    if (this._state >= 2) return;
    if (typeof event.data === 'string') {
      let control;
      try { control = JSON.parse(event.data); } catch { this._fail('VNC control rejected'); return; }
      if (this._state === 0 && control && control.type === 'auth_ok') {
        this._state = 1;
        this._resolve(this);
        if (this.onopen) this.onopen({ type: 'open', target: this });
      } else if (this._state === 1 && control && control.type === 'checkpoint') {
        if (typeof control.nonce !== 'string' || !/^[A-Za-z0-9+/]{22}==$/.test(control.nonce)
            || typeof control.sequence !== 'string' || !/^[1-9][0-9]{0,19}$/.test(control.sequence)
            || BigInt(control.sequence) !== this._sequence + 1n
            || (this._nonce && this._nonce !== control.nonce)) {
          this._fail('VNC checkpoint rejected'); return;
        }
        this._nonce = control.nonce;
        this._sequence++;
        this._pending.push({ control });
        this._flush();
      } else this._fail('VNC control rejected');
      return;
    }
    if (this._state !== 1 || !(event.data instanceof ArrayBuffer)
        || event.data.byteLength > BATCH_CAP - this._pendingBytes) {
      this._fail('VNC receive limit'); return;
    }
    this._pendingBytes += event.data.byteLength;
    this._pending.push({ event });
    this._flush();
  }
  _flush() {
    if (!this._handler || this._state !== 1) return;
    while (this._pending.length && this._state === 1) {
      const item = this._pending.shift();
      if (item.event) {
        this._pendingBytes -= item.event.data.byteLength;
        try { this._handler(item.event); } catch { this._fail('VNC receiver failed'); return; }
      } else {
        this._ws.send(JSON.stringify({ ...item.control, type: 'ack' }));
      }
    }
  }
}

export async function openVncChannel(url, uuid, token, WebSocketImpl = WebSocket, signal) {
  if (signal?.aborted) throw new Error('VNC connection cancelled');
  const channel = new FlowChannel(url, uuid, token, WebSocketImpl, signal);
  return channel.ready;
}
PCV.vncFlow = { open: openVncChannel };
