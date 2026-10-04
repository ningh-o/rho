// jsonrpc.ts — LSP wire framing over a byte stream: HTTP-style
// `Content-Length` headers, UTF-8 byte counts, JSON bodies. This is the
// whole transport knowledge the server needs; the law is exactness — a
// frame's Content-Length counts UTF-8 BYTES, never string length.
//
// Pins: test/jsonrpc.test.ts (framing round-trip, split reads, CJK and
// emoji payloads whose string length differs from their byte length).

/** One parsed incoming message. */
export interface Frame {
  payload: string;
}

/**
 * Incremental frame reader. Feed raw bytes as they arrive (partial
 * reads are expected); completed frames surface one by one. Leftover
 * bytes stay buffered until their frame completes.
 */
export class FrameReader {
  private buf = Buffer.alloc(0);

  constructor(private readonly onFrame: (payload: string) => void) {}

  /** Feed a chunk of raw bytes from the stream. */
  feed(chunk: Buffer): void {
    this.buf = this.buf.length === 0 ? Buffer.from(chunk) : Buffer.concat([this.buf, chunk]);
    for (;;) {
      const frame = this.tryParse();
      if (frame === null) break;
      this.onFrame(frame);
    }
  }

  /** True when a partial header or body is still buffered. */
  get pending(): boolean {
    return this.buf.length > 0;
  }

  private tryParse(): string | null {
    const headerEnd = this.buf.indexOf("\r\n\r\n");
    if (headerEnd < 0) return null;
    const headerText = this.buf.subarray(0, headerEnd).toString("utf8");
    let contentLength = -1;
    for (const line of headerText.split("\r\n")) {
      const colon = line.indexOf(":");
      if (colon < 0) continue;
      const name = line.slice(0, colon).trim().toLowerCase();
      if (name === "content-length") {
        const value = Number(line.slice(colon + 1).trim());
        if (Number.isInteger(value) && value >= 0) contentLength = value;
      }
    }
    if (contentLength < 0) {
      // Malformed header block: drop it through the header delimiter so
      // the stream can resynchronize; the payload is unrecoverable by
      // design (never guess a length).
      this.buf = this.buf.subarray(headerEnd + 4);
      return null;
    }
    const bodyStart = headerEnd + 4;
    if (this.buf.length < bodyStart + contentLength) return null;
    const payload = this.buf.subarray(bodyStart, bodyStart + contentLength).toString("utf8");
    this.buf = this.buf.subarray(bodyStart + contentLength);
    return payload;
  }
}

/** Serialize one outgoing message into framed bytes. */
export function writeFrame(message: unknown): Buffer {
  const json = JSON.stringify(message);
  // The law: Content-Length is the UTF-8 byte length of the payload.
  const byteLength = Buffer.byteLength(json, "utf8");
  const header = `Content-Length: ${byteLength}\r\n\r\n`;
  return Buffer.concat([Buffer.from(header, "utf8"), Buffer.from(json, "utf8")]);
}

/** Parse a framed payload as a JSON-RPC message shape. */
export interface JsonRpcMessage {
  jsonrpc: "2.0";
  id?: number | string | null;
  method?: string;
  params?: unknown;
  result?: unknown;
  error?: { code: number; message: string; data?: unknown };
}

export function parseMessage(payload: string): JsonRpcMessage | null {
  try {
    const value = JSON.parse(payload) as JsonRpcMessage;
    if (value === null || typeof value !== "object") return null;
    return value;
  } catch {
    return null;
  }
}
