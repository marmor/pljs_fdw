// pgwire: a minimal Postgres wire-protocol v3 client, built on pljsFdwNet
// (raw TCP + sha256/hmacSha256). Supports trust/cleartext/SCRAM-SHA-256
// auth, and the extended query protocol with text-format parameters and
// results (Parse/Bind/Describe/Execute/Sync). No SSL, no connection
// pooling/pipelining, no LISTEN/NOTIFY, no COPY, no binary format --
// deliberately trimmed to what a synchronous FDW execute()/insert()/
// update()/delete() needs.
//
// Everything below is wrapped in an IIFE rather than left at top level:
// pljs.require()/pljs_module_require() does not cache modules, and
// evaluates each require() into the *shared* global scope (module/exports
// are plain global properties, not a per-file closure) -- so a second
// pljs.require('pgwire') anywhere in the same JSContext (e.g. a second
// foreign table using this same client library) would hit "redeclaration
// of 'PgConnection'" if these were top-level declarations.
//
// Usage: load this file's contents into pljs.modules under whatever path
// you want to require() it by, e.g.:
//   INSERT INTO pljs.modules (path, source) VALUES ('pgwire', $js$ ...file
//   contents... $js$);
// then, from any pljs_fdw module:
//   const { PgConnection } = pljs.require('pgwire');
//   const conn = new PgConnection({host, port, user, password, database});
//   const {rows, rowCount, command} = conn.query('SELECT $1::int AS n', [1]);
//   conn.close();
module.exports = (function() {

// ---------- UTF-8 ----------

function utf8Encode(str) {
  const bytes = [];
  for (let i = 0; i < str.length; i++) {
    let code = str.charCodeAt(i);

    if (code >= 0xd800 && code <= 0xdbff && i + 1 < str.length) {
      const low = str.charCodeAt(i + 1);
      if (low >= 0xdc00 && low <= 0xdfff) {
        code = 0x10000 + ((code - 0xd800) << 10) + (low - 0xdc00);
        i++;
      }
    }

    if (code < 0x80) {
      bytes.push(code);
    } else if (code < 0x800) {
      bytes.push(0xc0 | (code >> 6), 0x80 | (code & 0x3f));
    } else if (code < 0x10000) {
      bytes.push(0xe0 | (code >> 12), 0x80 | ((code >> 6) & 0x3f),
                0x80 | (code & 0x3f));
    } else {
      bytes.push(0xf0 | (code >> 18), 0x80 | ((code >> 12) & 0x3f),
                0x80 | ((code >> 6) & 0x3f), 0x80 | (code & 0x3f));
    }
  }
  return bytes;
}

function utf8Decode(u8, start, end) {
  let s = '';
  let i = start;

  while (i < end) {
    const b0 = u8[i];

    if (b0 < 0x80) {
      s += String.fromCharCode(b0);
      i += 1;
    } else if ((b0 & 0xe0) === 0xc0) {
      s += String.fromCharCode(((b0 & 0x1f) << 6) | (u8[i + 1] & 0x3f));
      i += 2;
    } else if ((b0 & 0xf0) === 0xe0) {
      s += String.fromCharCode(((b0 & 0xf) << 12) |
                               ((u8[i + 1] & 0x3f) << 6) | (u8[i + 2] & 0x3f));
      i += 3;
    } else {
      const cp = ((b0 & 0x7) << 18) | ((u8[i + 1] & 0x3f) << 12) |
                ((u8[i + 2] & 0x3f) << 6) | (u8[i + 3] & 0x3f);
      s += String.fromCharCode(0xd800 + ((cp - 0x10000) >> 10),
                               0xdc00 + ((cp - 0x10000) & 0x3ff));
      i += 4;
    }
  }
  return s;
}

// ---------- base64 ----------

const B64_CHARS =
    'ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/';

function base64Encode(bytes) {
  let out = '';
  let i = 0;

  for (; i + 2 < bytes.length; i += 3) {
    const n = (bytes[i] << 16) | (bytes[i + 1] << 8) | bytes[i + 2];
    out += B64_CHARS[(n >> 18) & 0x3f] + B64_CHARS[(n >> 12) & 0x3f] +
          B64_CHARS[(n >> 6) & 0x3f] + B64_CHARS[n & 0x3f];
  }

  const rem = bytes.length - i;

  if (rem === 1) {
    const n = bytes[i] << 16;
    out += B64_CHARS[(n >> 18) & 0x3f] + B64_CHARS[(n >> 12) & 0x3f] + '==';
  } else if (rem === 2) {
    const n = (bytes[i] << 16) | (bytes[i + 1] << 8);
    out += B64_CHARS[(n >> 18) & 0x3f] + B64_CHARS[(n >> 12) & 0x3f] +
          B64_CHARS[(n >> 6) & 0x3f] + '=';
  }

  return out;
}

function base64Decode(str) {
  const clean = str.replace(/=+$/, '');
  const bytes = [];
  let buffer = 0;
  let bits = 0;

  for (let i = 0; i < clean.length; i++) {
    const v = B64_CHARS.indexOf(clean[i]);
    if (v < 0) continue;
    buffer = (buffer << 6) | v;
    bits += 6;

    if (bits >= 8) {
      bits -= 8;
      bytes.push((buffer >> bits) & 0xff);
    }
  }

  return new Uint8Array(bytes);
}

// ---------- message writer ----------

class MsgWriter {
  constructor() {
    this.bytes = [];
  }

  u8(n) {
    this.bytes.push(n & 0xff);
    return this;
  }

  i16(n) {
    this.bytes.push((n >> 8) & 0xff, n & 0xff);
    return this;
  }

  i32(n) {
    this.bytes.push((n >>> 24) & 0xff, (n >>> 16) & 0xff, (n >>> 8) & 0xff,
                    n & 0xff);
    return this;
  }

  cstr(s) {
    for (const b of utf8Encode(s)) this.bytes.push(b);
    this.bytes.push(0);
    return this;
  }

  raw(arr) {
    for (const b of arr) this.bytes.push(b);
    return this;
  }

  toUint8Array() {
    return new Uint8Array(this.bytes);
  }
}

/** @param tag single-char message type, or null for StartupMessage (no type byte). */
function buildMessage(tag, bodyFn) {
  const body = new MsgWriter();
  bodyFn(body);

  const out = new MsgWriter();
  if (tag !== null) out.u8(tag.charCodeAt(0));
  out.i32(body.bytes.length + 4);
  out.raw(body.bytes);

  return out.toUint8Array();
}

// ---------- buffered message reader ----------

class SocketReader {
  constructor(sock) {
    this.sock = sock;
    this.buf = new Uint8Array(0);
  }

  _fill(need) {
    while (this.buf.length < need) {
      const chunk = pljsFdwNet.recv(this.sock, 65536);

      if (chunk.length === 0) {
        throw new Error('pgwire: connection closed by server');
      }

      const merged = new Uint8Array(this.buf.length + chunk.length);
      merged.set(this.buf, 0);
      merged.set(chunk, this.buf.length);
      this.buf = merged;
    }
  }

  readBytes(n) {
    this._fill(n);
    const out = this.buf.slice(0, n);
    this.buf = this.buf.slice(n);
    return out;
  }

  readByte() {
    return this.readBytes(1)[0];
  }

  readInt32() {
    const b = this.readBytes(4);
    return ((b[0] << 24) | (b[1] << 16) | (b[2] << 8) | b[3]) >>> 0;
  }

  /** @returns {tag: string, body: Uint8Array} */
  readMessage() {
    const tag = String.fromCharCode(this.readByte());
    const len = this.readInt32();
    const body = this.readBytes(len - 4);
    return {tag, body};
  }
}

function bodyReadCString(body, offset) {
  let end = offset;
  while (body[end] !== 0) end++;
  return {value: utf8Decode(body, offset, end), next: end + 1};
}

function bodyReadInt32(body, offset) {
  return ((body[offset] << 24) | (body[offset + 1] << 16) |
         (body[offset + 2] << 8) | body[offset + 3]) >>>
        0;
}

// ---------- SCRAM-SHA-256 ----------

function xorBytes(a, b) {
  const out = new Uint8Array(a.length);
  for (let i = 0; i < a.length; i++) out[i] = a[i] ^ b[i];
  return out;
}

/** PBKDF2-HMAC-SHA256 with dkLen == hLen (32) -- SCRAM's only use case,
 * so this is just the single-block F() construction, not general PBKDF2. */
function pbkdf2Sha256One(password, salt, iterations) {
  const saltPlus1 = new Uint8Array(salt.length + 4);
  saltPlus1.set(salt, 0);
  saltPlus1[salt.length] = 0;
  saltPlus1[salt.length + 1] = 0;
  saltPlus1[salt.length + 2] = 0;
  saltPlus1[salt.length + 3] = 1;

  let u = pljsFdwNet.hmacSha256(password, saltPlus1);
  let result = u;

  for (let i = 1; i < iterations; i++) {
    u = pljsFdwNet.hmacSha256(password, u);
    result = xorBytes(result, u);
  }

  return result;
}

function randomNonce() {
  let s = '';
  for (let i = 0; i < 24; i++) {
    s += B64_CHARS[Math.floor(Math.random() * 64)];
  }
  return s;
}

function bytesToStr(bytes) {
  return utf8Decode(bytes, 0, bytes.length);
}

function scramAuth(sock, reader, password) {
  const clientNonce = randomNonce();
  const clientFirstBare = 'n=,r=' + clientNonce;
  const gs2Header = 'n,,';

  pljsFdwNet.send(
      sock, buildMessage('p', (w) => {
        w.cstr('SCRAM-SHA-256');
        const resp = gs2Header + clientFirstBare;
        w.i32(resp.length);
        w.raw(utf8Encode(resp));
      }));

  const cont = reader.readMessage();
  if (cont.tag !== 'R' || bodyReadInt32(cont.body, 0) !== 11) {
    throw new Error('pgwire: expected AuthenticationSASLContinue');
  }

  const serverFirst = bytesToStr(cont.body.slice(4));
  const rMatch = /r=([^,]*)/.exec(serverFirst);
  const sMatch = /s=([^,]*)/.exec(serverFirst);
  const iMatch = /i=(\d+)/.exec(serverFirst);

  if (!rMatch || !sMatch || !iMatch) {
    throw new Error('pgwire: malformed server-first-message');
  }

  const combinedNonce = rMatch[1];
  const salt = base64Decode(sMatch[1]);
  const iterations = parseInt(iMatch[1], 10);

  if (combinedNonce.indexOf(clientNonce) !== 0) {
    throw new Error('pgwire: server nonce does not extend client nonce');
  }

  const passwordBytes = new Uint8Array(utf8Encode(password));
  const saltedPassword = pbkdf2Sha256One(passwordBytes, salt, iterations);
  const clientKey = pljsFdwNet.hmacSha256(
      saltedPassword, new Uint8Array(utf8Encode('Client Key')));
  const storedKey = pljsFdwNet.sha256(clientKey);

  const clientFinalWithoutProof =
      'c=' + base64Encode(Array.from(utf8Encode(gs2Header))) +
      ',r=' + combinedNonce;
  const authMessage = clientFirstBare + ',' + serverFirst + ',' +
      clientFinalWithoutProof;

  const clientSignature =
      pljsFdwNet.hmacSha256(storedKey, new Uint8Array(utf8Encode(authMessage)));
  const clientProof = xorBytes(clientKey, clientSignature);
  const clientFinal =
      clientFinalWithoutProof + ',p=' + base64Encode(Array.from(clientProof));

  pljsFdwNet.send(sock, buildMessage('p', (w) => {
                    w.raw(utf8Encode(clientFinal));
                  }));

  const final = reader.readMessage();
  if (final.tag !== 'R' || bodyReadInt32(final.body, 0) !== 12) {
    throw new Error('pgwire: expected AuthenticationSASLFinal');
  }

  const serverFinal = bytesToStr(final.body.slice(4));
  const vMatch = /v=([^,]*)/.exec(serverFinal);
  const serverKey = pljsFdwNet.hmacSha256(
      saltedPassword, new Uint8Array(utf8Encode('Server Key')));
  const expectedServerSignature = pljsFdwNet.hmacSha256(
      serverKey, new Uint8Array(utf8Encode(authMessage)));

  if (!vMatch || base64Encode(Array.from(expectedServerSignature)) !==
                     vMatch[1]) {
    throw new Error('pgwire: server signature verification failed');
  }
}

// ---------- connection ----------

function pgError(body) {
  let message = 'unknown error';
  let offset = 0;

  while (body[offset] !== 0) {
    const field = String.fromCharCode(body[offset]);
    const {value, next} = bodyReadCString(body, offset + 1);
    offset = next;
    if (field === 'M') message = value;
  }

  return new Error('pgwire: ' + message);
}

class PgConnection {
  constructor(options) {
    this.sock = pljsFdwNet.connect(options.host || '127.0.0.1',
                                   options.port || 5432);
    this.reader = new SocketReader(this.sock);
    this._closed = false;

    pljsFdwNet.send(
        this.sock, buildMessage(null, (w) => {
          w.i32(0x00030000);
          w.cstr('user');
          w.cstr(options.user);
          w.cstr('database');
          w.cstr(options.database || options.user);
          w.u8(0);
        }));

    this._authenticate(options);
    this._drainToReady();
  }

  _authenticate(options) {
    const msg = this.reader.readMessage();

    if (msg.tag === 'E') {
      throw pgError(msg.body);
    }

    if (msg.tag !== 'R') {
      throw new Error('pgwire: expected authentication message, got ' +
                      msg.tag);
    }

    const authType = bodyReadInt32(msg.body, 0);

    if (authType === 0) {
      return; // AuthenticationOk -- trust, or already satisfied
    } else if (authType === 3) {
      pljsFdwNet.send(this.sock, buildMessage('p', (w) => {
                        w.cstr(options.password);
                      }));
    } else if (authType === 10) {
      scramAuth(this.sock, this.reader, options.password);
    } else if (authType === 5) {
      throw new Error(
          'pgwire: md5 password auth is not supported (use scram-sha-256 or trust)');
    } else {
      throw new Error('pgwire: unsupported auth type ' + authType);
    }

    const ok = this.reader.readMessage();
    if (ok.tag === 'E') throw pgError(ok.body);
    if (ok.tag !== 'R' || bodyReadInt32(ok.body, 0) !== 0) {
      throw new Error('pgwire: authentication failed');
    }
  }

  /** Reads ParameterStatus/BackendKeyData/NoticeResponse until ReadyForQuery. */
  _drainToReady() {
    for (;;) {
      const msg = this.reader.readMessage();

      if (msg.tag === 'Z') {
        return;
      } else if (msg.tag === 'E') {
        throw pgError(msg.body);
      }
      // S (ParameterStatus), K (BackendKeyData), N (NoticeResponse): ignored.
    }
  }

  /**
   * @param sql query text with $1, $2, ... placeholders
   * @param params array of parameter values (stringified; null -> SQL NULL)
   * @returns {rows: [{col: 'text value', ...}, ...], rowCount, command}
   */
  query(sql, params) {
    params = params || [];

    pljsFdwNet.send(
        this.sock, buildMessage('P', (w) => {
          w.cstr(''); // unnamed statement
          w.cstr(sql);
          w.i16(0); // 0 = infer all parameter types
        }));

    pljsFdwNet.send(
        this.sock, buildMessage('B', (w) => {
          w.cstr(''); // unnamed portal
          w.cstr(''); // unnamed statement
          w.i16(0); // 0 parameter format codes -> all text
          w.i16(params.length);
          for (const p of params) {
            if (p === null || p === undefined) {
              w.i32(-1);
            } else {
              const bytes = utf8Encode(String(p));
              w.i32(bytes.length);
              w.raw(bytes);
            }
          }
          w.i16(0); // 0 result format codes -> all text
        }));

    pljsFdwNet.send(this.sock, buildMessage('D', (w) => {
                      w.u8('P'.charCodeAt(0));
                      w.cstr(''); // unnamed portal
                    }));

    pljsFdwNet.send(this.sock, buildMessage('E', (w) => {
                      w.cstr(''); // unnamed portal
                      w.i32(0); // no row limit
                    }));

    pljsFdwNet.send(this.sock, buildMessage('S', () => {}));

    let columns = null;
    const rows = [];
    let rowCount = 0;
    let command = null;

    for (;;) {
      const msg = this.reader.readMessage();

      if (msg.tag === '1' || msg.tag === '2') {
        // ParseComplete, BindComplete
        continue;
      } else if (msg.tag === 'T') {
        columns = this._parseRowDescription(msg.body);
      } else if (msg.tag === 'D') {
        rows.push(this._parseDataRow(msg.body, columns));
      } else if (msg.tag === 'C') {
        const {value} = bodyReadCString(msg.body, 0);
        command = value;
        const m = /(\d+)$/.exec(value);
        rowCount = m ? parseInt(m[1], 10) : rows.length;
      } else if (msg.tag === 'E') {
        this._drainToReady();
        throw pgError(msg.body);
      } else if (msg.tag === 'Z') {
        break;
      }
      // I (EmptyQueryResponse), N (NoticeResponse): ignored.
    }

    return {rows, rowCount, command};
  }

  _parseRowDescription(body) {
    const count = (body[0] << 8) | body[1];
    const columns = [];
    let offset = 2;

    for (let i = 0; i < count; i++) {
      const {value: name, next} = bodyReadCString(body, offset);
      offset = next + 18; // tableOid(4)+colAttNum(2)+typeOid(4)+typeLen(2)+typmod(4)+formatCode(2)
      columns.push(name);
    }

    return columns;
  }

  _parseDataRow(body, columns) {
    const count = (body[0] << 8) | body[1];
    let offset = 2;
    const row = {};

    for (let i = 0; i < count; i++) {
      const len = bodyReadInt32(body, offset);
      offset += 4;

      if (len === -1) {
        row[columns[i]] = null;
      } else {
        row[columns[i]] = utf8Decode(body, offset, offset + len);
        offset += len;
      }
    }

    return row;
  }

  close() {
    if (this._closed) return;
    this._closed = true;
    try {
      pljsFdwNet.send(this.sock, buildMessage('X', () => {}));
    } catch (e) {
      // best-effort
    }
    pljsFdwNet.close(this.sock);
  }
}

return {PgConnection};

})();
