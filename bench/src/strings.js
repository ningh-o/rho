// bench kernel: build one string by appending 200k small pieces (cycled
// "a", "bc", "def", "ghij" -> 500000 bytes). JS's string append is `+=`
// (V8 builds a cons string and flattens on demand). Checksum: final
// length + char-code sum (ASCII, so char codes == bytes).
"use strict";
const PIECES = ["a", "bc", "def", "ghij"];
let s = "";
for (let i = 0; i < 200000; i++) s += PIECES[i & 3];
let sum = 0;
for (let j = 0; j < s.length; j++) sum += s.charCodeAt(j);
console.log(`strings n=${s.length} bytes=${sum}`);
