

function finished() {
  process.exit(0);
}
let number = 0;
setTimeout(foo, 0);
function foo() {
  try {
    bar();
  } catch (e) {}
  setTimeout(number == 10 ? finished : foo, 0);
}
function bar() {
  number++;
  throw { number };
}
