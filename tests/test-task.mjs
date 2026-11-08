import { createAssert, finish, requireExe, run } from './commons.mjs';

const checks = requireExe(createAssert());
const { assert } = checks;

const id = run(['task', 'id', 'ipc_prune']);
assert(id.status === 0 && id.json?.id === 5 && id.json?.name === 'ipc_prune' && id.json?.offload === true, `id (${id.stdout})`);

const unknown = run(['task', 'id', 'not_a_task']);
assert(unknown.status === 1 && unknown.json?.code === 'EINVAL', 'unknown task');

const catalog = run(['task', 'catalog']);
const names = (catalog.json?.tasks || []).map((row) => row.id);
assert(catalog.status === 0 && names.includes('startup_manifests') && names.includes('settings_prewarm'), `catalog (${catalog.stdout})`);

const dir = 'fixtures/tmp/claim';
const wrote = run(['claim', 'write', '--dir', dir, '--pid', '9', '--kind', 'ui', '--priority', '100', '--now', '1000']);
assert(wrote.status === 0 && wrote.json?.kind === 'ui' && wrote.json?.pid === 9, `write (${wrote.stdout})`);

const read = run(['claim', 'read', '--dir', dir]);
assert(read.status === 0 && read.json?.priority === 100 && read.json?.updatedAt === 1000, `read (${read.stdout})`);

const higher = run(['claim', 'higher', '--dir', dir, '--self-pid', '1', '--self-priority', '10', '--now', '1500', '--alive']);
assert(higher.status === 0 && higher.json?.higher === true, `higher (${higher.stdout})`);

const stale = run(['claim', 'higher', '--dir', dir, '--self-pid', '1', '--self-priority', '10', '--now', '10000', '--alive']);
assert(stale.status === 0 && stale.json?.higher === false, `stale (${stale.stdout})`);

const self = run(['claim', 'higher', '--dir', dir, '--self-pid', '9', '--self-priority', '10', '--now', '1500', '--alive']);
assert(self.status === 0 && self.json?.higher === false, 'own claim is not foreign');

finish(checks);
