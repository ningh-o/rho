// Renders each tutorial example into a code panel with an edit toggle and
// a Run button. Editing is pure client-side (the tutorial page never loads
// the compiler); Run hands the CURRENT text — edited or not — to the
// playground through the share hash, which the playground runs as-is.
import { EXAMPLES } from "./examples.js";
import { highlightRho } from "./highlight.js";

function findExample(id) {
  return EXAMPLES.find((e) => e.id === id);
}

document.querySelectorAll("template[data-example]").forEach((tpl) => {
  const ex = findExample(tpl.dataset.example);
  if (!ex) return;
  const div = document.createElement("div");
  div.className = "example";
  div.innerHTML = `
    <div class="bar"><span>${ex.id}.rho</span><span class="spacer"></span>
      <button class="edit">edit</button>
      <button class="run">Run ▸</button></div>
    <pre class="code"></pre>
    <textarea class="code-edit" hidden spellcheck="false" autocapitalize="off" autocomplete="off" autocorrect="off"></textarea>`;
  let current = ex.code;
  const pre = div.querySelector("pre.code");
  const ta = div.querySelector("textarea.code-edit");
  const editBtn = div.querySelector(".edit");
  pre.innerHTML = highlightRho(current);
  editBtn.addEventListener("click", () => {
    if (ta.hidden) {
      ta.value = current;
      ta.hidden = false;
      pre.hidden = true;
      editBtn.textContent = "done";
      ta.focus();
    } else {
      current = ta.value;
      pre.innerHTML = highlightRho(current);
      ta.hidden = true;
      pre.hidden = false;
      editBtn.textContent = "edit";
    }
  });
  div.querySelector(".run").addEventListener("click", () => {
    const src = ta.hidden ? current : ta.value;
    location.href =
      "playground.html#code=" + encodeURIComponent(btoa(unescape(encodeURIComponent(src))));
  });
  tpl.replaceWith(div);
});
