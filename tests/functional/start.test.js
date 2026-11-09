import { spawn } from 'node:child_process';
import { afterEach, expect, test } from 'vitest';

let child;
afterEach(async () => {
  if (child && child.exitCode === null && child.signalCode === null) {
    const exited = new Promise((resolve) => child.once('exit', resolve));
    child.kill();
    await exited;
  }
});

test('npm start 진입점이 서버를 띄운다', async () => {
  child = spawn('node', ['bin/start.js'], { env: { ...process.env, PORT: '0' } });
  let err = '';
  child.stderr.on('data', (d) => { err += d; });
  const port = await new Promise((resolve, reject) => {
    let out = '';
    child.stdout.on('data', (d) => {
      out += d;
      const m = out.match(/listening on http:\/\/localhost:(\d+)/);
      if (m) resolve(Number(m[1]));
    });
    child.on('exit', (code) => reject(new Error(`조기 종료 code=${code} stderr=${err}`)));
  });
  const base = `http://127.0.0.1:${port}/keys/a`;
  const put = await fetch(base, {
    method: 'PUT',
    headers: { 'content-type': 'application/json' },
    body: JSON.stringify({ value: 'v' }),
  });
  expect(put.ok).toBe(true);
  expect((await (await fetch(base)).json()).value).toBe('v');
});

test.each(['abc', '70000'])('잘못된 PORT=%s이면 한 줄 오류와 종료 코드 1', async (bad) => {
  child = spawn('node', ['bin/start.js'], { env: { ...process.env, PORT: bad } });
  let err = '';
  child.stderr.on('data', (d) => { err += d; });
  const code = await new Promise((resolve) => child.on('exit', resolve));
  expect(code).toBe(1);
  expect(err.trim()).toBe(`mini-redis 시작 실패: 잘못된 PORT ${bad}`);
});
