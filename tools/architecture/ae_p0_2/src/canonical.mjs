import crypto from "node:crypto";
import { types as utilTypes } from "node:util";

const NUMBER = /^-?(?:0|[1-9][0-9]*)(?:\.[0-9]+)?(?:[eE][+-]?[0-9]+)?/;
const UTF8 = new TextDecoder("utf-8", { fatal: true, ignoreBOM: true });
const HEX_DIGEST = /^[0-9a-f]{64}$/;
const TYPED_ARRAY_PROTOTYPE = Object.getPrototypeOf(Uint8Array.prototype);
const TYPED_ARRAY_BUFFER = Object.getOwnPropertyDescriptor(TYPED_ARRAY_PROTOTYPE, "buffer").get;
const TYPED_ARRAY_BYTE_LENGTH = Object.getOwnPropertyDescriptor(TYPED_ARRAY_PROTOTYPE, "byteLength").get;
const TYPED_ARRAY_BYTE_OFFSET = Object.getOwnPropertyDescriptor(TYPED_ARRAY_PROTOTYPE, "byteOffset").get;
const TYPED_ARRAY_SET = TYPED_ARRAY_PROTOTYPE.set;

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

export function snapshotBytes(input, label = "input") {
  if (input !== null && (typeof input === "object" || typeof input === "function") && utilTypes.isProxy(input)) {
    reject(`${label} bytes must not be a Proxy`);
  }
  if (!utilTypes.isUint8Array(input)) reject(`${label} must be a Uint8Array`);

  let backing;
  let byteLength;
  let byteOffset;
  try {
    // Invoke captured intrinsic getters against the internal slots. Ordinary
    // property reads such as input.length/input.buffer are forbidden here:
    // typed arrays can shadow them with attacker-controlled accessors.
    backing = Reflect.apply(TYPED_ARRAY_BUFFER, input, []);
    byteLength = Reflect.apply(TYPED_ARRAY_BYTE_LENGTH, input, []);
    byteOffset = Reflect.apply(TYPED_ARRAY_BYTE_OFFSET, input, []);
  } catch {
    reject(`${label} must be an attached Uint8Array`);
  }
  if (utilTypes.isSharedArrayBuffer(backing)) reject(`${label} must not use SharedArrayBuffer storage`);

  try {
    const source = new Uint8Array(backing, byteOffset, byteLength);
    const copy = new Uint8Array(byteLength);
    Reflect.apply(TYPED_ARRAY_SET, copy, [source]);
    const copyBacking = Reflect.apply(TYPED_ARRAY_BUFFER, copy, []);
    return Buffer.from(copyBacking, 0, byteLength);
  } catch {
    reject(`${label} could not be copied from a stable byte range`);
  }
}

export function decodeUtf8RejectingBom(input, label = "input") {
  if (typeof input === "string") {
    if (input.charCodeAt(0) === 0xfeff) reject(`${label} starts with a UTF-8 BOM`);
    return input;
  }
  const bytes = snapshotBytes(input, label);
  // TextDecoder normally consumes an initial BOM. Inspect the undecoded bytes so
  // byte-for-byte canonical validation can never erase the evidence first.
  if (bytes.length >= 3 && bytes[0] === 0xef && bytes[1] === 0xbb && bytes[2] === 0xbf) {
    reject(`${label} starts with a UTF-8 BOM`);
  }
  try {
    return UTF8.decode(bytes);
  } catch {
    reject(`${label} bytes are not well-formed UTF-8`);
  }
}

function encode(value, active) {
  const kind = typeof value;
  if (value !== null && (kind === "object" || kind === "function") && utilTypes.isProxy(value)) {
    // This must precede Array.isArray, getPrototypeOf, ownKeys, descriptors, or
    // any other reflective operation: a revoked Proxy throws and a live Proxy
    // can run user traps at each of those boundaries.
    reject("JCS rejects Proxy values");
  }
  if (value === null) return "null";
  if (kind === "boolean") return value ? "true" : "false";
  if (kind === "string") return JSON.stringify(scalarString(value));
  if (kind === "number") {
    if (!Number.isFinite(value)) reject("JCS rejects non-finite numbers");
    return Object.is(value, -0) ? "0" : JSON.stringify(value);
  }
  if (["undefined", "function", "symbol", "bigint"].includes(kind)) {
    reject(`JCS rejects ${kind} values`);
  }
  if (active.has(value)) reject("JCS rejects cyclic values");
  active.add(value);
  try {
    if (Array.isArray(value)) {
      if (Object.getPrototypeOf(value) !== Array.prototype) reject("JCS accepts only ordinary arrays");
      const ownKeys = Reflect.ownKeys(value);
      if (ownKeys.some((key) => typeof key !== "string")) reject("JCS rejects symbol properties");
      const lengthDescriptor = Object.getOwnPropertyDescriptor(value, "length");
      if (!lengthDescriptor || !("value" in lengthDescriptor) || lengthDescriptor.enumerable) {
        reject("JCS accepts only ordinary arrays");
      }
      const length = lengthDescriptor.value;
      if (ownKeys.length !== length + 1) {
        reject("JCS rejects sparse arrays or extra array properties");
      }
      const parts = [];
      for (let i = 0; i < length; i += 1) {
        const descriptor = Object.getOwnPropertyDescriptor(value, String(i));
        if (!descriptor || !("value" in descriptor) || !descriptor.enumerable) reject("JCS rejects array accessors");
        parts.push(encode(descriptor.value, active));
      }
      return `[${parts.join(",")}]`;
    }
    const prototype = Object.getPrototypeOf(value);
    if (prototype !== Object.prototype && prototype !== null) reject("JCS accepts only plain JSON objects");
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

export function schemaWithoutSelfId(schema) {
  if (schema !== null && (typeof schema === "object" || typeof schema === "function") && utilTypes.isProxy(schema)) {
    reject("schema must not be a Proxy");
  }
  if (schema === null || typeof schema !== "object" || Array.isArray(schema)) reject("schema must be a plain object");
  const prototype = Object.getPrototypeOf(schema);
  if (prototype !== Object.prototype && prototype !== null) reject("schema must be a plain object");

  const body = Object.create(null);
  let foundSelfId = false;
  for (const key of Reflect.ownKeys(schema)) {
    if (typeof key !== "string") reject("schema rejects symbol properties");
    scalarString(key, "schema key");
    const descriptor = Object.getOwnPropertyDescriptor(schema, key);
    if (!descriptor || !("value" in descriptor) || !descriptor.enumerable) {
      reject("schema rejects accessors and hidden properties");
    }
    if (key === "$id") {
      if (typeof descriptor.value !== "string") reject("schema top-level $id must be a string");
      scalarString(descriptor.value, "schema top-level $id");
      foundSelfId = true;
      continue;
    }
    Object.defineProperty(body, key, {
      value: descriptor.value,
      enumerable: true,
      writable: true,
      configurable: true,
    });
  }
  if (!foundSelfId) reject("schema must contain one top-level $id self-identifier");
  return body;
}

export function schemaPreimage(schema) {
  return domainPreimage("daw-schema-v1", schemaWithoutSelfId(schema));
}

export function schemaId(schema) {
  return sha256(schemaPreimage(schema));
}

export function documentPreimage(schemaIdHex, documentWithoutId) {
  if (typeof schemaIdHex !== "string" || schemaIdHex.length !== 64 || !HEX_DIGEST.test(schemaIdHex)) {
    reject("schema ID must be 64 lowercase hexadecimal characters");
  }
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
    this.text = decodeUtf8RejectingBom(input, "JSON");
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
  const text = decodeUtf8RejectingBom(input, "JSON");
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
