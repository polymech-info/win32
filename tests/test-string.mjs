import { createAssert, finish, requireExe, run } from './commons.mjs';

const checks = requireExe(createAssert());
const { assert } = checks;

const usage = run(['string', 'lower']);
assert(usage.status === 2, `missing text exits 2 (got ${usage.status})`);

const lower = run(['string', 'lower', 'AbC']);
assert(lower.status === 0 && lower.json?.value === 'abc', 'lower');

const trim = run(['string', 'trim', '  hi \n']);
assert(trim.status === 0 && trim.json?.value === 'hi', 'trim');

const leaf = run(['string', 'leaf', 'https://host/a/file.png?x=1']);
assert(leaf.status === 0 && leaf.json?.value === 'file.png', 'leaf');

const ext = run(['string', 'ext', '.gitignore']);
assert(ext.status === 0 && ext.json?.value === '', 'dotfile has no extension');

const png = run(['string', 'ext', 'C:/docs/Photo.PNG']);
assert(png.status === 0 && png.json?.value === '.png', 'ext lower');

const match = run(['string', 'match', '--hay', 'list cameras', '--query', 'camera']);
assert(match.status === 0 && match.json?.matched === true, `match camera (${match.stdout})`);

const subst = run(['string', 'subst', 'hello ${NAME}', '--set', 'NAME=anne']);
assert(subst.status === 0 && subst.json?.value === 'hello anne', `subst (${subst.stdout})`);

const kept = run(['string', 'subst', 'hello ${MISSING}']);
assert(kept.status === 0 && kept.json?.value === 'hello ${MISSING}', 'unresolved token stays');

const args = run([
  'string', 'args',
  '--arg', 'tool',
  '--arg', '${CURRENT_SELECTION}',
  '--select', 'C:\\foo\\bar',
]);
const got = (args.json?.args || []).map((item) => String(item).replaceAll('\\', '/'));
assert(args.status === 0 && got[0] === 'tool' && got[1] === 'C:/foo/bar', `args (${args.stdout})`);

finish(checks);
