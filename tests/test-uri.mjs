import { createAssert, finish, requireExe, run } from './commons.mjs';

const checks = requireExe(createAssert());
const { assert } = checks;

const http = run(['uri', 'parse', 'https://example.com/a%20b?x=1#f']);
assert(
  http.status === 0 && http.json?.scheme === 'https' && http.json?.host === 'example.com'
    && http.json?.path === '/a%20b' && http.json?.query === 'x=1' && http.json?.fragment === 'f',
  `parse (${http.stdout})`,
);

const file = run(['uri', 'parse', 'file://C:/dir/a']);
assert(file.status === 0 && file.json?.scheme === 'file' && String(file.json?.path).replaceAll('\\', '/') === 'C:/dir/a', `file (${file.stdout})`);

const user = run(['uri', 'parse', '--reject-userinfo', 'https://user:pass@example.com/a']);
assert(user.status === 1 && user.json?.code === 'EINVAL', 'reject userinfo');

const spaced = run(['uri', 'parse', '--lenient', 'https://example.com/a b']);
assert(spaced.status === 0 && spaced.json?.path === '/a%20b', `lenient (${spaced.stdout})`);

const decoded = run(['uri', 'decode', 'a%20b']);
assert(decoded.status === 0 && decoded.json?.value === 'a b', 'percent decode');

const loop = run(['uri', 'private', 'http://127.0.0.1/x']);
assert(loop.status === 0 && loop.json?.private === true && loop.json?.reason === 'loopback address', `loopback (${loop.stdout})`);

const pub = run(['uri', 'private', 'https://example.com/a']);
assert(pub.status === 0 && pub.json?.private === false, `public host (${pub.stdout})`);

const v6 = run(['uri', 'private', 'http://[::1]/']);
assert(v6.status === 0 && v6.json?.private === true, `ipv6 loopback (${v6.stdout})`);

finish(checks);
