import crypto from "node:crypto";

const NUMBER = /^-?(?:0|[1-9][0-9]*)(?:\.[0-9]+)?(?:[eE][+-]?[0-9]+)?/;
const UTF8 = new TextDecoder("utf-8", { fatal: true });
const HEX_DIGEST = /^[0-9a-f]{64}$/;

function reject(message, offset) {
  throw new Error(offset === undefined ? message : `${message} at byte ${offset}`);
}

function scalarString(value, label = "string") {
  if (typeof value !== "string") reject(`${label} must be a string`);
  for (let i = 0; i < value.length; i += 1) {
    const unit = value.charCodeAt(i);
    if (unit >= 0xd800 && unit <= 0xdbff) {
      const low = value.charCodeAt(i + 1);
      if (!(low >= 0xdc00 && low <= 0xdfff)) reject(`${label} contains a lone high surrogate`);
      i += 1;
    } else if (unit >= 0xdc00 && unit <= 0xdfff) {
      reject(`${label} contains a lone low surrogate`);
    }
  }
  return value;
}

function encode(value, active) {
  if (value === null) return "null";
  if (typeof value === "boolean") return value ? "true" : "false";
  if (typeof value === "string") return JSON.stringify(scalarString(value));
  if (typeof value === "number") {
    if (!Number.isFinite(value)) reject("JCS rejects non-finite numbers");
    return Object.is(value, -0) ? "0" : JSON.stringify(value);
  }
  if (["undefined", "function", "symbol", "bigint"].includes(typeof value)) {
    reject(`JCS rejects ${typeof value} values`);
  }
  if (active.has(value)) reject("JCS rejects cyclic values");
  active.add(value);
  try {
    if (Array.isArray(value)) {
      if (Object.getPrototypeOf(value) !== Array.prototype) reject("JCS accepts only ordinary arrays");
      if (Object.getOwnPropertySymbols(value).length) reject("JCS rejects symbol properties");
      const ownKeys = Reflect.ownKeys(value);
      if (ownKeys.length !== value.length + 1 || ownKeys[value.length] !== "length") {
        reject("JCS rejects sparse arrays or extra array properties");
      }
      const enumerable = Object.keys(value);
      if (enumerable.length !== value.length) reject("JCS rejects sparse arrays or extra array properties");
      const parts = [];
      for (let i = 0; i < value.length; i += 1) {
        if (!Object.hasOwn(value, i) || enumerable[i] !== String(i)) reject("JCS rejects sparse arrays or extra array properties");
        const descriptor = Object.getOwnPropertyDescriptor(value, String(i));
        if (!descriptor || !("value" in descriptor) || !descriptor.enumerable) reject("JCS rejects array accessors");
        parts.push(encode(descriptor.value, active));
      }
      return `[${parts.join(",")}]`;
    }
    const prototype = Object.getPrototypeOf(value);
    if (prototype !== Object.prototype && prototype !== null) reject("JCS accepts only plain JSON objects");
    if (Object.getOwnPropertySymbols(value).length) reject("JCS rejects symbol properties");
    const keys = Reflect.ownKeys(value);
    if (keys.some((key) => typeof key !== "string")) reject("JCS rejects symbol properties");
    const members = [];
    for (const key of [...keys].sort()) {
      scalarString(key, "object key");
      const descriptor = Object.getOwnPropertyDescriptor(value, key);
      if (!descriptor || !("value" in descriptor) || !descriptor.enumerable) reject("JCS rejects accessors and hidden properties");
      members.push(`${JSON.stringify(key)}:${encode(descriptor.value, active)}`);
    }
    return `{${members.join(",")}}`;
  } finally {
    active.delete(value);
  }
}

// RFC 8785 operates on the parsed JSON data model. JavaScript-only values are
// rejected rather than coerced through toJSON, elided, or emitted as invalid JSON.
export function jcs(value) {
  return encode(value, new Set());
}

// Stored canonical documents are exactly JCS bytes; publication adds no LF.
export const canonical = jcs;
export const sha256 = (bytes) => crypto.createHash("sha256").update(bytes).digest("hex");

export function domainPreimage(domain, value) {
  if (typeof domain !== "string" || !domain || domain.includes("\0") || !/^[\x20-\x7e]+$/.test(domain)) reject("digest domain must be nonempty printable ASCII without NUL");
  return Buffer.concat([Buffer.from(domain, "ascii"), Buffer.from([0]), Buffer.from(jcs(value), "utf8")]);
}

export const digest = (domain, value) => sha256(domainPreimage(domain, value));

export function schemaPreimage(schemaWithoutSelfId) {
  return domainPreimage("daw-schema-v1", schemaWithoutSelfId);
}

export function schemaId(schemaWithoutSelfId) {
  return sha256(schemaPreimage(schemaWithoutSelfId));
}

export function documentPreimage(schemaIdHex, documentWithoutId) {
  if (typeof schemaIdHex !== "string" || !HEX_DIGEST.test(schemaIdHex)) reject("schema ID must be 64 lowercase hexadecimal characters");
  return Buffer.concat([
    Buffer.from("daw-doc-v1\0", "ascii"),
    Buffer.from(schemaIdHex, "hex"),
    Buffer.from([0]),
    Buffer.from(jcs(documentWithoutId), "utf8"),
  ]);
}

export function documentId(schemaIdHex, documentWithoutId) {
  return sha256(documentPreimage(schemaIdHex, documentWithoutId));
}

class StrictParser {
  constructor(input) {
    if (typeof input === "string") this.text = input;
    else {
      try { this.text = UTF8.decode(Buffer.from(input)); }
      catch { reject("JSON bytes are not well-formed UTF-8"); }
    }
    this.offset = 0;
  }

  parse() {
    this.space();
    const value = this.value();
    this.space();
    if (this.offset !== this.text.length) reject("unexpected trailing JSON data", this.offset);
    return value;
  }

  space() {
    while (/[\u0009\u000a\u000d\u0020]/.test(this.text[this.offset] ?? "")) this.offset += 1;
  }

  value() {
    const token = this.text[this.offset];
    if (token === "{") return this.object();
    if (token === "[") return this.array();
    if (token === "\"") return this.string();
    if (this.text.startsWith("true", this.offset)) return this.literal("true", true);
    if (this.text.startsWith("false", this.offset)) return this.literal("false", false);
    if (this.text.startsWith("null", this.offset)) return this.literal("null", null);
    if (token === "-" || /[0-9]/.test(token ?? "")) return this.number();
    reject("expected JSON value", this.offset);
  }

  literal(token, value) { this.offset += token.length; return value; }

  string() {
    const start = this.offset++;
    let escaped = false;
    while (this.offset < this.text.length) {
      const unit = this.text.charCodeAt(this.offset);
      if (!escaped && unit === 0x22) {
        this.offset += 1;
        let parsed;
        try { parsed = JSON.parse(this.text.slice(start, this.offset)); }
        catch { reject("invalid JSON string", start); }
        return scalarString(parsed, "JSON string");
      }
      if (!escaped && unit < 0x20) reject("unescaped JSON control character", this.offset);
      if (!escaped && unit === 0x5c) escaped = true;
      else escaped = false;
      this.offset += 1;
    }
    reject("unterminated JSON string", start);
  }

  number() {
    const token = this.text.slice(this.offset).match(NUMBER)?.[0];
    if (!token) reject("invalid JSON number", this.offset);
    this.offset += token.length;
    const value = Number(token);
    if (!Number.isFinite(value)) reject("JSON number is not finite", this.offset);
    return value;
  }

  array() {
    this.offset += 1; this.space();
    const result = [];
    if (this.text[this.offset] === "]") { this.offset += 1; return result; }
    while (true) {
      result.push(this.value()); this.space();
      if (this.text[this.offset] === "]") { this.offset += 1; return result; }
      if (this.text[this.offset] !== ",") reject("expected ',' or ']'", this.offset);
      this.offset += 1; this.space();
    }
  }

  object() {
    this.offset += 1; this.space();
    const result = Object.create(null); const seen = new Set();
    if (this.text[this.offset] === "}") { this.offset += 1; return result; }
    while (true) {
      if (this.text[this.offset] !== "\"") reject("expected object key", this.offset);
      const key = this.string();
      if (seen.has(key)) reject(`duplicate JSON key ${JSON.stringify(key)}`, this.offset);
      seen.add(key); this.space();
      if (this.text[this.offset] !== ":") reject("expected ':'", this.offset);
      this.offset += 1; this.space();
      Object.defineProperty(result, key, { value: this.value(), enumerable: true, writable: true, configurable: true });
      this.space();
      if (this.text[this.offset] === "}") { this.offset += 1; return result; }
      if (this.text[this.offset] !== ",") reject("expected ',' or '}'", this.offset);
      this.offset += 1; this.space();
    }
  }
}

export function parseJsonRejectingDuplicates(input) {
  return new StrictParser(input).parse();
}

export function parseCanonicalJson(input) {
  const text = typeof input === "string" ? input : UTF8.decode(Buffer.from(input));
  const value = parseJsonRejectingDuplicates(text);
  if (jcs(value) !== text) reject("stored JSON is not byte-for-byte RFC 8785 JCS");
  return value;
}

export function canonicalDecimal(value, { signed = false, bits = 64 } = {}) {
  const grammar = signed ? /^(0|-?[1-9][0-9]*)$/ : /^(0|[1-9][0-9]*)$/;
  if (typeof value !== "string" || !grammar.test(value)) reject("integer is not a canonical decimal string");
  const parsed = BigInt(value); const width = BigInt(bits);
  const minimum = signed ? -(1n << (width - 1n)) : 0n;
  const maximum = signed ? (1n << (width - 1n)) - 1n : (1n << width) - 1n;
  if (parsed < minimum || parsed > maximum) reject(`integer is outside ${signed ? "signed" : "unsigned"} ${bits}-bit range`);
  return parsed;
}

export function assertSortedUnique(values, key, label = "array") {
  let previous;
  for (const value of values) {
    const current = key(value);
    if (previous !== undefined && previous >= current) reject(`${label} is unsorted or contains a duplicate key`);
    previous = current;
  }
  return values;
}
