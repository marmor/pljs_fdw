// xml_fdw: a read-only FDW exposing rows parsed out of a local XML file.
// The pljs_fdw port of Multicorn's xmlfdw.py, which wraps Python's stdlib
// xml.sax -- nothing in the original is outdated, so this is a faithful
// behavioral port of a working reference, not a modernization. QuickJS has
// no XML parser at all, so the SAX-equivalent scanning is hand-written
// below (see parseXmlRows) rather than a thin wrapper around a library.
//
// Options:
//   filename -- full path to the XML file (required)
//   elemTag  -- the tag name marking each row's boundary; every occurrence
//               of this tag becomes one row (required)
//
// Shape, matching the original exactly: for each <elemTag> occurrence, its
// DIRECT child elements whose tag name matches a wanted column become that
// column's value, as text content -- e.g.
//   <items><item><id>1</id><name>widget</name></item>...</items>
// with elemTag='item' and columns id/name maps each <item> to one row.
// Attributes are not read (matching the original, which never looks at
// SAX's attrs argument either). Grandchildren (markup nested inside a
// captured column) are flattened to their text content rather than
// erroring or being dropped -- e.g. <name>a <b>bold</b> word</name> yields
// "a bold word" for `name`. The buffer_size option from the original
// (a streaming chunk size for Python's incremental parser) doesn't apply
// here: pljsFdwFs.readFile() has no partial-read API, so the whole file is
// read and scanned in one pass regardless.
//
// This is a MANDATORY divergence, not a style choice (see
// docs/ARCHITECTURE.md's "A porting hazard worth calling out explicitly"
// section, discovered while porting csv_fdw): the original's execute()
// takes `quals` and never filters by them, relying on Multicorn/Postgres
// to recheck locally -- safe under Multicorn's model, not under
// pljs_fdw's, where an unhandled-but-recognized qual is trusted and
// skipped from any local recheck. This module filters by `quals` itself
// for exactly that reason.
module.exports = (function() {

// ---------- name / character classification (no RegExp; see the
// pljsFdwFs/RegExp hazard note in fs.c and docs/ARCHITECTURE.md) ----------

function isNameStartCode(code) {
  return (code >= 65 && code <= 90) ||     // A-Z
      (code >= 97 && code <= 122) ||       // a-z
      code === 95 || code === 58;          // _ :
}

function isNameCode(code) {
  return isNameStartCode(code) || (code >= 48 && code <= 57) || code === 45 ||
      code === 46; // 0-9 - .
}

function readName(text, i) {
  const n = text.length;
  const start = i;

  if (i < n && isNameStartCode(text.charCodeAt(i))) {
    i += 1;
    while (i < n && isNameCode(text.charCodeAt(i))) i += 1;
  }

  return {name: text.slice(start, i), next: i};
}

/** @p i must point just past a tag name. Skips attributes (including
 * quoted values, which may themselves contain '>') up to the tag's own
 * closing '>' or self-closing '/>'. */
function skipToTagEnd(text, i) {
  const n = text.length;
  let selfClosing = false;

  while (i < n) {
    const c = text[i];

    if (c === '>') {
      i += 1;
      break;
    } else if (c === '/' && text[i + 1] === '>') {
      selfClosing = true;
      i += 2;
      break;
    } else if (c === '"' || c === '\'') {
      const quote = c;
      i += 1;
      while (i < n && text[i] !== quote) i += 1;
      i += 1;
    } else {
      i += 1;
    }
  }

  return {selfClosing, next: i};
}

/** Decodes the five predefined XML entities plus decimal/hex numeric
 * character references (&#NN; / &#xHH;). Anything else (an unknown named
 * entity, or a bare unmatched '&') is left exactly as written. */
function decodeEntities(s) {
  if (s.indexOf('&') === -1) return s;

  let out = '';
  let i = 0;
  const n = s.length;

  while (i < n) {
    const c = s[i];

    if (c !== '&') {
      out += c;
      i += 1;
      continue;
    }

    const semi = s.indexOf(';', i + 1);

    if (semi === -1) {
      out += c;
      i += 1;
      continue;
    }

    const ent = s.slice(i + 1, semi);
    let decoded = null;

    if (ent === 'amp') decoded = '&';
    else if (ent === 'lt') decoded = '<';
    else if (ent === 'gt') decoded = '>';
    else if (ent === 'quot') decoded = '"';
    else if (ent === 'apos') decoded = '\'';
    else if (ent[0] === '#') {
      const isHex = ent[1] === 'x' || ent[1] === 'X';
      const code = isHex ? parseInt(ent.slice(2), 16) : parseInt(ent.slice(1), 10);
      if (!isNaN(code)) decoded = String.fromCharCode(code);
    }

    if (decoded !== null) {
      out += decoded;
      i = semi + 1;
    } else {
      out += c;
      i += 1;
    }
  }

  return out;
}

/**
 * @brief Scans @p text for occurrences of @p elemTag, returning one row
 * object per occurrence with a property per direct-child element whose
 * tag name is in @p columns (an array of local column names).
 */
function parseXmlRows(text, elemTag, columns) {
  const columnSet = {};
  for (const c of columns) columnSet[c] = true;

  const rows = [];
  let rootSeen = 0;
  let currentRow = null;

  // While inside a captured column's element, all further text (including
  // inside any nested markup) accumulates into activeText; activeDepth
  // tracks nesting so only that same element's real closing tag ends the
  // capture, not a coincidentally-named tag elsewhere.
  let activeColumn = null;
  let activeDepth = 0;
  let activeText = '';

  function handleStart(name, selfClosing) {
    if (activeColumn !== null) {
      if (!selfClosing) activeDepth += 1;
      return;
    }

    if (name === elemTag) {
      rootSeen += 1;

      if (rootSeen === 1) {
        currentRow = {};
      }

      if (selfClosing && rootSeen === 1) {
        rows.push(currentRow);
        currentRow = null;
        rootSeen = 0;
      } else if (selfClosing) {
        rootSeen -= 1;
      }

      return;
    }

    if (rootSeen === 1 && columnSet[name]) {
      activeColumn = name;
      activeText = '';

      if (selfClosing) {
        currentRow[name] = '';
        activeColumn = null;
      } else {
        activeDepth = 1;
      }
    }
  }

  function handleEnd(name) {
    if (activeColumn !== null) {
      activeDepth -= 1;

      if (activeDepth === 0) {
        currentRow[activeColumn] = activeText;
        activeColumn = null;
        activeText = '';
      }

      return;
    }

    if (name === elemTag) {
      rootSeen -= 1;

      if (rootSeen === 0 && currentRow !== null) {
        rows.push(currentRow);
        currentRow = null;
      }
    }
  }

  function handleText(str, rawLiteral) {
    if (activeColumn !== null) {
      activeText += rawLiteral ? str : decodeEntities(str);
    }
  }

  const n = text.length;
  let i = 0;

  while (i < n) {
    if (text[i] !== '<') {
      const lt = text.indexOf('<', i);
      const end = lt === -1 ? n : lt;
      handleText(text.slice(i, end), false);
      i = end;
      continue;
    }

    if (text.slice(i, i + 4) === '<!--') {
      const end = text.indexOf('-->', i + 4);
      i = end === -1 ? n : end + 3;
    } else if (text.slice(i, i + 9) === '<![CDATA[') {
      const end = text.indexOf(']]>', i + 9);
      handleText(text.slice(i + 9, end === -1 ? n : end), true);
      i = end === -1 ? n : end + 3;
    } else if (text[i + 1] === '?') {
      const end = text.indexOf('?>', i + 2);
      i = end === -1 ? n : end + 2;
    } else if (text[i + 1] === '!') {
      const end = text.indexOf('>', i + 2);
      i = end === -1 ? n : end + 1;
    } else if (text[i + 1] === '/') {
      const {name, next} = readName(text, i + 2);
      const gt = text.indexOf('>', next);
      i = gt === -1 ? n : gt + 1;
      handleEnd(name);
    } else {
      const {name, next} = readName(text, i + 1);
      const {selfClosing, next: afterTag} = skipToTagEnd(text, next);
      i = afterTag;
      handleStart(name, selfClosing);
    }
  }

  return rows;
}

// ---------- qual filtering (mandatory here; see file header) ----------

function toNumIfNumeric(v) {
  if (typeof v === 'number') return v;
  if (typeof v === 'string' && v.trim() !== '' && !isNaN(v)) return Number(v);
  return v;
}

function evalQual(row, qual) {
  const rawV = toNumIfNumeric(row[qual.column]);
  const rawT = toNumIfNumeric(qual.value);
  const bothNumeric = typeof rawV === 'number' && typeof rawT === 'number';
  const v = bothNumeric ? rawV : String(rawV);
  const t = bothNumeric ? rawT : String(rawT);

  switch (qual.operator) {
    case '=': return v === t;
    case '<>': return v !== t;
    case '<': return v < t;
    case '<=': return v <= t;
    case '>': return v > t;
    case '>=': return v >= t;
    default: return true;
  }
}

class XmlFdw {
  constructor(options) {
    if (!options.filename) {
      throw new Error('xml_fdw: "filename" option is required');
    }
    if (!options.elemTag) {
      throw new Error('xml_fdw: "elemTag" option is required');
    }

    this.filename = options.filename;
    this.elemTag = options.elemTag;
  }

  *execute(quals, columns, sortkeys, columnMap) {
    // A local column's real XML tag name can differ from its Postgres name
    // (the standard pljs_fdw column_name mapping, see docs/FDW_API.md) --
    // parseXmlRows needs to look for the REMOTE (tag) names, and the
    // resulting rows need translating back to local names before quals
    // (also keyed by local name) can be evaluated or the row yielded.
    const remoteNames =
        columns.map((c) => (columnMap && columnMap[c]) || c);
    const text = pljsFdwFs.readFile(this.filename);
    const remoteRows = parseXmlRows(text, this.elemTag, remoteNames);

    for (const remoteRow of remoteRows) {
      const row = {};

      for (const localName of columns) {
        const remoteName = (columnMap && columnMap[localName]) || localName;
        row[localName] = remoteRow[remoteName];
      }

      if (!quals || quals.every((q) => evalQual(row, q))) {
        yield row;
      }
    }
  }
}

return XmlFdw;

})();
