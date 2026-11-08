import { mkdirSync, writeFileSync } from 'node:fs';
import { join } from 'node:path';
import { PKG, createAssert, finish, requireExe, run } from './commons.mjs';

const checks = requireExe(createAssert());
const { assert } = checks;

const tree = join(PKG, 'fixtures', 'tree');
mkdirSync(join(tree, '.git'), { recursive: true });
writeFileSync(join(tree, '.gitignore'), '*.log\n!keep.log\n');
writeFileSync(join(tree, 'a.log'), 'x');
writeFileSync(join(tree, 'keep.log'), 'x');
writeFileSync(join(tree, 'note.txt'), 'note');
writeFileSync(join(PKG, 'fixtures', 'sample.png'), Buffer.from([0x89, 0x50, 0x4e, 0x47, 0x0d, 0x0a, 0x1a, 0x0a]));

const hit = run(['fs', 'match', '--pattern', '*.log', '--rel', 'dir/a.log']);
assert(hit.status === 0 && hit.json?.matched === true && hit.json?.negative === false, `match log (${hit.stdout})`);

const folded = run(['fs', 'match', '--fold', '--pattern', 'Foo.txt', '--rel', 'foo.txt']);
assert(folded.status === 0 && folded.json?.matched === true, `fold (${folded.stdout})`);

const exact = run(['fs', 'match', '--pattern', 'Foo.txt', '--rel', 'foo.txt']);
assert(exact.status === 0 && exact.json?.matched === false, 'case sensitive by default');

const ignored = run(['fs', 'ignore', 'fixtures/tree/a.log']);
assert(ignored.status === 0 && ignored.json?.ignored === true, `ignored log (${ignored.stdout})`);

const kept = run(['fs', 'ignore', 'fixtures/tree/keep.log']);
assert(kept.status === 0 && kept.json?.ignored === false, `negated keep.log (${kept.stdout})`);

const note = run(['fs', 'ignore', 'fixtures/tree/note.txt']);
assert(note.status === 0 && note.json?.ignored === false, `note not ignored (${note.stdout})`);

const png = run(['fs', 'probe', 'fixtures/sample.png']);
assert(png.status === 0 && png.json?.kind === 'image' && png.json?.extension === '.png', `png (${png.stdout})`);

const missing = run(['fs', 'probe', 'fixtures/missing.bin']);
assert(missing.status === 1 && missing.json?.code === 'ENOENT', 'missing probe');

finish(checks);
