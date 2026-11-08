import { createAssert, finish, requireExe, run } from './commons.mjs';

const checks = requireExe(createAssert());
const { assert } = checks;

const found = run(['which', 'hello', '--path', 'fixtures/bin', '--ext', '.cmd', '--file']);
const path = String(found.json?.path || '').replaceAll('\\', '/');
assert(found.status === 0 && path.endsWith('fixtures/bin/hello.cmd') && found.json?.kind === 'cmd', `which (${found.stdout})`);

const missing = run(['which', 'no-such-pm-commons-tool', '--path', 'fixtures/bin', '--file']);
assert(missing.status === 1 && missing.json?.code === 'ENOENT', 'missing tool');

const empty = run(['library', 'probe', '']);
assert(empty.status === 1 && empty.json?.code === 'EINVAL', `empty library (${empty.stdout} ${empty.stderr})`);

const absent = run(['library', 'probe', 'pm-commons-no-such-lib']);
assert(absent.status === 0 && absent.json?.found === false, 'missing library');

if (process.platform === 'win32') {
  const kernel = run(['library', 'probe', 'kernel32.dll']);
  assert(kernel.status === 0 && kernel.json?.found === true, `kernel32 (${kernel.stdout})`);
}

finish(checks);
