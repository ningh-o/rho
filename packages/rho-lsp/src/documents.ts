// documents.ts — the open-document store. Full-text sync only (the
// wire stays whole-document; incrementality is an inside-the-core
// question the shell never sees). Oversized documents are marked and
// SKIPPED — their diagnostics are cleared, never stale (ecosystem.md
// §4: degrade, never wrong).
//
// Pins: test/documents.test.ts.

import { dirname } from "node:path";

export const DEFAULT_MAX_DOC_BYTES = 512 * 1024;

export interface Document {
  uri: string;
  /** Editor-facing filesystem path when the uri is a file:, else undefined. */
  path?: string;
  version: number;
  text: string;
  /** UTF-8 byte size at last sync; over the cap the doc is skipped. */
  bytes: number;
}

export class DocumentStore {
  private readonly docs = new Map<string, Document>();

  constructor(private readonly maxBytes: number = DEFAULT_MAX_DOC_BYTES) {}

  open(uri: string, version: number, text: string, docPath?: string): Document {
    const doc: Document = {
      uri,
      ...(docPath !== undefined ? { path: docPath } : {}),
      version,
      text,
      bytes: Buffer.byteLength(text, "utf8"),
    };
    this.docs.set(uri, doc);
    return doc;
  }

  /** Full-sync change; unknown uri returns undefined (protocol misuse). */
  change(uri: string, version: number, text: string): Document | undefined {
    const doc = this.docs.get(uri);
    if (!doc) return undefined;
    doc.version = version;
    doc.text = text;
    doc.bytes = Buffer.byteLength(text, "utf8");
    return doc;
  }

  close(uri: string): boolean {
    return this.docs.delete(uri);
  }

  get(uri: string): Document | undefined {
    return this.docs.get(uri);
  }

  /** All open documents (didOpen order not preserved by Map? it is — insertion order). */
  all(): Document[] {
    return [...this.docs.values()];
  }

  /** Open documents that are file: siblings of the given path. */
  siblingsOf(doc: Document): Document[] {
    if (doc.path === undefined) return [];
    const dir = dirname(doc.path);
    return this.all().filter((d) => d.path !== undefined && d.path !== doc.path && dirname(d.path) === dir && d.path.endsWith(".rho"));
  }

  overCap(doc: Document): boolean {
    return doc.bytes > this.maxBytes;
  }
}
