## Modules

<dl>
<dt><a href="#module_hashtable">hashtable</a></dt>
<dd><p>체이닝 해시 테이블. 모든 함수는 동기 함수이다.</p>
</dd>
<dt><a href="#module_store">store</a></dt>
<dd><p>키-값 저장소 명령 계층. HashTable 위에 SET/GET/DEL/EXPIRE와 lazy 만료를 올린다.
모든 함수는 동기 함수이다(동시성 보장의 근거).</p>
</dd>
</dl>

<a name="module_hashtable"></a>

## hashtable
체이닝 해시 테이블. 모든 함수는 동기 함수이다.


* [hashtable](#module_hashtable)
    * [.HashTable](#module_hashtable.HashTable)
        * [.size](#module_hashtable.HashTable+size) : <code>number</code>
        * [.capacity](#module_hashtable.HashTable+capacity) : <code>number</code>
        * [.get(key)](#module_hashtable.HashTable+get) ⇒ <code>\*</code>
        * [.set(key, value)](#module_hashtable.HashTable+set) ⇒ <code>void</code>
        * [.delete(key)](#module_hashtable.HashTable+delete) ⇒ <code>boolean</code>
    * [.fnv1a(str)](#module_hashtable.fnv1a) ⇒ <code>number</code>

<a name="module_hashtable.HashTable"></a>

### hashtable.HashTable
체이닝 방식 해시 테이블. 키는 문자열이다.
초기 capacity는 8이고, 추가 후 `size / capacity > 1`이면
capacity를 2배로 늘리고 모든 노드를 한 번에 rehash한다.

**Kind**: static class of [<code>hashtable</code>](#module_hashtable)  

* [.HashTable](#module_hashtable.HashTable)
    * [.size](#module_hashtable.HashTable+size) : <code>number</code>
    * [.capacity](#module_hashtable.HashTable+capacity) : <code>number</code>
    * [.get(key)](#module_hashtable.HashTable+get) ⇒ <code>\*</code>
    * [.set(key, value)](#module_hashtable.HashTable+set) ⇒ <code>void</code>
    * [.delete(key)](#module_hashtable.HashTable+delete) ⇒ <code>boolean</code>

<a name="module_hashtable.HashTable+size"></a>

#### hashTable.size : <code>number</code>
저장된 키의 개수.

**Kind**: instance property of [<code>HashTable</code>](#module_hashtable.HashTable)  
<a name="module_hashtable.HashTable+capacity"></a>

#### hashTable.capacity : <code>number</code>
현재 버킷 개수. 초기값은 8이다.

**Kind**: instance property of [<code>HashTable</code>](#module_hashtable.HashTable)  
<a name="module_hashtable.HashTable+get"></a>

#### hashTable.get(key) ⇒ <code>\*</code>
키의 값을 찾는다.

**Kind**: instance method of [<code>HashTable</code>](#module_hashtable.HashTable)  
**Returns**: <code>\*</code> - 값. 키가 없으면 `undefined`  

| Param | Type | Description |
| --- | --- | --- |
| key | <code>string</code> | 찾을 키 |

<a name="module_hashtable.HashTable+set"></a>

#### hashTable.set(key, value) ⇒ <code>void</code>
키에 값을 저장한다. 이미 있는 키는 값을 덮어쓰며 size를 늘리지 않는다.
새 키는 체인 머리에 추가하고, 부하율이 1을 넘으면 확장한다.

**Kind**: instance method of [<code>HashTable</code>](#module_hashtable.HashTable)  

| Param | Type | Description |
| --- | --- | --- |
| key | <code>string</code> | 저장할 키 |
| value | <code>\*</code> | 저장할 값 |

<a name="module_hashtable.HashTable+delete"></a>

#### hashTable.delete(key) ⇒ <code>boolean</code>
키를 삭제한다.

**Kind**: instance method of [<code>HashTable</code>](#module_hashtable.HashTable)  
**Returns**: <code>boolean</code> - 지웠으면 `true`, 키가 없었으면 `false`  

| Param | Type | Description |
| --- | --- | --- |
| key | <code>string</code> | 삭제할 키 |

<a name="module_hashtable.fnv1a"></a>

### hashtable.fnv1a(str) ⇒ <code>number</code>
FNV-1a 32비트 해시를 계산한다.
입력은 `charCodeAt`로 읽은 UTF-16 코드 단위이다.

**Kind**: static method of [<code>hashtable</code>](#module_hashtable)  
**Returns**: <code>number</code> - 부호 없는 32비트 정수 (0 ~ 2^32-1)  

| Param | Type | Description |
| --- | --- | --- |
| str | <code>string</code> | 해시할 문자열 |

<a name="module_store"></a>

## store
키-값 저장소 명령 계층. HashTable 위에 SET/GET/DEL/EXPIRE와 lazy 만료를 올린다.
모든 함수는 동기 함수이다(동시성 보장의 근거).


* [store](#module_store)
    * _static_
        * [.createStore([options])](#module_store.createStore) ⇒ <code>Object</code>
    * _inner_
        * [~set(key, value, [ttlSec])](#module_store..set) ⇒ <code>true</code>
        * [~get(key)](#module_store..get) ⇒ <code>string</code> \| <code>null</code>
        * [~del(key)](#module_store..del) ⇒ <code>boolean</code>
        * [~expire(key, ttlSec)](#module_store..expire) ⇒ <code>boolean</code>

<a name="module_store.createStore"></a>

### store.createStore([options]) ⇒ <code>Object</code>
저장소를 만든다. 엔트리는 `{ value, expireAt }`(밀리초, 만료 없으면 `null`)이다.
모든 명령은 키를 찾은 뒤 만료를 먼저 확인하고, 만료되었으면 삭제하고 없음으로 취급한다.

**Kind**: static method of [<code>store</code>](#module_store)  
**Returns**: <code>Object</code> - 명령 객체  

| Param | Type | Default | Description |
| --- | --- | --- | --- |
| [options] | <code>object</code> |  |  |
| [options.now] | <code>function</code> | <code>Date.now</code> | 현재 시각(밀리초)을 돌려주는 시계 |
| [options.table] | <code>HashTable</code> | <code>new HashTable()</code> | 엔트리를 담을 해시 테이블 |

<a name="module_store..set"></a>

### store~set(key, value, [ttlSec]) ⇒ <code>true</code>
값을 저장한다. 기존 값은 덮어쓰고, ttl이 없으면 기존 만료도 없앤다.

**Kind**: inner method of [<code>store</code>](#module_store)  

| Param | Type | Description |
| --- | --- | --- |
| key | <code>string</code> | 키 |
| value | <code>string</code> | 값 |
| [ttlSec] | <code>number</code> | 만료까지 초. 검증은 호출하는 쪽이 한다 |

<a name="module_store..get"></a>

### store~get(key) ⇒ <code>string</code> \| <code>null</code>
값을 조회한다.

**Kind**: inner method of [<code>store</code>](#module_store)  
**Returns**: <code>string</code> \| <code>null</code> - 값. 없거나 만료되었으면 `null`  

| Param | Type | Description |
| --- | --- | --- |
| key | <code>string</code> | 키 |

<a name="module_store..del"></a>

### store~del(key) ⇒ <code>boolean</code>
키를 지운다.

**Kind**: inner method of [<code>store</code>](#module_store)  
**Returns**: <code>boolean</code> - 지웠으면 `true`, 없거나 만료되었으면 `false`  

| Param | Type | Description |
| --- | --- | --- |
| key | <code>string</code> | 키 |

<a name="module_store..expire"></a>

### store~expire(key, ttlSec) ⇒ <code>boolean</code>
만료 시각을 `now() + ttlSec * 1000`으로 다시 잡는다.

**Kind**: inner method of [<code>store</code>](#module_store)  
**Returns**: <code>boolean</code> - 설정했으면 `true`, 없거나 만료되었으면 `false`  

| Param | Type | Description |
| --- | --- | --- |
| key | <code>string</code> | 키 |
| ttlSec | <code>number</code> | 만료까지 초. 검증은 호출하는 쪽이 한다 |

