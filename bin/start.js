import { createStore } from '../src/store.js';
import { createServer } from '../src/server.js';

/** PORT 환경 변수를 해석한다. 비어 있으면 6380, 0~65535 정수가 아니면 null. */
function resolvePort(raw) {
  if (raw === undefined || raw === '') return 6380;
  const n = Number(raw);
  return Number.isInteger(n) && n >= 0 && n <= 65535 ? n : null;
}

const port = resolvePort(process.env.PORT);
if (port === null) {
  console.error(`mini-redis 시작 실패: 잘못된 PORT ${process.env.PORT}`);
  process.exit(1);
}
const server = createServer({ store: createStore() });

server.on('error', (err) => {
  console.error(`mini-redis 시작 실패: ${err.code ?? err.message}`);
  process.exit(1);
});
server.listen(port, () => {
  console.log(`mini-redis listening on http://localhost:${server.address().port}`);
});
