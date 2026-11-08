import { createAssert, finish, requireExe, run } from './commons.mjs';

const checks = requireExe(createAssert());
const { assert } = checks;

const frame = run(['ipc', 'roundtrip', '--id', 'a', '--type', 'ping', '--payload', '{"n":1}']);
assert(
  frame.status === 0 && frame.json?.id === 'a' && frame.json?.type === 'ping'
    && frame.json?.payload === '{"n":1}' && frame.json?.bytes > 4,
  `roundtrip (${frame.stdout})`,
);

const usage = run(['ipc', 'roundtrip', '--id', 'a']);
assert(usage.status === 2, `missing payload exits 2 (got ${usage.status})`);

finish(checks);
