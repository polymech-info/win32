import { createAssert, finish, requireExe, run } from './commons.mjs';

const checks = requireExe(createAssert());
const { assert } = checks;

const ok = run(['path', 'sanitize', 'a//b']);
assert(ok.status === 0 && ok.json?.value === 'a/b', `sanitize (${ok.stdout})`);

const traversal = run(['path', 'sanitize', '../etc']);
assert(traversal.status === 1 && traversal.json?.ok === false && traversal.json?.code === 'EFORBIDDEN', 'traversal');

const absolute = run(['path', 'sanitize', '--hex', Buffer.from('/etc/passwd', 'utf8').toString('hex')]);
assert(absolute.status === 0 && absolute.json?.value === 'etc/passwd', `leading slash stripped (${absolute.stdout})`);

const drive = run(['path', 'sanitize', 'C:/secret']);
assert(drive.status === 0 && drive.json?.value === 'C/secret', `colon stripped (${drive.stdout})`);

const encoded = run(['path', 'sanitize', '%2e%2e/%2e%2e/secret']);
assert(encoded.status === 1 && encoded.json?.code === 'EFORBIDDEN', 'encoded traversal');

const nested = run(['path', 'sanitize', '%252e%252e/secret']);
assert(nested.status === 1 && nested.json?.code === 'EFORBIDDEN', 'nested encoding');

const percent = run(['path', 'sanitize', '100%25done']);
assert(percent.status === 1 && percent.json?.code === 'EFORBIDDEN', `double percent (${percent.stdout})`);

const disguised = run(['path', 'sanitize', 'safe/.?.']);
assert(disguised.status === 0 && disguised.json?.value === 'safe/.?.', `dot-question stays (${disguised.stdout})`);

const name = run(['path', 'filename', 'hello?.txt']);
assert(name.status === 0 && name.json?.value === 'hello.txt', `filename (${name.stdout})`);

const reserved = run(['path', 'filename', 'con.txt']);
assert(reserved.status === 0 && reserved.json?.value === '', 'reserved device name');

const conSpace = run(['path', 'filename', 'CON ']);
assert(conSpace.status === 0 && conSpace.json?.value === 'CON', `reserved checked before trail strip (${conSpace.stdout})`);

const slashRepl = run(['path', 'filename', 'hello?.txt', '--replacement', '/']);
assert(slashRepl.status === 0 && slashRepl.json?.value === 'hello/.txt', `replacement is literal (${slashRepl.stdout})`);

const longName = 'a'.repeat(256);
const cut = run(['path', 'filename', longName]);
assert(cut.status === 0 && cut.json?.value === 'a'.repeat(255), `byte truncate (${cut.stdout})`);

const safe = run(['path', 'safe', 'hello.txt']);
assert(safe.status === 0 && safe.json?.safe === true, 'safe name');

const unsafe = run(['path', 'safe', '..']);
assert(unsafe.status === 0 && unsafe.json?.safe === false, 'dotdot is unsafe');

const fileDot = run(['path', 'safe', 'file.']);
assert(fileDot.status === 0 && fileDot.json?.safe === true, 'existing trailing dot');

const cyr = run(['path', 'safe', '--hex', Buffer.from('А', 'utf8').toString('hex')]);
assert(cyr.status === 0 && cyr.json?.safe === false, `cyrillic byte hits the control regex (${cyr.stdout})`);

const comSup = run(['path', 'safe', '--hex', Buffer.from('COM\u00b9', 'utf8').toString('hex')]);
assert(comSup.status === 0 && comSup.json?.safe === true, `superscript is not a device digit (${comSup.stdout})`);

const norm = run(['path', 'normalize', 'C:\\foo\\..\\bar', '--force']);
assert(norm.status === 0 && String(norm.json?.value).replaceAll('\\', '/') === 'C:/bar', `normalize (${norm.stdout})`);

const unc = run(['path', 'normalize', '\\\\server\\share\\foo\\..\\bar', '--force']);
assert(unc.status === 0 && String(unc.json?.value).replaceAll('\\', '/') === '/server/share/bar', `unc (${unc.stdout})`);

const url = run(['path', 'normalize', 'https://example.com/a']);
assert(url.status === 0 && url.json?.value === 'https://example.com/a', 'url stays');

const writeOk = run(['path', 'write', 'safe/file.txt']);
assert(writeOk.status === 0 && writeOk.json?.value === 'safe/file.txt', 'write keeps path');

const writeDot = run(['path', 'write', 'safe/.?.']);
assert(writeDot.status === 0 && writeDot.json?.value === 'safe/..', `write strips ? and keeps the rest (${writeDot.stdout})`);

const writeSpace = run(['path', 'write', 'safe/.. /outside.txt']);
assert(writeSpace.status === 0 && writeSpace.json?.value === 'safe/../outside.txt', `write trims then keeps .. (${writeSpace.stdout})`);

const writeDrop = run(['path', 'write', 'safe/???/file.txt']);
assert(writeDrop.status === 0 && writeDrop.json?.value === 'safe/file.txt', `write drops an empty segment (${writeDrop.stdout})`);

const writeNul = run(['path', 'write', 'NUL']);
assert(writeNul.status === 0 && writeNul.json?.value === 'NUL', `write keeps NUL (${writeNul.stdout})`);

const selection = run(['path', 'selection', 'C:\\foo\\bar']);
assert(selection.status === 0 && selection.json?.accepted === true, `selection (${selection.stdout})`);

const selectionDot = run(['path', 'selection', 'C:/foo/../bar']);
assert(selectionDot.status === 1 && selectionDot.json?.code === 'EFORBIDDEN', 'selection rejects dotdot');

finish(checks);
