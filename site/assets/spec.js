// The spec reader: fetches the markdown and renders it in place — no
// dependencies, no build step. The subset below is what the four spec
// documents actually use: headings, fenced code (highlighted as rho),
// paragraphs, inline code / bold / links, unordered and ordered lists
// (nested), pipe tables, --- rules, and > quotes. The raw .md stays
// one click away ("source").

const SPEC_DOCS = [
  { slug: "spec", title: "Overview", file: "spec/spec.md" },
  { slug: "syntax", title: "Syntax", file: "spec/syntax.md" },
  { slug: "type-system", title: "Type system", file: "spec/type-system.md" },
  { slug: "module-system", title: "Modules", file: "spec/module-system.md" },
];

function esc(s) {
  return s.replace(/&/g, "&amp;").replace(/</g, "&lt;").replace(/>/g, "&gt;");
}

// inline pass: `code`, **bold**, [text](href). Order matters — code first
// so emphasis markers inside backticks survive.
function inline(md) {
  let out = "";
  let i = 0;
  while (i < md.length) {
    const ch = md[i];
    if (ch === "`") {
      const end = md.indexOf("`", i + 1);
      if (end > i) {
        out += `<code>${esc(md.slice(i + 1, end))}</code>`;
        i = end + 1;
        continue;
      }
    }
    if (ch === "*" && md[i + 1] === "*") {
      const end = md.indexOf("**", i + 2);
      if (end > i + 1) {
        out += `<b>${inline(md.slice(i + 2, end))}</b>`;
        i = end + 2;
        continue;
      }
    }
    if (ch === "[") {
      const m = /^\[([^\]]+)\]\(([^)\s]+)\)/.exec(md.slice(i));
      if (m) {
        const href = m[2];
        const local = /^(?:https?:\/\/|\/|#)/.test(href)
          ? href
          : resolveDoc(href);
        out += `<a href="${esc(local)}">${esc(m[1])}</a>`;
        i += m[0].length;
        continue;
      }
    }
    out += esc(ch);
    i++;
  }
  return out;
}

// relative .md links between the spec files become reader links
function resolveDoc(href) {
  const base = href.split("/").pop().replace(/\.md$/, "");
  const hit = SPEC_DOCS.find((d) => d.slug === base);
  return hit ? `spec.html?doc=${hit.slug}` : href;
}

function renderMarkdown(md) {
  const lines = md.split("\n");
  let out = "";
  let i = 0;
  let listStack = []; // "ul" | "ol"

  const closeLists = (to) => {
    while (listStack.length > to) {
      out += `</${listStack.pop()}>`;
    }
  };
  const headingId = (text, seen) => {
    let id = text
      .toLowerCase()
      .replace(/[^a-z0-9]+/g, "-")
      .replace(/^-|-$/g, "");
    while (seen.has(id)) id = id + "-x";
    seen.add(id);
    return id;
  };
  const seenIds = new Set();

  while (i < lines.length) {
    const line = lines[i];

    if (!line.trim()) {
      i++;
      continue;
    }
    if (/^---+\s*$/.test(line)) {
      closeLists(0);
      out += "<hr>";
      i++;
      continue;
    }
    const h = /^(#{1,4})\s+(.*)$/.exec(line);
    if (h) {
      closeLists(0);
      const level = h[1].length + 1; // # → h2: the page owns h1
      const text = h[2].replace(/`/g, "").replace(/\*\*/g, "");
      const id = headingId(text, seenIds);
      out += `<h${level} id="${id}">${inline(h[2])}</h${level}>`;
      i++;
      continue;
    }
    if (line.startsWith("```")) {
      closeLists(0);
      i++;
      const code = [];
      while (i < lines.length && !lines[i].startsWith("```")) {
        code.push(lines[i]);
        i++;
      }
      i++; // closing fence
      out += `<pre class="code rho-code">${highlightRho(code.join("\n"))}</pre>`;
      continue;
    }
    if (line.startsWith("> ")) {
      closeLists(0);
      const quote = [];
      while (i < lines.length && lines[i].startsWith("> ")) {
        quote.push(lines[i].slice(2));
        i++;
      }
      out += `<blockquote><p>${inline(quote.join(" "))}</p></blockquote>`;
      continue;
    }
    if (line.startsWith("|") && i + 1 < lines.length && /^\|[-\s|:]+\|$/.test(lines[i + 1])) {
      closeLists(0);
      // split on real cell separators only: pipes inside `inline code`
      // (`\|\|` in the precedence table) are content, not boundaries
      const cells = (row) => {
        const parts = [];
        let cur = "";
        let inCode = false;
        for (let k = 1; k < row.length - 1; k++) {
          const ch = row[k];
          if (ch === "`") inCode = !inCode;
          if (ch === "|" && !inCode) {
            parts.push(cur.trim());
            cur = "";
            continue;
          }
          cur += ch;
        }
        parts.push(cur.trim());
        return parts;
      };
      const head = cells(line);
      out += "<table><thead><tr>";
      for (const c of head) out += `<th>${inline(c)}</th>`;
      out += "</tr></thead><tbody>";
      i += 2;
      while (i < lines.length && lines[i].startsWith("|")) {
        out += "<tr>";
        for (const c of cells(lines[i])) out += `<td>${inline(c)}</td>`;
        out += "</tr>";
        i++;
      }
      out += "</tbody></table>";
      continue;
    }
    const li = /^(\s*)([-*]|\d+\.)\s+(.*)$/.exec(line);
    if (li) {
      const depth = Math.floor(li[1].length / 2);
      const kind = li[2] === "-" || li[2] === "*" ? "ul" : "ol";
      while (listStack.length > depth + 1) out += `</${listStack.pop()}>`;
      while (listStack.length < depth + 1) {
        out += `<${kind}>`;
        listStack.push(kind);
      }
      if (listStack[listStack.length - 1] !== kind) {
        out += `</${listStack.pop()}>`;
        out += `<${kind}>`;
        listStack.push(kind);
      }
      out += `<li>${inline(li[3])}</li>`;
      i++;
      continue;
    }
    // paragraph: gather until a blank line or a block starter
    closeLists(0);
    const para = [line];
    i++;
    while (
      i < lines.length &&
      lines[i].trim() &&
      !/^(#{1,4}\s|```|\||> |-|\d+\.\s)/.test(lines[i])
    ) {
      para.push(lines[i]);
      i++;
    }
    out += `<p>${inline(para.join(" "))}</p>`;
  }
  closeLists(0);
  return out;
}

// ---- page wiring -------------------------------------------------------------

function docFromQuery() {
  const slug = new URLSearchParams(location.search).get("doc");
  return SPEC_DOCS.find((d) => d.slug === slug) || SPEC_DOCS[0];
}

function navHtml(active) {
  return SPEC_DOCS.map(
    (d) =>
      `<a class="spec-tab${d.slug === active ? " on" : ""}" href="spec.html?doc=${d.slug}">${d.title}</a>`,
  ).join("");
}

async function loadSpec() {
  const doc = docFromQuery();
  document.getElementById("spec-tabs").innerHTML = navHtml(doc.slug);
  const raw = document.getElementById("spec-raw");
  raw.href = doc.file;
  const body = document.getElementById("spec-body");
  body.innerHTML = '<p class="spec-loading">loading…</p>';
  try {
    const res = await fetch(doc.file);
    if (!res.ok) throw new Error(res.status);
    const md = await res.text();
    body.innerHTML = renderMarkdown(md);
  } catch (e) {
    body.innerHTML = `<p>Could not load <code>${esc(doc.file)}</code> (${esc(String(e))}). <a href="${doc.file}">Read the raw markdown instead.</a></p>`;
  }
}

// links inside rendered docs point at spec.html?doc=…; keep the reader in place
document.addEventListener("click", (e) => {
  const a = e.target.closest("a");
  if (!a || !a.href.includes("spec.html?doc=")) return;
  e.preventDefault();
  const slug = new URL(a.href).searchParams.get("doc");
  history.pushState(null, "", `spec.html?doc=${slug}`);
  loadSpec();
  window.scrollTo(0, 0);
});
window.addEventListener("popstate", loadSpec);
