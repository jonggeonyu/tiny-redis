/**
 * 데모 원본 저장소. 느린 DB를 흉내 내며 호출 횟수를 센다.
 * @module fake-db
 */

/**
 * 가짜 DB를 만든다. 조회와 수정은 delayMs만큼 늦게 끝난다.
 * @param {object} options
 * @param {Array<{id: (string|number)}>} options.users 처음 들어 있는 사용자 목록
 * @param {number} [options.delayMs=100] 호출마다 기다리는 밀리초
 * @returns {{
 *   getUser: function((string|number)): Promise<(object|null)>,
 *   updateUser: function((string|number), object): Promise<(object|null)>,
 *   calls: number
 * }} getUser, updateUser 메서드와 호출 횟수 calls를 가진 객체
 */
export function createFakeDb({ users, delayMs = 100 }) {
  const rows = new Map(users.map((u) => [String(u.id), { ...u }])); // 데모 원본이므로 Map을 써도 된다
  let calls = 0;
  const wait = () => new Promise((r) => setTimeout(r, delayMs));

  return {
    /**
     * 사용자를 조회한다.
     * @param {(string|number)} id 사용자 id
     * @returns {Promise<(object|null)>} 사용자 복사본. 없으면 null
     */
    async getUser(id) {
      calls++;
      await wait();
      const u = rows.get(String(id));
      return u ? { ...u } : null;
    },

    /**
     * 사용자 필드를 수정한다. id는 바꾸지 않는다.
     * @param {(string|number)} id 사용자 id
     * @param {object} fields 덮어쓸 필드
     * @returns {Promise<(object|null)>} 수정된 사용자 복사본. 없으면 null
     */
    async updateUser(id, fields) {
      calls++;
      await wait();
      const u = rows.get(String(id));
      if (!u) return null;
      Object.assign(u, fields, { id: u.id });
      return { ...u };
    },

    /**
     * getUser와 updateUser 호출 횟수의 합.
     * @type {number}
     */
    get calls() {
      return calls;
    },
  };
}
