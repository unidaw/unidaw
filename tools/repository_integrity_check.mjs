#!/usr/bin/env node

import {
  chmodSync,
  closeSync,
  constants,
  fstatSync,
  lstatSync,
  mkdtempSync,
  mkdirSync,
  openSync,
  readFileSync,
  readlinkSync,
  readdirSync,
  realpathSync,
  renameSync,
  rmSync,
  symlinkSync,
  unlinkSync,
  writeFileSync,
} from 'node:fs';
import { tmpdir } from 'node:os';
import {
  basename,
  dirname,
  extname,
  isAbsolute,
  join,
  relative,
  resolve,
  sep,
} from 'node:path';
import { spawnSync } from 'node:child_process';
import { fileURLToPath } from 'node:url';

// Direct Node invocation is an internal implementation detail. The supported entrypoint is
// repository_integrity_check.sh, whose shell bootstrap performs lexical no-follow checks before
// sourcing repository_root.sh. Resolve this module for its repository identity; do not inspect
// system ancestors (macOS commonly spells /tmp as a symlink to /private/tmp).
const MODULE_FILE = realpathSync(fileURLToPath(import.meta.url));
const MODULE_REPOSITORY_ROOT = realpathSync(join(dirname(MODULE_FILE), '..'));
const EXPECTED_PACKET_SHA = '258f42359dd3f500df06f4797c4f9d84cb9c4a1b';
const EXPECTED_PRODUCT_BASELINE = '62bafdc6cf1cd53168ce73d098cd6acc78659be8';
const PACKET_PATH = 'docs/architecture/tasks/AE-P0.1-repair-current.md';

// Every tracked regular file is assigned a named live or excluded class. There is no default
// exclusion and no speculative extension list: adding a new class is a reviewable policy change.
// `extensionRule` classes are audited against the real index so a dead extension allowlist cannot
// accumulate. Direct docs/*.md and README.md are live because the registered doc_citation check
// reads exactly those paths; nested architecture packets are not swept by that test.
const LIVE_CLASS_RULES = [
  { id: 'shell-extension', extensions: ['.sh'],
    provenance: 'shell source is executable input even when mode 100644 and without a shebang',
    matches: ({ path }) => extname(path).toLowerCase() === '.sh' },
  { id: 'node-source-extension', extensions: ['.js', '.mjs'],
    provenance: 'Node and browser source/test modules',
    matches: ({ path }) => ['.js', '.mjs'].includes(extname(path).toLowerCase()) },
  { id: 'python-source-extension', extensions: ['.py'],
    provenance: 'Python tools and registered checks',
    matches: ({ path }) => extname(path).toLowerCase() === '.py' },
  { id: 'native-source-extension', extensions: ['.cpp', '.h', '.hpp'],
    provenance: 'C++ build and test source consumed by CMake, and generated C++ headers consumed by registered checks',
    matches: ({ path }) => ['.cpp', '.h', '.hpp'].includes(extname(path).toLowerCase()) },
  { id: 'typescript-source-extension', extensions: ['.ts'],
    provenance: 'TypeScript source; the AE-P0.2 contract emission is compiled and asserted by ae_p0_2_contracts',
    matches: ({ path }) => extname(path).toLowerCase() === '.ts' },
  { id: 'rust-source-extension', extensions: ['.rs'],
    provenance: 'Rust workspace source and build scripts',
    matches: ({ path }) => extname(path).toLowerCase() === '.rs' },
  { id: 'web-source-extension', extensions: ['.css', '.html'],
    provenance: 'web application source consumed by browser and web checks',
    matches: ({ path }) => ['.css', '.html'].includes(extname(path).toLowerCase()) },
  { id: 'json-operational-data', extensions: ['.json'],
    provenance: 'project, preset, package, design-token, and test configuration input',
    matches: ({ path }) => extname(path).toLowerCase() === '.json' },
  { id: 'build-and-repository-config',
    provenance: 'exact build, dependency, ignore, and operational config names',
    matches: ({ path }) => {
      const name = basename(path);
      return name === 'CMakeLists.txt' || name === 'Cargo.toml' || name === 'Cargo.lock'
        || name === '.gitignore' || name === '.dawlint'
        || name === 'comparer_blind_fields.txt';
    } },
  { id: 'registered-operational-markdown',
    provenance: 'README.md, root Markdown and everything under docs/ are consumed by the registered doc_citation check',
    matches: ({ path }) => path.endsWith('.md') &&
      (!path.includes('/') || path.startsWith('docs/')) },
  { id: 'executable-mode',
    provenance: 'Git/worktree executable bit marks an operational entry point',
    matches: ({ executable }) => executable },
  { id: 'shebang',
    provenance: 'a shebang makes an otherwise unknown file executable input',
    matches: ({ bytes }) => bytes.length >= 2 && bytes[0] === 0x23 && bytes[1] === 0x21 },
];

const EXCLUDED_CLASS_RULES = [
  // `ae-p0-2-generated-contracts` was here, reading: "generated wire contracts whose only consumers
  // are AE-P0.2 tests that are NOT registered in ctest — excluded because nothing that runs reads
  // them". Registering `ae_p0_2_contracts` in ctest made that sentence false the moment it landed,
  // so the exclusion went with it and these files are now classified LIVE like anything else a
  // registered check consumes.
  //
  // Second time this exact thing has happened here — `root-design-prose` died the same way, an hour
  // after doc_citation's scope widened to root documents. A provenance is a CLAIM ABOUT THE WORLD,
  // so extending a check's reach is also an edit to every sentence explaining why something sat
  // outside it. Worth expecting rather than rediscovering.

  { id: 'pinned-task-packet',
    provenance: 'the exact task packet is validated byte-for-byte against EXPECTED_PACKET_SHA',
    matches: ({ path }) => path === PACKET_PATH },
  { id: 'opaque-plugin-state',
    provenance: 'binary plugin-state fixtures are never interpreted as paths or executable text',
    matches: ({ path }) => extname(path).toLowerCase() === '.bin' },
  { id: 'audio-fixture',
    provenance: 'WAV files are binary audio fixtures validated by audio-specific checks',
    matches: ({ path }) => extname(path).toLowerCase() === '.wav' },
  { id: 'raster-asset',
    provenance: 'PNG files are binary visual assets',
    matches: ({ path }) => extname(path).toLowerCase() === '.png' },
  { id: 'font-asset',
    provenance: 'WOFF2 files are binary vendored fonts',
    matches: ({ path }) => extname(path).toLowerCase() === '.woff2' },
  // 'root-design-prose' is GONE. It said root Markdown was "design/governance prose not consumed
  // by a registered check", and on 2026-08-14 doc_citation_check's scope widened to root *.md and
  // then to docs/**, which made that sentence false the moment it landed. The files are now
  // classified live for the reason the markdown rule above states, and this exclusion would have
  // gone on asserting they were unchecked. A provenance is a claim about the world, so widening a
  // check's reach is also an edit to whatever explained why something was outside it.
  { id: 'web-design-prose',
    provenance: 'ui-web Markdown is design/user prose not read by the registered web checks',
    matches: ({ path }) => path.startsWith('ui-web/') && path.endsWith('.md') },
];

function classifySurface(path, mode, bytes, executable = mode === '100755') {
  if (path === PACKET_PATH) {
    return { kind: 'excluded', ...EXCLUDED_CLASS_RULES[0] };
  }
  if (extname(path).toLowerCase() === '.pyc') {
    return { kind: 'forbidden', id: 'generated-python-bytecode',
      provenance: 'generated Python bytecode must never be tracked as repository source' };
  }
  const context = { path, mode, bytes, executable };
  for (const rule of LIVE_CLASS_RULES) {
    if (rule.matches(context)) return { kind: 'live', ...rule };
  }
  for (const rule of EXCLUDED_CLASS_RULES) {
    if (rule.matches(context)) return { kind: 'excluded', ...rule };
  }
  return { kind: 'unknown', id: 'unclassified-file',
    provenance: 'tracked files require explicit live or excluded provenance' };
}

const CONTENT_RULES = [
  {
    id: 'user-specific-absolute-path',
    explanation: 'live repository content contains an absolute user-specific checkout path',
    patterns: [
      /\/Users\/[A-Za-z0-9._-]+\/(?:src|source|code|dev|git|github|checkout|checkouts|work|worktrees|workspace|workspaces|repos|Projects|projects)\/[^\s"'`<>]+/g,
      /\/home\/[A-Za-z0-9._-]+\/(?:src|source|code|dev|git|github|checkout|checkouts|work|worktrees|workspace|workspaces|repos|Projects|projects)\/[^\s"'`<>]+/g,
      /\/root\/(?:src|source|code|dev|git|github|checkout|checkouts|work|worktrees|workspace|workspaces|repos|Projects|projects)\/[^\s"'`<>]+/g,
      /[A-Za-z]:[\\/]+Users[\\/]+[A-Za-z0-9._-]+[\\/]+(?:src|source|code|dev|git|github|checkout|checkouts|work|worktrees|workspace|workspaces|repos|Projects|projects)[\\/]+[^\s"'`<>]+/g,
    ],
  },
  {
    id: 'home-derived-checkout-path',
    explanation: 'live repository content derives a checkout path from a user home directory',
    patterns: [
      /(?:^|[^A-Za-z0-9_])(?:~[A-Za-z0-9._-]*|\$HOME|\$\{HOME\}|%USERPROFILE%)[\\/]+(?:src|source|code|dev|git|github|checkout|checkouts|work|worktrees|workspace|workspaces|repos|Projects|projects)[\\/]+(?:[^\\/\s"'`<>]+[\\/]+)*(?:daw(?:-[A-Za-z0-9._-]+)?|frontend|backend|[A-Za-z0-9._-]*(?:checkout|worktree)[A-Za-z0-9._-]*)\b/gim,
      /\b(?:process\.env\.HOME|os\.homedir\(\)|homedir\(\)|Path\.home\(\)|expanduser\([^\n)]*~)[^\n]{0,240}(?:src|source|code|dev|git|github|checkout|checkouts|work|worktrees|workspace|workspaces|repos|Projects|projects)[^\n]{0,240}(?:daw(?:-[A-Za-z0-9._-]+)?|frontend|backend|[A-Za-z0-9._-]*(?:checkout|worktree)[A-Za-z0-9._-]*)\b/g,
    ],
  },
];

const RELATIVE_CHECKOUT_PATTERN = /(?:^|[\s"'`=:(])(?<candidate>(?:\.\.[\\/]+)+(?:[^\\/\s"'`<>]+[\\/]+)*daw(?:-[A-Za-z0-9._-]+)?)(?=$|[\s"'`<>),;]|[\\/]+(?!\.\.\.(?:[\\/]+|[\s"'`<>),;]|$)))/gm;
const RELATIVE_PATH_PATTERN = /(?:^|[\s"'`=:(])(?<candidate>(?:\.\.[\\/]+)+(?:[^\\/\s"'`<>),;]+[\\/]+)*(?:frontend|backend|[A-Za-z0-9._-]*(?:checkout|worktree)[A-Za-z0-9._-]*))(?=$|[\\/\s"'`<>),;])/gim;
const SHELL_PARENT_EXPRESSION_PATTERN = /(?:\$\{?[A-Za-z_][A-Za-z0-9_]*\}?|\$\([^\n)]*dirname[^\n)]*\))[\\/]+\.\.[\\/]+(?:daw(?:-[A-Za-z0-9._-]+)?|frontend|backend|[A-Za-z0-9._-]*(?:checkout|worktree)[A-Za-z0-9._-]*)\b/gim;
const PATH_CALL_START_PATTERN = /\b(?:(?:path\.)?(?:join|resolve)|new\s+URL)\s*\(/g;
const SOURCE_DIRECTORY_NAMES = new Set([
  'src', 'source', 'code', 'dev', 'git', 'github', 'checkout', 'checkouts',
  'work', 'worktrees', 'workspace', 'workspaces', 'repos', 'projects',
]);

function run(command, args, options = {}) {
  const { env: requestedEnvironment = process.env, ...spawnOptions } = options;
  const environment = { ...requestedEnvironment };
  // Local repository inspection must not inherit a caller's alternate index, worktree, object
  // store, or discovery boundary. None of the check's Git commands need any GIT_* input.
  if (command === 'git') {
    for (const name of Object.keys(environment)) {
      if (name.startsWith('GIT_')) delete environment[name];
    }
  }
  const result = spawnSync(command, args, {
    encoding: 'utf8',
    ...spawnOptions,
    env: environment,
  });
  if (result.error) throw result.error;
  return result;
}

function trackedEntries(root) {
  const result = run('git', ['-C', root, 'ls-files', '--stage', '-z']);
  if (result.status !== 0) {
    throw new Error('git ls-files failed while enumerating the tracked repository state');
  }

  const byPath = new Map();
  const unmergedPaths = new Set();
  for (const record of result.stdout.split('\0')) {
    if (!record) continue;
    const tab = record.indexOf('\t');
    if (tab < 0) throw new Error('git ls-files returned an unexpected record');
    const metadata = record.slice(0, tab).split(' ');
    const path = record.slice(tab + 1);
    if (metadata.length !== 3) throw new Error('git ls-files returned unexpected metadata');
    const [mode, oid, stage] = metadata;
    if (stage !== '0') {
      unmergedPaths.add(path);
      continue;
    }
    if (byPath.has(path)) throw new Error('git ls-files returned duplicate stage-0 entries');
    byPath.set(path, { mode, oid, path });
  }
  for (const path of unmergedPaths) byPath.delete(path);
  return {
    entries: [...byPath.values()].sort((a, b) => a.path < b.path ? -1 : a.path > b.path ? 1 : 0),
    unmergedPaths: [...unmergedPaths].sort(),
  };
}

function readTrackedBlob(root, entry) {
  const result = run('git', ['-C', root, 'cat-file', 'blob', entry.oid], {
    encoding: null,
    maxBuffer: 64 * 1024 * 1024,
  });
  if (result.status !== 0 || !Buffer.isBuffer(result.stdout)) {
    throw new Error(`cannot read tracked blob for ${entry.path}`);
  }
  return result.stdout;
}

function untrackedNonIgnoredPaths(root) {
  const result = run('git', ['-C', root, 'ls-files', '--others', '--exclude-standard', '-z', '--']);
  if (result.status !== 0) {
    throw new Error('git ls-files failed while enumerating non-ignored working-tree paths');
  }
  return result.stdout.split('\0').filter(Boolean).sort();
}

function isWindowsAbsolute(target) {
  return /^[A-Za-z]:[\\/]/.test(target) || /^\\\\/.test(target);
}

function staysInsideRoot(root, path) {
  const rel = relative(root, path);
  return rel === '' || (!isAbsolute(rel) && rel !== '..' && !rel.startsWith(`..${sep}`));
}

function lineForOffset(text, offset) {
  let line = 1;
  for (let i = 0; i < offset; i++) {
    if (text.charCodeAt(i) === 10) line++;
  }
  return line;
}

function inspectWorktreePath(root, repositoryPath) {
  const components = repositoryPath.split('/');
  if (components.length === 0 || components.some((part) => !part || part === '.' || part === '..')) {
    return { kind: 'invalid' };
  }
  let current = root;
  for (let index = 0; index < components.length; index++) {
    current = join(current, components[index]);
    let metadata;
    try {
      metadata = lstatSync(current);
    } catch (error) {
      if (error && error.code === 'ENOENT') return { kind: 'missing' };
      throw error;
    }
    if (metadata.isSymbolicLink()) {
      let target = '';
      try { target = readlinkSync(current); } catch { /* reported without target details */ }
      return {
        kind: 'symlink',
        ancestor: index !== components.length - 1,
        absolute: isAbsolute(target) || isWindowsAbsolute(target),
      };
    }
    if (index !== components.length - 1) {
      if (!metadata.isDirectory()) return { kind: 'blocked-ancestor' };
      continue;
    }
    if (!metadata.isFile()) return { kind: metadata.isDirectory() ? 'directory' : 'special' };
  }

  const absolutePath = join(root, ...components);
  let descriptor;
  try {
    descriptor = openSync(absolutePath, constants.O_RDONLY | (constants.O_NOFOLLOW ?? 0));
    const metadata = fstatSync(descriptor);
    if (!metadata.isFile()) return { kind: 'special' };
    return {
      kind: 'regular',
      bytes: readFileSync(descriptor),
      executable: (metadata.mode & 0o111) !== 0,
    };
  } catch (error) {
    if (error && (error.code === 'ENOENT' || error.code === 'ELOOP')) {
      return { kind: error.code === 'ENOENT' ? 'missing' : 'symlink', ancestor: false, absolute: false };
    }
    throw error;
  } finally {
    if (descriptor !== undefined) closeSync(descriptor);
  }
}

function findClosingParenthesis(text, openingOffset) {
  let depth = 0;
  let quote = '';
  let escaped = false;
  for (let index = openingOffset; index < text.length; index++) {
    const character = text[index];
    if (quote) {
      if (escaped) {
        escaped = false;
      } else if (character === '\\') {
        escaped = true;
      } else if (character === quote) {
        quote = '';
      }
      continue;
    }
    if (character === '"' || character === "'" || character === '`') {
      quote = character;
    } else if (character === '(') {
      depth++;
    } else if (character === ')') {
      depth--;
      if (depth === 0) return index;
    }
  }
  return -1;
}

function staticStringValues(text) {
  const values = [];
  for (let index = 0; index < text.length; index++) {
    const quote = text[index];
    if (quote !== '"' && quote !== "'" && quote !== '`') continue;
    let raw = '';
    let closed = false;
    for (index++; index < text.length; index++) {
      const character = text[index];
      if (character === '\\' && index + 1 < text.length) {
        const escaped = text[++index];
        raw += escaped === 'n' ? '\n' : escaped === 'r' ? '\r' : escaped === 't' ? '\t' : escaped;
      } else if (character === quote) {
        closed = true;
        break;
      } else {
        raw += character;
      }
    }
    if (closed && !(quote === '`' && raw.includes('${'))) values.push(raw);
  }
  return values;
}

function isCheckoutTarget(value) {
  return /^(?:daw(?:-[A-Za-z0-9._-]+)?|frontend|backend|[A-Za-z0-9._-]*(?:checkout|worktree)[A-Za-z0-9._-]*)$/i.test(value);
}

function hasAdjacentCombination(values, predicate) {
  for (let start = 0; start < values.length; start++) {
    let combined = '';
    for (let end = start; end < Math.min(values.length, start + 4); end++) {
      combined += values[end];
      if (predicate(combined)) return true;
    }
  }
  return false;
}

function relativeCandidateIsCheckoutLocal(root, repositoryPath, candidate, rootBased = false) {
  if (!root || !repositoryPath) return false;
  const normalized = candidate.replace(/[\\/]+/g, sep);
  const base = rootBased ? root : dirname(resolve(root, repositoryPath));
  return staysInsideRoot(root, resolve(base, normalized));
}

function pathExpressionFindings(text, context = {}) {
  const findings = [];
  PATH_CALL_START_PATTERN.lastIndex = 0;
  for (const match of text.matchAll(PATH_CALL_START_PATTERN)) {
    const openingOffset = text.indexOf('(', match.index ?? 0);
    if (openingOffset < 0) continue;
    const closingOffset = findClosingParenthesis(text, openingOffset);
    if (closingOffset < 0) continue;
    const body = text.slice(openingOffset + 1, closingOffset);
    const values = staticStringValues(body);
    const components = values.flatMap((value) => value.split(/[\\/]+/).filter(Boolean));
    const hasParent = components.includes('..')
      || hasAdjacentCombination(values, (value) => value === '..');
    const hasSibling = components.some(isCheckoutTarget)
      || hasAdjacentCombination(values, isCheckoutTarget);
    const firstParent = values.findIndex((value, index) => value.split(/[\\/]+/).includes('..')
      || (index + 1 < values.length && value + values[index + 1] === '..'));
    const candidate = firstParent >= 0 ? values.slice(firstParent).join(sep) : '';
    const rootBased = /\b(?:ROOT|root|repositoryRoot|repoRoot)\b/.test(body);
    const checkoutLocal = candidate
      && relativeCandidateIsCheckoutLocal(context.root, context.path, candidate, rootBased);
    if (hasParent && hasSibling && !checkoutLocal) {
      findings.push({
        offset: match.index ?? 0,
        rule: 'implicit-sibling-path-expression',
        explanation: 'live repository content constructs an implicit sibling-checkout path',
      });
    }

    const usesHome = /\b(?:process\.env\.HOME|os\.homedir|homedir|Path\.home|expanduser)\s*(?:\(|\b)/.test(body);
    const hasSourceDirectory = components.some((component) => SOURCE_DIRECTORY_NAMES.has(component.toLowerCase()));
    const hasCheckoutComponent = components.some(isCheckoutTarget)
      || hasAdjacentCombination(values, isCheckoutTarget);
    if (usesHome && hasSourceDirectory && hasCheckoutComponent) {
      findings.push({
        offset: match.index ?? 0,
        rule: 'home-derived-checkout-path',
        explanation: 'live repository content constructs a checkout path from a user home directory',
      });
    }
  }
  return findings;
}

function scanLiveBytes(path, state, bytes, classRule, add, context = {}) {
  if (bytes.includes(0)) {
    add({ path, state, line: 0, rule: 'live-file-contains-nul', classRule,
      explanation: 'classified live content contains a NUL byte' });
    return;
  }
  let text;
  try {
    text = new TextDecoder('utf-8', { fatal: true }).decode(bytes);
  } catch {
    add({ path, state, line: 0, rule: 'live-file-invalid-utf8', classRule,
      explanation: 'classified live content is not valid UTF-8' });
    return;
  }
  const seen = new Set();
  const report = (line, rule, explanation) => {
    const key = `${rule}:${line}`;
    if (seen.has(key)) return;
    seen.add(key);
    add({ path, state, line, rule, classRule, explanation });
  };

  for (const rule of CONTENT_RULES) {
    for (const pattern of rule.patterns) {
      pattern.lastIndex = 0;
      for (const match of text.matchAll(pattern)) {
        report(lineForOffset(text, match.index ?? 0), rule.id, rule.explanation);
      }
    }
  }
  for (const pattern of [RELATIVE_CHECKOUT_PATTERN, RELATIVE_PATH_PATTERN]) {
    pattern.lastIndex = 0;
    for (const match of text.matchAll(pattern)) {
      const candidate = match.groups?.candidate;
      if (!candidate || /[\\/]+\.\.\.$/.test(candidate)) continue;
      if (relativeCandidateIsCheckoutLocal(context.root, path, candidate)) continue;
      const offset = (match.index ?? 0) + match[0].indexOf(candidate);
      report(lineForOffset(text, offset), 'implicit-sibling-checkout',
        'live repository content contains an implicit relative sibling-checkout path');
    }
  }
  SHELL_PARENT_EXPRESSION_PATTERN.lastIndex = 0;
  for (const match of text.matchAll(SHELL_PARENT_EXPRESSION_PATTERN)) {
    report(lineForOffset(text, match.index ?? 0), 'implicit-sibling-path-expression',
      'live repository content constructs an implicit sibling-checkout path');
  }
  for (const finding of pathExpressionFindings(text, { ...context, path })) {
    report(lineForOffset(text, finding.offset), finding.rule, finding.explanation);
  }
}

function packetTreeEntry(root, packetSha, packetPath) {
  const result = run('git', ['-C', root, 'ls-tree', '-z', packetSha, '--', packetPath]);
  if (result.status !== 0 || !result.stdout.endsWith('\0')) return null;
  const record = result.stdout.slice(0, -1);
  if (record.includes('\0')) return null;
  const tab = record.indexOf('\t');
  if (tab < 0 || record.slice(tab + 1) !== packetPath) return null;
  const [mode, type, oid] = record.slice(0, tab).split(' ');
  if (!mode || !type || !oid) return null;
  return { mode, type, oid, path: packetPath };
}

function validatePacketProvenance(root, trackedByPath, add, options = {}) {
  const packetSha = options.packetSha ?? EXPECTED_PACKET_SHA;
  const baselineSha = options.baselineSha ?? EXPECTED_PRODUCT_BASELINE;
  const packetPath = options.packetPath ?? PACKET_PATH;
  const commit = run('git', ['-C', root, 'cat-file', '-e', `${packetSha}^{commit}`]);
  if (commit.status !== 0) {
    add({ path: packetPath, state: 'packet', line: 0, rule: 'packet-commit-missing',
      explanation: 'the acknowledged task-packet commit is unavailable' });
    return;
  }
  const baseline = run('git', ['-C', root, 'cat-file', '-e', `${baselineSha}^{commit}`]);
  const baselineAncestor = baseline.status === 0
    ? run('git', ['-C', root, 'merge-base', '--is-ancestor', baselineSha, packetSha])
    : { status: 1 };
  if (baseline.status !== 0 || baselineAncestor.status !== 0) {
    add({ path: packetPath, state: 'packet', line: 0, rule: 'packet-baseline-mismatch',
      explanation: 'the acknowledged packet is not based on the declared product baseline' });
  } else {
    const changed = run('git', ['-C', root, 'diff', '--name-only', '-z', baselineSha, packetSha, '--']);
    const paths = changed.status === 0 ? changed.stdout.split('\0').filter(Boolean) : [];
    if (changed.status !== 0 || paths.length !== 1 || paths[0] !== packetPath) {
      add({ path: packetPath, state: 'packet', line: 0, rule: 'packet-scope-mismatch',
        explanation: 'commits between product baseline and acknowledged packet changed non-packet paths' });
    }
  }
  const ancestor = run('git', ['-C', root, 'merge-base', '--is-ancestor', packetSha, 'HEAD']);
  if (ancestor.status !== 0) {
    add({ path: packetPath, state: 'packet', line: 0, rule: 'packet-not-ancestor',
      explanation: 'the acknowledged task packet is not an ancestor of the checked worktree' });
  }

  const expected = packetTreeEntry(root, packetSha, packetPath);
  if (!expected || expected.type !== 'blob') {
    add({ path: packetPath, state: 'packet', line: 0, rule: 'packet-blob-missing',
      explanation: 'the acknowledged packet commit does not contain the expected regular packet file' });
    return;
  }
  const indexed = trackedByPath.get(packetPath);
  if (!indexed || indexed.mode !== expected.mode || indexed.oid !== expected.oid) {
    add({ path: packetPath, state: 'index', line: 0, rule: 'packet-index-drift',
      explanation: 'the index packet does not match the acknowledged packet commit' });
  }
  const worktree = inspectWorktreePath(root, packetPath);
  if (worktree.kind !== 'regular') {
    add({ path: packetPath, state: 'worktree', line: 0, rule: 'packet-worktree-invalid',
      explanation: 'the working-tree packet is missing, non-regular, or reached through a symlink' });
    return;
  }
  const expectedBytes = readTrackedBlob(root, expected);
  const expectedExecutable = expected.mode === '100755';
  if (!worktree.bytes.equals(expectedBytes) || worktree.executable !== expectedExecutable) {
    add({ path: packetPath, state: 'worktree', line: 0, rule: 'packet-worktree-drift',
      explanation: 'the working-tree packet does not match the acknowledged packet commit' });
  }
}

function scanRepository(root, options = {}) {
  const canonicalRoot = realpathSync(root);
  const tracked = trackedEntries(canonicalRoot);
  const entries = tracked.entries;
  const trackedByPath = new Map(entries.map((entry) => [entry.path, entry]));
  const violations = [];
  const classificationCounts = new Map();
  let indexLiveFiles = 0;
  let worktreeLiveFiles = 0;
  let skippedUnknownUntracked = 0;

  const add = (violation) => violations.push({ line: 0, classRule: '', ...violation });
  const countClass = (state, classification) => {
    const key = `${state}:${classification.kind}:${classification.id}`;
    classificationCounts.set(key, (classificationCounts.get(key) ?? 0) + 1);
  };

  for (const path of tracked.unmergedPaths) {
    add({ path, state: 'index', rule: 'unmerged-index-entry',
      explanation: 'tracked path has no authoritative stage-0 blob' });
  }

  for (const entry of entries) {
    const absolutePath = resolve(canonicalRoot, entry.path);
    if (!staysInsideRoot(canonicalRoot, absolutePath)) {
      add({ path: entry.path, state: 'index', rule: 'tracked-path-escapes-root',
        explanation: 'tracked path resolves outside the repository root' });
      continue;
    }
    if (entry.mode === '120000') {
      const targetBytes = readTrackedBlob(canonicalRoot, entry);
      if (targetBytes.includes(0)) {
        add({ path: entry.path, state: 'index', rule: 'tracked-symlink-target-invalid',
          explanation: 'tracked symlink target contains a NUL byte' });
      } else {
        const target = targetBytes.toString('utf8');
        const absolute = isAbsolute(target) || isWindowsAbsolute(target);
        const escaping = !absolute && !staysInsideRoot(canonicalRoot, resolve(dirname(absolutePath), target));
        add({ path: entry.path, state: 'index',
          rule: absolute ? 'tracked-absolute-symlink' : escaping ? 'tracked-escaping-symlink' : 'tracked-relative-symlink',
          explanation: absolute ? 'tracked symlink has an absolute target'
            : escaping ? 'tracked relative symlink escapes the repository root'
              : 'tracked relative symlinks are forbidden even when lexically contained' });
      }
      continue;
    }
    if (entry.mode === '160000') {
      add({ path: entry.path, state: 'index', rule: 'tracked-gitlink',
        explanation: 'tracked gitlinks are forbidden dependency escapes' });
      continue;
    }
    if (entry.mode !== '100644' && entry.mode !== '100755') {
      add({ path: entry.path, state: 'index', rule: 'tracked-mode-unsupported',
        explanation: 'tracked path has an unsupported Git mode' });
      continue;
    }
    const bytes = readTrackedBlob(canonicalRoot, entry);
    const classification = classifySurface(entry.path, entry.mode, bytes);
    countClass('index', classification);
    if (classification.kind === 'forbidden' || classification.kind === 'unknown') {
      add({ path: entry.path, state: 'index', rule: classification.id,
        classRule: classification.id, explanation: classification.provenance });
    } else if (classification.kind === 'live') {
      indexLiveFiles++;
      scanLiveBytes(entry.path, 'index', bytes, classification.id, add, { root: canonicalRoot });
    }
  }

  const untracked = untrackedNonIgnoredPaths(canonicalRoot);
  const worktreePaths = [...new Set([...entries.map((entry) => entry.path), ...untracked])].sort();
  const untrackedSet = new Set(untracked);
  for (const path of worktreePaths) {
    const representation = inspectWorktreePath(canonicalRoot, path);
    const isUntracked = untrackedSet.has(path);
    if (representation.kind === 'missing') continue;
    if (representation.kind === 'symlink') {
      const rule = representation.ancestor ? 'worktree-symlinked-ancestor'
        : representation.absolute ? 'worktree-absolute-symlink' : 'worktree-relative-symlink';
      add({ path, state: 'worktree', rule,
        explanation: representation.ancestor
          ? 'working-tree path is reached through a symlinked ancestor'
          : representation.absolute ? 'working-tree path is an absolute symlink'
            : 'working-tree path is a relative symlink' });
      continue;
    }
    if (representation.kind !== 'regular') {
      add({ path, state: 'worktree', rule: 'worktree-path-not-regular',
        explanation: 'working-tree path is not a regular file' });
      continue;
    }
    const mode = representation.executable ? '100755' : '100644';
    const classification = classifySurface(path, mode, representation.bytes, representation.executable);
    countClass('worktree', classification);
    if (classification.kind === 'unknown' && isUntracked) {
      skippedUnknownUntracked++;
      continue;
    }
    if (classification.kind === 'forbidden' || classification.kind === 'unknown') {
      add({ path, state: 'worktree', rule: classification.id,
        classRule: classification.id, explanation: classification.provenance });
    } else if (classification.kind === 'live') {
      worktreeLiveFiles++;
      scanLiveBytes(path, 'worktree', representation.bytes, classification.id, add, { root: canonicalRoot });
    }
  }

  if (options.auditExtensionRules) {
    for (const rule of LIVE_CLASS_RULES) {
      for (const extension of rule.extensions ?? []) {
        if (!entries.some((entry) => extname(entry.path).toLowerCase() === extension)) {
          add({ path: '<classification-policy>', state: 'policy', rule: 'dead-live-extension-rule',
            classRule: rule.id, explanation: 'a live extension rule has no tracked production representative' });
        }
      }
    }
  }
  if (options.validatePacket) {
    validatePacketProvenance(canonicalRoot, trackedByPath, add, options.packetOptions);
  }

  violations.sort((a, b) =>
    a.state < b.state ? -1 : a.state > b.state ? 1
      : a.path < b.path ? -1 : a.path > b.path ? 1
        : a.line - b.line || (a.rule < b.rule ? -1 : a.rule > b.rule ? 1 : 0));
  return {
    classificationCounts,
    entries: entries.length,
    indexLiveFiles,
    packetSha: options.validatePacket ? (options.packetOptions?.packetSha ?? EXPECTED_PACKET_SHA) : null,
    skippedUnknownUntracked,
    untrackedPaths: untracked.length,
    violations,
    worktreeLiveFiles,
    worktreePaths: worktreePaths.length,
  };
}

function formatViolation(violation) {
  const location = violation.line > 0 ? `${violation.path}:${violation.line}` : violation.path;
  const classification = violation.classRule ? ` [class=${violation.classRule}]` : '';
  return `${violation.state}:${location}: ${violation.rule}${classification}: ${violation.explanation}`;
}

function git(root, ...args) {
  const result = run('git', ['-C', root, ...args]);
  if (result.status !== 0) throw new Error(`git ${args[0]} failed in a self-test fixture`);
  return result;
}

function gitOutput(root, ...args) {
  return git(root, ...args).stdout.trim();
}

function makeFixture(parent, name, files = {}) {
  const root = join(parent, name);
  mkdirSync(root, { recursive: true });
  git(root, 'init', '-q');
  for (const [path, contents] of Object.entries(files)) {
    const absolutePath = join(root, path);
    mkdirSync(dirname(absolutePath), { recursive: true });
    writeFileSync(absolutePath, contents);
  }
  return root;
}

function stageAll(root) {
  git(root, 'add', '-A');
}

function assert(condition, message) {
  if (!condition) throw new Error(message);
}

function trustedSelfTestTempBase() {
  const saved = new Map();
  for (const name of ['TMPDIR', 'TMP', 'TEMP']) {
    saved.set(name, process.env[name]);
    delete process.env[name];
  }
  let canonicalTemp;
  try {
    canonicalTemp = realpathSync(tmpdir());
  } finally {
    for (const [name, value] of saved) {
      if (value === undefined) delete process.env[name];
      else process.env[name] = value;
    }
  }
  const target = lstatSync(canonicalTemp);
  assert(target.isDirectory() && !target.isSymbolicLink(), 'refusing to use an invalid OS temp root');
  const containingWorktree = run('git', ['-C', canonicalTemp, 'rev-parse', '--show-toplevel']);
  assert(containingWorktree.status !== 0, 'refusing to create self-test repositories inside a Git worktree');
  return canonicalTemp;
}

function safeRemoveSelfTestRoot(path, trustedTempBase) {
  const canonicalParent = realpathSync(dirname(path));
  assert(canonicalParent === trustedTempBase, 'refusing to remove a self-test directory outside the validated OS temp root');
  assert(basename(path).startsWith('daw-repository-integrity-'), 'refusing to remove an unexpected self-test directory');
  const target = lstatSync(path);
  assert(target.isDirectory() && !target.isSymbolicLink(), 'refusing to remove a non-directory or symlink self-test target');
  rmSync(path, { recursive: true, force: true });
}

function selfTest(repositoryRoot) {
  const tempBase = trustedSelfTestTempBase();
  const tempRoot = mkdtempSync(join(tempBase, 'daw-repository-integrity-'));
  const requiredControls = new Set([
    'clean-index-and-worktree',
    'index-bad-worktree-clean',
    'worktree-bad-index-clean',
    'permitted-safe-dirty-worktree',
    'untracked-live-worktree',
    'worktree-absolute-symlink-replacement',
    'worktree-symlinked-ancestor',
    'operational-markdown',
    'home-derived-literal',
    'home-derived-expression',
    'split-path-expression',
    'mode-100644-no-shebang-shell-extension',
    'node-modules-symlink-ignored',
    'force-added-node-modules-symlink-rejected',
    'packet-pin-clean',
    'packet-worktree-drift',
    'packet-index-drift',
    'excluded-class-provenance',
    'unknown-tracked-class-rejected',
    'generated-bytecode-rejected',
    'extension-policy-production-coverage',
    'stack-role-split-policy',
    'live-nul-rejected',
    'contained-relative-symlink-rejected',
    'escaping-relative-symlink-rejected',
    'foreign-cwd-root-isolation',
    'symlinked-shell-entrypoint-rejected',
    'git-environment-steering-rejected',
    'bash-environment-sanitized',
    'public-root-argument-rejected',
    'invalid-root-override-rejected',
    'git-worktree-temp-steering-ignored',
    'non-git-temp-steering-ignored',
    'daw-scrub-regression-control', 'ws-port-derivation-control',
    'page-identity-no-store-control', 'sidecar-listener-ownership-control',
    'ready-retirement-registration-control',
    'wrapper-helper-checker-canaries',
  ]);
  const passedControls = new Set();
  const pass = (id, label) => {
    assert(requiredControls.has(id), `self-test reported an undeclared control: ${id}`);
    assert(!passedControls.has(id), `self-test reported a duplicate control: ${id}`);
    passedControls.add(id);
    console.log(`  PASS  ${id}: ${label}`);
  };
  const slash = '/';
  const backslash = String.fromCharCode(92);
  const userPath = ['', 'Users', 'fixture-user', 'src', 'foreign-checkout'].join(slash);
  const windowsUserPath = ['C:', 'Users', 'fixture-user', 'src', 'foreign-checkout'].join(backslash);
  const nonCheckoutPath = ['', 'Users', 'fixture-user', 'samples', 'kick.wav'].join(slash);
  const absoluteLinkTarget = ['', 'Users', 'fixture-user', 'private', 'dependency'].join(slash);
  const safeHomeDependency = ['$' + 'HOME', 'src', 'juce', 'JUCE'].join(slash);
  const safeShell = `#!/bin/sh\n# sample input: ${nonCheckoutPath}\n# dependency: ${safeHomeDependency}\nexit 0\n`;
  const badShell = `#!/bin/sh\n# checkout: ${userPath}\nexit 0\n`;

  try {
    const clean = makeFixture(tempRoot, 'clean', {
      'tools/good.sh': safeShell,
    });
    stageAll(clean);
    assert(scanRepository(clean).violations.length === 0, 'clean live fixture was rejected');
    pass('clean-index-and-worktree', 'clean staged and working representations pass');

    const indexBad = makeFixture(tempRoot, 'index-bad', {
      'tools/dual-state.sh': badShell,
    });
    stageAll(indexBad);
    writeFileSync(join(indexBad, 'tools', 'dual-state.sh'), safeShell);
    const indexBadResult = scanRepository(indexBad);
    assert(indexBadResult.violations.some((violation) => violation.state === 'index'
      && violation.rule === 'user-specific-absolute-path'), 'bad index bytes were hidden by clean working-tree bytes');
    assert(!indexBadResult.violations.some((violation) => violation.state === 'worktree'),
      'clean working-tree bytes were reported as forbidden');
    pass('index-bad-worktree-clean', 'index bytes remain authoritative even when worktree is clean');

    const worktreeBad = makeFixture(tempRoot, 'worktree-bad', {
      'tools/dual-state.sh': safeShell,
    });
    stageAll(worktreeBad);
    writeFileSync(join(worktreeBad, 'tools', 'dual-state.sh'), badShell);
    const worktreeBadResult = scanRepository(worktreeBad);
    assert(worktreeBadResult.violations.some((violation) => violation.state === 'worktree'
      && violation.rule === 'user-specific-absolute-path'), 'bad working-tree bytes were hidden by clean index bytes');
    assert(!worktreeBadResult.violations.some((violation) => violation.state === 'index'),
      'clean index bytes were reported as forbidden');
    assert(!worktreeBadResult.violations.map(formatViolation).join('\n').includes(userPath),
      'dual-state diagnostic leaked the forbidden path value');
    pass('worktree-bad-index-clean', 'working-tree bytes are scanned independently from the index');

    const safeDirty = makeFixture(tempRoot, 'safe-dirty', {
      'tools/dirty.sh': safeShell,
    });
    stageAll(safeDirty);
    writeFileSync(join(safeDirty, 'tools', 'dirty.sh'), '#!/bin/sh\n# harmless permitted edit\nexit 0\n');
    assert(scanRepository(safeDirty).violations.length === 0, 'permitted dirty working-tree content was rejected');
    pass('permitted-safe-dirty-worktree', 'ordinary safe dirty changes remain supported');

    const untrackedLive = makeFixture(tempRoot, 'untracked-live', {
      'tools/good.sh': safeShell,
    });
    stageAll(untrackedLive);
    mkdirSync(join(untrackedLive, 'test'));
    writeFileSync(join(untrackedLive, 'test', 'untracked.mjs'), `export const checkout = ${JSON.stringify(userPath)};\n`);
    const untrackedLiveResult = scanRepository(untrackedLive);
    assert(untrackedLiveResult.violations.some((violation) => violation.state === 'worktree'
      && violation.path === 'test/untracked.mjs' && violation.rule === 'user-specific-absolute-path'),
    'non-ignored untracked live content bypassed the working-tree scan');
    pass('untracked-live-worktree', 'non-ignored untracked operational content is scanned');

    const absoluteLink = makeFixture(tempRoot, 'absolute-link-replacement', {
      'tools/dependency.sh': safeShell,
    });
    stageAll(absoluteLink);
    unlinkSync(join(absoluteLink, 'tools', 'dependency.sh'));
    symlinkSync(absoluteLinkTarget, join(absoluteLink, 'tools', 'dependency.sh'));
    const absoluteLinkResult = scanRepository(absoluteLink);
    assert(absoluteLinkResult.violations.some((violation) => violation.state === 'worktree'
      && violation.rule === 'worktree-absolute-symlink'), 'regular-file to absolute-symlink replacement was not rejected');
    assert(!absoluteLinkResult.violations.some((violation) => violation.state === 'index'),
      'clean regular index representation was rejected during symlink replacement control');
    assert(!absoluteLinkResult.violations.map(formatViolation).join('\n').includes(absoluteLinkTarget), 'symlink diagnostic leaked its target');
    pass('worktree-absolute-symlink-replacement', 'unstaged absolute-symlink replacement cannot hide behind a regular index blob');

    const symlinkedAncestor = makeFixture(tempRoot, 'symlinked-ancestor', {
      'tools/nested/good.sh': safeShell,
    });
    stageAll(symlinkedAncestor);
    const movedTools = join(tempRoot, 'moved-tools');
    renameSync(join(symlinkedAncestor, 'tools'), movedTools);
    symlinkSync(movedTools, join(symlinkedAncestor, 'tools'));
    const symlinkedAncestorResult = scanRepository(symlinkedAncestor);
    assert(symlinkedAncestorResult.violations.some((violation) => violation.state === 'worktree'
      && violation.path === 'tools/nested/good.sh' && violation.rule === 'worktree-symlinked-ancestor'),
    'working-tree descendant reached through a symlinked ancestor was not rejected');
    pass('worktree-symlinked-ancestor', 'every working-tree path component is checked without following symlinks');

    const operationalMarkdown = makeFixture(tempRoot, 'operational-markdown', {
      'docs/DEMO.md': `Run from ${userPath}\n`,
      'DESIGN.md': `Historical example ${userPath}\n`,
    });
    stageAll(operationalMarkdown);
    const markdownResult = scanRepository(operationalMarkdown);
    assert(markdownResult.violations.some((violation) => violation.path === 'docs/DEMO.md'
      && violation.rule === 'user-specific-absolute-path'
      && violation.classRule === 'registered-operational-markdown'),
    'direct operational Markdown was not classified and scanned');
    // This assertion used to be its own INVERSE — `!violations.some(... 'DESIGN.md')`, asserting
    // that root design prose was exempt, which was true while `root-design-prose` excluded it.
    // That rule was deleted when doc_citation's scope widened to root documents, and the control
    // was left behind asserting the superseded behaviour. It has been failing ever since, as the
    // registered `root_isolation` test, which is precisely how a red check becomes a place later
    // breakage hides: the suite was already red here, so nothing new could show up.
    //
    // Root Markdown is LIVE now, so the honest control is the opposite one: a user-specific
    // absolute path in root prose MUST be reported. Asserting the rule, not the history.
    assert(markdownResult.violations.some((violation) => violation.path === 'DESIGN.md'
      && violation.rule === 'user-specific-absolute-path'),
    'root design prose is live content now and its absolute path was NOT reported');
    pass('operational-markdown', 'registered direct docs and root prose are both live and both scanned');

    const homeLiteral = ['$' + 'HOME', 'src', 'foreign-checkout'].join(slash);
    const homeDerivedLiteral = makeFixture(tempRoot, 'home-derived-literal', {
      'tools/home.sh': `#!/bin/sh\ncheckout=${homeLiteral}\n`,
    });
    stageAll(homeDerivedLiteral);
    assert(scanRepository(homeDerivedLiteral).violations.some((violation) =>
      violation.rule === 'home-derived-checkout-path'), 'literal home-derived checkout path was not rejected');
    pass('home-derived-literal', 'shell home-derived checkout paths are rejected');

    const homeFunction = ['home', 'dir()'].join('');
    const homeExpressionText = `const checkout = path.join(${homeFunction}, 'src', 'foreign-checkout');\n`;
    const homeDerivedExpression = makeFixture(tempRoot, 'home-derived-expression', {
      'test/home.mjs': homeExpressionText,
    });
    stageAll(homeDerivedExpression);
    assert(scanRepository(homeDerivedExpression).violations.some((violation) =>
      violation.rule === 'home-derived-checkout-path'), 'split home-derived path expression was not rejected');
    pass('home-derived-expression', 'home APIs combined with source directories are rejected');

    const joinName = ['jo', 'in'].join('');
    const siblingName = ['daw', 'web'].join('-');
    const splitExpressionText = `const checkout = path.${joinName}(root, '..', '${siblingName}');\n`;
    const splitExpression = makeFixture(tempRoot, 'split-expression', {
      'test/path.mjs': splitExpressionText,
    });
    stageAll(splitExpression);
    assert(scanRepository(splitExpression).violations.some((violation) =>
      violation.rule === 'implicit-sibling-path-expression'), 'split sibling path expression was not rejected');
    pass('split-path-expression', 'static path-constructor components cannot hide sibling authority');

    const plainShell = makeFixture(tempRoot, 'plain-shell', {
      'tools/no-shebang.sh': `# checkout: ${windowsUserPath}\nexit 0\n`,
    });
    chmodSync(join(plainShell, 'tools', 'no-shebang.sh'), 0o644);
    stageAll(plainShell);
    const plainShellEntry = trackedEntries(plainShell).entries.find((entry) => entry.path === 'tools/no-shebang.sh');
    assert(plainShellEntry?.mode === '100644', 'non-executable shell control was not staged with mode 100644');
    const plainShellBytes = readTrackedBlob(plainShell, plainShellEntry);
    assert(!(plainShellBytes[0] === 0x23 && plainShellBytes[1] === 0x21), 'non-shebang shell control accidentally has a shebang');
    const plainShellResult = scanRepository(plainShell);
    assert(plainShellResult.violations.some((violation) => violation.state === 'index'
      && violation.rule === 'user-specific-absolute-path'
      && violation.classRule === 'shell-extension'), 'mode-100644 no-shebang shell did not fire the shell-extension rule');
    pass('mode-100644-no-shebang-shell-extension', 'extension rule fires independently of executable mode and shebang');

    const dependencyIgnore = makeFixture(tempRoot, 'dependency-ignore', {
      '.gitignore': readFileSync(join(repositoryRoot, '.gitignore')),
      'README.md': 'dependency ignore fixture\n',
    });
    stageAll(dependencyIgnore);
    const dependencyTarget = join(tempRoot, 'dependency-target');
    mkdirSync(dependencyTarget);
    symlinkSync(dependencyTarget, join(dependencyIgnore, 'node_modules'));
    assert(git(dependencyIgnore, 'check-ignore', '-q', 'node_modules').status === 0,
      'actual repository ignore policy did not ignore a node_modules symlink name');
    const ignoredDependencyResult = scanRepository(dependencyIgnore);
    assert(!ignoredDependencyResult.violations.some((violation) => violation.path === 'node_modules'),
      'ignored untracked node_modules symlink was included in repository authority');
    pass('node-modules-symlink-ignored', 'actual .gitignore excludes the symlink name, not only directories');
    git(dependencyIgnore, 'add', '-f', 'node_modules');
    const forcedDependencyResult = scanRepository(dependencyIgnore);
    assert(forcedDependencyResult.violations.some((violation) => violation.path === 'node_modules'
      && violation.state === 'index' && violation.rule === 'tracked-absolute-symlink'),
    'force-added absolute dependency symlink was not rejected from the index');
    pass('force-added-node-modules-symlink-rejected', 'force-add cannot bypass tracked dependency-symlink policy');

    const packetFixture = makeFixture(tempRoot, 'packet-pin', {
      'README.md': 'packet baseline\n',
    });
    git(packetFixture, 'config', 'user.name', 'Repository Integrity Self Test');
    git(packetFixture, 'config', 'user.email', 'repository-integrity@example.invalid');
    stageAll(packetFixture);
    git(packetFixture, 'commit', '-q', '-m', 'baseline');
    const fixtureBaseline = gitOutput(packetFixture, 'rev-parse', 'HEAD');
    const packetFile = join(packetFixture, ...PACKET_PATH.split('/'));
    mkdirSync(dirname(packetFile), { recursive: true });
    const pinnedPacketBytes = Buffer.from('# fixture packet\n');
    writeFileSync(packetFile, pinnedPacketBytes);
    stageAll(packetFixture);
    git(packetFixture, 'commit', '-q', '-m', 'packet');
    const fixturePacket = gitOutput(packetFixture, 'rev-parse', 'HEAD');
    const fixturePacketOptions = {
      packetSha: fixturePacket,
      baselineSha: fixtureBaseline,
      packetPath: PACKET_PATH,
    };
    const pinnedPacketResult = scanRepository(packetFixture, {
      validatePacket: true,
      packetOptions: fixturePacketOptions,
    });
    assert(pinnedPacketResult.violations.length === 0, 'clean exact packet provenance was rejected');
    pass('packet-pin-clean', 'exact packet SHA, baseline ancestry/scope, index blob, and worktree bytes agree');
    writeFileSync(packetFile, '# drifted packet\n');
    const packetWorktreeDrift = scanRepository(packetFixture, {
      validatePacket: true,
      packetOptions: fixturePacketOptions,
    });
    assert(packetWorktreeDrift.violations.some((violation) => violation.rule === 'packet-worktree-drift'),
      'dirty task-packet worktree bytes bypassed the exact packet pin');
    pass('packet-worktree-drift', 'worktree packet drift is rejected even with a pinned index');
    git(packetFixture, 'add', PACKET_PATH);
    const packetIndexDrift = scanRepository(packetFixture, {
      validatePacket: true,
      packetOptions: fixturePacketOptions,
    });
    assert(packetIndexDrift.violations.some((violation) => violation.rule === 'packet-index-drift'),
      'staged task-packet drift bypassed the exact packet pin');
    pass('packet-index-drift', 'index packet drift is rejected independently');

    const excluded = classifySurface('fixture.wav', '100644', Buffer.from('opaque'));
    assert(excluded.kind === 'excluded' && excluded.id === 'audio-fixture' && excluded.provenance,
      'excluded binary class lacks explicit provenance');
    // The fixture used to carry a root `DESIGN.md` holding an absolute path alongside the .wav,
    // asserting BOTH stayed silent. That was the `root-design-prose` exclusion, now deleted — root
    // Markdown is live and such a path is a real violation there, so keeping the file in a
    // zero-violations fixture asserted the opposite of the current rule. The .wav is the actual
    // subject here: a binary whose bytes must never be interpreted as paths or executable text.
    //
    // The path is now INSIDE the .wav bytes on purpose. Previously the fixture's audio content held
    // no path at all, so "zero violations" was satisfied whether wavs were excluded or scanned —
    // the assertion could not tell its two outcomes apart. It can now.
    //
    // Stated precisely, because the obvious mutation does NOT isolate this line: deleting the
    // audio-fixture exclusion is caught two assertions above ("excluded binary class lacks explicit
    // provenance"), which gates the same rule more strictly and fires first. So this assertion is
    // meaningful but not independently demonstrated, and saying so is better than implying a
    // control was proven when a sibling did the work.
    const excludedFixture = makeFixture(tempRoot, 'excluded-provenance', {
      'fixture.wav': Buffer.from(`opaque audio fixture ${userPath}`),
    });
    stageAll(excludedFixture);
    assert(scanRepository(excludedFixture).violations.length === 0, 'explicit excluded classes did not remain excluded');
    pass('excluded-class-provenance', 'excluded classes are named and carry reviewable provenance');

    const unknownFixture = makeFixture(tempRoot, 'unknown-class', {
      'fixture.dat': 'unknown tracked input\n',
    });
    stageAll(unknownFixture);
    assert(scanRepository(unknownFixture).violations.some((violation) =>
      violation.rule === 'unclassified-file' && violation.state === 'index'),
    'unknown tracked class was silently excluded');
    pass('unknown-tracked-class-rejected', 'tracked classes fail closed without provenance');

    const bytecodeFixture = makeFixture(tempRoot, 'bytecode-class', {
      'tools/cache.pyc': Buffer.from([0x42, 0x0d, 0x0a]),
    });
    stageAll(bytecodeFixture);
    assert(scanRepository(bytecodeFixture).violations.some((violation) =>
      violation.rule === 'generated-python-bytecode'), 'tracked generated bytecode was not rejected');
    pass('generated-bytecode-rejected', 'generated Python bytecode is a forbidden tracked class');

    const productionEntries = trackedEntries(repositoryRoot).entries;
    for (const rule of LIVE_CLASS_RULES) {
      for (const extension of rule.extensions ?? []) {
        assert(productionEntries.some((entry) => extname(entry.path).toLowerCase() === extension),
          `live extension policy is dead in production: ${rule.id} ${extension}`);
      }
    }
    pass('extension-policy-production-coverage', 'every extension-based live class has a tracked production representative');

    const stackRunRoot = join(tempRoot, 'stack-run-owned');
    const stackSidecarCwd = join(stackRunRoot, 'sidecar', 'work');
    const stackTempDir = join(stackRunRoot, 'tmp');
    mkdirSync(stackSidecarCwd, { recursive: true });
    mkdirSync(stackTempDir);
    const stackCredentialFile = join(stackRunRoot, 'explicit.env');
    writeFileSync(stackCredentialFile, 'ANTHROPIC_API_KEY=self-test-only\n');
    const stackPolicy = run('node', ['--input-type=module', '-e', `
      const { realpathSync } = await import('node:fs');
      const { isAbsolute, relative, sep } = await import('node:path');
      const { pathToFileURL } = await import('node:url');
      const stack = await import(pathToFileURL(process.env.DAW_TEST_STACK_MODULE).href);
      const runRoot = realpathSync(process.env.DAW_TEST_STACK_RUN_ROOT);
      const sidecarCwd = realpathSync(process.env.DAW_TEST_STACK_SIDECAR_CWD);
      const within = (root, candidate) => {
        const rel = relative(root, candidate);
        return rel === '' || (!isAbsolute(rel) && rel !== '..' && !rel.startsWith('..' + sep));
      };
      if (stack.STACK_LOCAL_PATHS.repositoryRoot !== process.env.DAW_TEST_REPOSITORY_ROOT
          || !within(stack.STACK_LOCAL_PATHS.repositoryRoot, stack.STACK_LOCAL_PATHS.patcherPresetDir)) {
        throw new Error('stack checkout-local exports are not rooted in the containing checkout');
      }
      const searched = stack.sidecarCredentialSearchPaths(sidecarCwd);
      if (searched.length !== 3 || !searched.every((candidate) => within(runRoot, candidate))) {
        throw new Error('sidecar credential discovery escapes the run-owned root');
      }
      const ambient = {
        ANTHROPIC_API_KEY: 'ambient-key', DAW_ENV_FILE: '/ambient.env',
        NODE_OPTIONS: 'ambient-node', DAW_PLUGIN_CACHE: 'ambient-cache',
        DAW_EVENT_LOG: 'ambient-log', TMPDIR: 'ambient-temp',
      };
      if (Object.keys(stack.explicitStackCredentials(ambient, false)).length !== 0) {
        throw new Error('credential-free mode captured ambient credentials');
      }
      const credentials = stack.explicitStackCredentials({
        ANTHROPIC_API_KEY: 'explicit-key',
        DAW_ENV_FILE: process.env.DAW_TEST_STACK_CREDENTIAL_FILE,
      }, true);
      const roles = stack.stackChildEnvironments(ambient, {
        allowCredentials: true,
        credentials,
        shm: '/self_test_shm',
        projectDir: process.env.DAW_TEST_STACK_RUN_ROOT,
        hostBin: process.env.DAW_TEST_STACK_CREDENTIAL_FILE,
        pluginCache: process.env.DAW_TEST_STACK_CREDENTIAL_FILE,
        patcherPresetDir: stack.STACK_LOCAL_PATHS.patcherPresetDir,
        tempDir: process.env.DAW_TEST_STACK_TEMP_DIR,
      });
      const hasCredential = (environment) => Object.keys(environment)
        .some((name) => name.toUpperCase() === 'ANTHROPIC_API_KEY' || name.toUpperCase() === 'DAW_ENV_FILE');
      if (!hasCredential(roles.sidecar)
          || hasCredential(roles.engine) || hasCredential(roles.page) || hasCredential(roles.cli)) {
        throw new Error('credentials were not isolated to the paid sidecar role');
      }
      for (const environment of Object.values(roles)) {
        if (Object.keys(environment).some((name) => name.toUpperCase() === 'NODE_OPTIONS')
            || environment.TMPDIR !== process.env.DAW_TEST_STACK_TEMP_DIR) {
          throw new Error('ambient steering survived role environment construction');
        }
      }
      const common = stack.stackChildEnvironment(ambient, {
        pluginCache: process.env.DAW_TEST_STACK_CREDENTIAL_FILE,
        patcherPresetDir: stack.STACK_LOCAL_PATHS.patcherPresetDir,
        tempRoot: process.env.DAW_TEST_STACK_TEMP_DIR,
      });
      if (hasCredential(common) || common.DAW_PLUGIN_CACHE !== process.env.DAW_TEST_STACK_CREDENTIAL_FILE) {
        throw new Error('common environment retained credentials or lost pinned dependency inputs');
      }
      console.log('stack-role-split-pass');
    `], {
      cwd: tempRoot,
      env: {
        ...process.env,
        NODE_OPTIONS: '', NODE_PATH: '',
        DAW_TEST_STACK_MODULE: join(repositoryRoot, 'ui-web', 'test', 'stack.mjs'),
        DAW_TEST_REPOSITORY_ROOT: repositoryRoot,
        DAW_TEST_STACK_RUN_ROOT: stackRunRoot,
        DAW_TEST_STACK_SIDECAR_CWD: stackSidecarCwd,
        DAW_TEST_STACK_TEMP_DIR: stackTempDir,
        DAW_TEST_STACK_CREDENTIAL_FILE: stackCredentialFile,
      },
    });
    assert(stackPolicy.status === 0 && stackPolicy.stdout.includes('stack-role-split-pass'),
      `final stack role-split policy failed: ${stackPolicy.stderr.trim()}`);
    pass('stack-role-split-policy', 'run-owned credential search and explicit credentials are isolated to the sidecar role');

    const nulLive = makeFixture(tempRoot, 'nul-live', {
      'build.config.json': Buffer.from([0x7b, 0x00, 0x7d, 0x0a]),
      'tools/extensionless-hook': Buffer.from([0x23, 0x21, 0x2f, 0x62, 0x69, 0x6e, 0x2f, 0x73, 0x68, 0x0a, 0x00]),
    });
    stageAll(nulLive);
    const nulResult = scanRepository(nulLive);
    assert(new Set(nulResult.violations.filter((violation) => violation.state === 'index'
      && violation.rule === 'live-file-contains-nul').map((violation) => violation.path)).size === 2,
    'extension- or shebang-classified NUL-bearing live content bypassed the index scan');
    pass('live-nul-rejected', 'NUL bytes fail in extension and extensionless-shebang live classes');

    const internalLink = makeFixture(tempRoot, 'internal-link', {
      'fixtures/data.txt': 'contained\n',
    });
    symlinkSync('fixtures/data.txt', join(internalLink, 'data-link'));
    stageAll(internalLink);
    const internalLinkResult = scanRepository(internalLink);
    assert(internalLinkResult.violations.some((violation) => violation.state === 'index'
      && violation.rule === 'tracked-relative-symlink'), 'contained relative symlink was not rejected by explicit policy');
    pass('contained-relative-symlink-rejected', 'contained relative symlinks remain outside repository authority');

    const escapingLink = makeFixture(tempRoot, 'escaping-link', {
      'README.txt': 'fixture\n',
    });
    symlinkSync('../poison', join(escapingLink, 'escape-link'));
    stageAll(escapingLink);
    const escapingLinkResult = scanRepository(escapingLink);
    assert(escapingLinkResult.violations.some((violation) => violation.state === 'index'
      && violation.rule === 'tracked-escaping-symlink'), 'escaping relative symlink was not rejected distinctly');
    pass('escaping-relative-symlink-rejected', 'relative symlink escapes are distinguished and rejected');

    const caller = join(tempRoot, 'foreign-cwd');
    const poisonedSibling = join(tempRoot, 'daw-web');
    const credentialPathSentinel = join(tempRoot, 'credentials-must-not-print.env');
    const secretValueSentinel = 'secret-value-must-not-print';
    mkdirSync(caller);
    mkdirSync(join(poisonedSibling, 'tools'), { recursive: true });
    writeFileSync(join(poisonedSibling, 'tools', 'repository_integrity_check.mjs'), 'poison must not execute\n');
    const wrapper = join(repositoryRoot, 'tools', 'repository_integrity_check.sh');
    const foreignResult = run('bash', [wrapper], {
      cwd: caller,
      env: { ...process.env, DAW_REPOSITORY_ROOT: '' },
    });
    assert(foreignResult.status === 0 || foreignResult.status === 1,
      'checker errored when invoked from a foreign cwd');
    assert(foreignResult.stderr.includes(`repository root: ${repositoryRoot} (script location)`), 'foreign-cwd run did not report the containing checkout');
    assert(/repository integrity: (?:PASS|FAIL)/.test(`${foreignResult.stdout}${foreignResult.stderr}`),
      'foreign-cwd invocation did not run the repository scan');
    assert(!foreignResult.stdout.includes('poison must not execute'), 'poisoned sibling affected checker execution');
    pass('foreign-cwd-root-isolation', 'foreign cwd and poisoned sibling cannot redirect the checker');

    const webstackText = readFileSync(join(repositoryRoot, 'tools', 'webstack.sh'), 'utf8');
    const stackText = readFileSync(join(repositoryRoot, 'ui-web', 'test', 'stack.mjs'), 'utf8');
    assert(stackText.includes("name.toUpperCase().startsWith('DAW_')"), 'DAW scrub regression control missing');
    pass('daw-scrub-regression-control', 'mixed-case arbitrary DAW variables are removed before explicit pinning');
    assert(webstackText.includes('WS_STATE" = "$((PORT + 1))') && webstackText.includes('WS_CMD" = "$((PORT + 2))'), 'WS derivation control missing');
    pass('ws-port-derivation-control', 'websocket ports are constrained to PORT plus one and two');
    assert(webstackText.includes("test/serve.mjs") && webstackText.includes('no-store'), 'page identity/no-store control missing');
    pass('page-identity-no-store-control', 'page reuse requires checkout server identity and no-store semantics');
    assert(webstackText.includes('wait_for_sidecar_listener') && webstackText.includes('sidecar_listener_pids'), 'listener ownership control missing');
    pass('sidecar-listener-ownership-control', 'each sidecar listener is polled and matched to its PID');
    assert(readFileSync(join(repositoryRoot, 'tools', 'verify.sh'), 'utf8').includes('--self-test-ready-retirement'), 'retirement registration missing');
    pass('ready-retirement-registration-control', 'verify registers the READY-retirement self-test');

    mkdirSync(join(poisonedSibling, 'lib'), { recursive: true });
    writeFileSync(join(poisonedSibling, 'lib', 'repository_root.sh'), 'printf "poison helper executed\\n"\n');
    const symlinkedWrapper = join(poisonedSibling, 'integrity-entrypoint.sh');
    symlinkSync(wrapper, symlinkedWrapper);
    const symlinkedWrapperResult = run('bash', [symlinkedWrapper], {
      cwd: caller,
      env: { ...process.env, DAW_REPOSITORY_ROOT: '' },
    });
    assert(symlinkedWrapperResult.status !== 0, 'symlinked shell entrypoint was accepted');
    assert(!`${symlinkedWrapperResult.stdout}${symlinkedWrapperResult.stderr}`.includes('poison helper executed'), 'symlinked entrypoint sourced an adjacent poison helper');
    pass('symlinked-shell-entrypoint-rejected', 'shell entrypoint rejects links before helper discovery');

    const canaryRoot = join(poisonedSibling, 'wrapper-canaries');
    const canaryTools = join(canaryRoot, 'tools');
    mkdirSync(canaryTools, { recursive: true });
    writeFileSync(join(canaryTools, 'repository_integrity_check.sh'), readFileSync(wrapper));
    writeFileSync(join(canaryTools, 'repository_integrity_check.mjs'), readFileSync(join(repositoryRoot, 'tools', 'repository_integrity_check.mjs')));
    symlinkSync(join(poisonedSibling, 'lib'), join(canaryTools, 'lib'));
    const helperCanary = run('bash', [join(canaryTools, 'repository_integrity_check.sh')], { cwd: caller, env: { ...process.env, DAW_REPOSITORY_ROOT: '' } });
    assert(helperCanary.status !== 0 && !`${helperCanary.stdout}${helperCanary.stderr}`.includes('poison helper executed'), 'symlinked helper directory was accepted');
    const checkerCanaryRoot = join(poisonedSibling, 'checker-canary');
    const checkerTools = join(checkerCanaryRoot, 'tools');
    mkdirSync(join(checkerTools, 'lib'), { recursive: true });
    writeFileSync(join(checkerTools, 'repository_integrity_check.sh'), readFileSync(wrapper));
    writeFileSync(join(checkerTools, 'lib', 'repository_root.sh'), readFileSync(join(repositoryRoot, 'tools', 'lib', 'repository_root.sh')));
    symlinkSync(join(poisonedSibling, 'tools', 'repository_integrity_check.mjs'), join(checkerTools, 'repository_integrity_check.mjs'));
    const checkerCanary = run('bash', [join(checkerTools, 'repository_integrity_check.sh')], { cwd: caller, env: { ...process.env, DAW_REPOSITORY_ROOT: '' } });
    assert(checkerCanary.status !== 0, 'symlinked checker was accepted');
    pass('wrapper-helper-checker-canaries', 'wrapper refuses symlinked helper directories and checker files');

    const gitSteeringResult = run('bash', [wrapper], {
      cwd: caller,
      env: {
        ...process.env,
        DAW_REPOSITORY_ROOT: '',
        GIT_DIR: join(clean, '.git'),
        GIT_INDEX_FILE: join(clean, '.git', 'index'),
        GIT_WORK_TREE: clean,
      },
    });
    assert(gitSteeringResult.status === 0 || gitSteeringResult.status === 1,
      'Git steering environment bypassed or broke the checker');
    assert(gitSteeringResult.stderr.includes(`repository root: ${repositoryRoot} (script location)`), 'Git steering changed the reported repository root');
    assert(/repository integrity: (?:PASS|FAIL)/.test(`${gitSteeringResult.stdout}${gitSteeringResult.stderr}`),
      'Git steering prevented the repository scan');
    pass('git-environment-steering-rejected', 'inherited GIT_DIR, index, and worktree steering are stripped');

    const bashEnvironmentCanary = join(tempRoot, 'bash-environment-executed');
    const bashEnvironmentPoison = join(poisonedSibling, 'bash-environment-poison.sh');
    writeFileSync(bashEnvironmentPoison, [
      'case "${0##*/}" in',
      '  repository_integrity_check.sh) : > "$DAW_BASH_ENV_CANARY"; exit 0 ;;',
      'esac',
      '',
    ].join('\n'));
    const cleanShellEnvironment = run('cmake', [
      '-E', 'env', '--unset=BASH_ENV', '--unset=ENV', 'bash', wrapper,
    ], {
      cwd: caller,
      env: {
        ...process.env,
        BASH_ENV: bashEnvironmentPoison,
        DAW_BASH_ENV_CANARY: bashEnvironmentCanary,
        DAW_REPOSITORY_ROOT: '',
      },
    });
    let bashEnvironmentRan = true;
    try { lstatSync(bashEnvironmentCanary); } catch { bashEnvironmentRan = false; }
    assert((cleanShellEnvironment.status === 0 || cleanShellEnvironment.status === 1)
      && /repository integrity: (?:PASS|FAIL)/.test(`${cleanShellEnvironment.stdout}${cleanShellEnvironment.stderr}`),
    'sanitized CTest-style shell launch did not run the real integrity guard');
    assert(!bashEnvironmentRan, 'BASH_ENV executed before the CTest-style integrity guard launch');
    pass('bash-environment-sanitized', 'CTest-style launch strips BASH_ENV before shell startup');

    const redirectedArgument = run('bash', [wrapper, '--root', clean], {
      cwd: caller,
      env: { ...process.env, DAW_REPOSITORY_ROOT: '' },
    });
    assert(redirectedArgument.status !== 0, 'a public root argument redirected the checker');
    pass('public-root-argument-rejected', 'checker accepts no root-redirection argument');

    const canary = join(tempRoot, 'invalid-override-child-ran');
    const helper = join(repositoryRoot, 'tools', 'lib', 'repository_root.sh');
    const invalidOverride = run('bash', ['-c', [
      'set -euo pipefail',
      '. "$DAW_TEST_ROOT_HELPER"',
      'ROOT="$(daw_repository_root)"',
      ': > "$DAW_TEST_CANARY"',
    ].join('\n')], {
      cwd: caller,
      env: {
        ...process.env,
        ANTHROPIC_API_KEY: secretValueSentinel,
        DAW_ENV_FILE: credentialPathSentinel,
        DAW_REPOSITORY_ROOT: poisonedSibling,
        DAW_TEST_CANARY: canary,
        DAW_TEST_ROOT_HELPER: helper,
      },
    });
    assert(invalidOverride.status !== 0, 'invalid explicit root override was accepted');
    let canaryExists = true;
    try { lstatSync(canary); } catch { canaryExists = false; }
    assert(!canaryExists, 'child command ran after an invalid explicit root override');
    const invalidOverrideOutput = `${invalidOverride.stdout}${invalidOverride.stderr}`;
    assert(!invalidOverrideOutput.includes(poisonedSibling), 'invalid-override diagnostic leaked the rejected path');
    assert(!invalidOverrideOutput.includes(credentialPathSentinel), 'invalid-override diagnostic leaked a credential path');
    assert(!invalidOverrideOutput.includes(secretValueSentinel), 'invalid-override diagnostic leaked a secret value');
    pass('invalid-root-override-rejected', 'DAW_REPOSITORY_ROOT only confirms the containing checkout and leaks no value');

    const poisonedGitTemp = makeFixture(tempRoot, 'poisoned-git-tmp', {
      'README.txt': 'self-test temp poison fixture\n',
    });
    stageAll(poisonedGitTemp);
    const poisonedPlainTemp = join(tempRoot, 'poisoned-plain-tmp');
    mkdirSync(poisonedPlainTemp);
    const tempPrefix = 'daw-repository-integrity-';
    const assertTempSteeringIgnored = (poisonedRoot, label) => {
      const before = readdirSync(poisonedRoot).filter((name) => name.startsWith(tempPrefix));
      const saved = new Map();
      for (const name of ['TMPDIR', 'TMP', 'TEMP']) {
        saved.set(name, process.env[name]);
        process.env[name] = poisonedRoot;
      }
      let selected;
      try {
        selected = trustedSelfTestTempBase();
      } finally {
        for (const [name, value] of saved) {
          if (value === undefined) delete process.env[name];
          else process.env[name] = value;
        }
      }
      const after = readdirSync(poisonedRoot).filter((name) => name.startsWith(tempPrefix));
      assert(selected === tempBase, `${label} temp steering changed the trusted OS temp root`);
      assert(JSON.stringify(after) === JSON.stringify(before), `${label} temp steering target was mutated`);
    };
    assertTempSteeringIgnored(poisonedGitTemp, 'Git-worktree');
    pass('git-worktree-temp-steering-ignored', 'Git-worktree temp steering cannot select or mutate the fixture base');
    assertTempSteeringIgnored(poisonedPlainTemp, 'non-Git');
    pass('non-git-temp-steering-ignored', 'non-Git temp steering cannot select or mutate the fixture base');

    const missingControls = [...requiredControls].filter((id) => !passedControls.has(id));
    assert(missingControls.length === 0, `self-test coverage missing: ${missingControls.join(', ')}`);
    console.log(`repository integrity self-test: PASS (${passedControls.size} named controls)`);
  } finally {
    safeRemoveSelfTestRoot(tempRoot, tempBase);
  }
}

function parseArguments(argv) {
  let runSelfTest = false;
  for (let i = 0; i < argv.length; i++) {
    if (argv[i] === '--self-test') {
      runSelfTest = true;
    } else {
      throw new Error(`unknown argument: ${argv[i]}`);
    }
  }
  return { runSelfTest };
}

try {
  const { runSelfTest } = parseArguments(process.argv.slice(2));
  if (runSelfTest) {
    selfTest(MODULE_REPOSITORY_ROOT);
  } else {
    const result = scanRepository(MODULE_REPOSITORY_ROOT, {
      auditExtensionRules: true,
      validatePacket: true,
    });
    if (result.violations.length > 0) {
      for (const violation of result.violations) console.error(formatViolation(violation));
      console.error(`repository integrity: FAIL (${result.violations.length} violation(s))`);
      process.exitCode = 1;
    } else {
      console.log(`repository integrity: PASS (${result.entries} tracked paths, ${result.worktreePaths} working-tree paths, ${result.indexLiveFiles} index live files, ${result.worktreeLiveFiles} worktree live files, ${result.untrackedPaths} non-ignored untracked paths; packet ${result.packetSha}; product baseline ${EXPECTED_PRODUCT_BASELINE})`);
    }
  }
} catch (error) {
  console.error(`repository integrity: ERROR: ${error instanceof Error ? error.message : String(error)}`);
  process.exitCode = 2;
}
