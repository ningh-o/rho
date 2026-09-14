// Renders each tutorial example into a code panel with a Run button.
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
      <button class="run">Run ▸</button></div>
    <pre class="code"></pre>`;
  const code = div.querySelector("pre.code");
  code.innerHTML = highlightRho(ex.code);
  div.querySelector(".run").addEventListener("click", () => {
    location.href = "playground.html?example=" + encodeURIComponent(ex.id);
  });
  tpl.replaceWith(div);
});
