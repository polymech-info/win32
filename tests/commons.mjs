import { spawnSync } from 'node:child_process';
import { existsSync } from 'node:fs';
import { dirname, join, resolve } from 'node:path';
import { fileURLToPath } from 'node:url';

export const PKG = resolve(dirname(fileURLToPath(import.meta.url)), '..');

export function exePath() {
  const fromEnv = process.env.PM_COMMONS_CLI;
  if (fromEnv && existsSync(fromEnv)) return resolve(fromEnv);
  const name = process.platform === 'win32' ? 'pm-commons.exe' : 'pm-commons';
  for (const dir of ['dev', 'release']) {
    const candidate = join(PKG, 'build', dir, name);
    if (existsSync(candidate)) return candidate;
  }
  return join(PKG, 'build', 'dev', name);
}

export function createAssert() {
  let passed = 0;
  let failed = 0;
  function assert(condition, label) {
    if (condition) {
      console.log(`  ok  ${label}`);
      passed++;
    } else {
      console.error(`  FAIL  ${label}`);
      failed++;
    }
  }
  return {
    assert,
    get passed() { return passed; },
    get failed() { return failed; },
  };
}

export function run(args, { env, cwd } = {}) {
  const exe = exePath();
  const r = spawnSync(exe, args, {
    cwd: cwd || PKG,
    encoding: 'utf8',
    timeout: 20_000,
    env: { ...process.env, ...env },
  });
  const stdout = String(r.stdout || '').trim();
  let json = null;
  if (stdout) {
    try { json = JSON.parse(stdout); } catch { json = null; }
  }
  return {
    status: r.status ?? -1,
    stdout,
    stderr: String(r.stderr || ''),
    json,
    spawnError: r.error ? String(r.error.message || r.error) : '',
  };
}

export function requireExe(checks) {
  const exe = exePath();
  if (!existsSync(exe)) {
    console.error(`pm-commons not built (${exe}). From packages/commons.cpp run: npm run build:cpp`);
    process.exit(1);
  }
  console.log(`pm-commons  ${exe}`);
  return checks;
}

export function finish(checks) {
  console.log(`\n${checks.passed} passed, ${checks.failed} failed`);
  process.exit(checks.failed ? 1 : 0);
}
