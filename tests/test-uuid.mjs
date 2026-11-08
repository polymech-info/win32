import { createAssert, finish, requireExe, run } from './commons.mjs';

const checks = requireExe(createAssert());
const { assert } = checks;

const seq = run(['uuid', 'next', '--seed', '5', '--count', '2']);
assert(seq.status === 0 && seq.json?.values?.[0] === 5 && seq.json?.values?.[1] === 6, `sequential (${seq.stdout})`);

const hex = run(['uuid', 'parse', '0x10']);
assert(hex.status === 0 && hex.json?.value === 16, `hex (${hex.stdout})`);

const standard = run(['uuid', 'parse', '9b1deb4d-3b7d-4bad-9bdd-2b0d7b3dcb6d']);
assert(
  standard.status === 0 && standard.json?.high === 0x9bdd2b0d && standard.json?.low === 0x7b3dcb6d,
  `standard uuid (${standard.stdout})`,
);

const formatted = run(['uuid', 'format', '--high', '1', '--low', '2']);
assert(
  formatted.status === 0 && formatted.json?.text === '00000000-0000-0000-00000001-00000002',
  `format (${formatted.stdout})`,
);

finish(checks);
