/**
 * Vitest global setup — polyfills DOMParser for the Node test environment.
 * In the browser, DOMParser is native and this polyfill is not used.
 */
import { DOMParser as XmldomDOMParser } from "@xmldom/xmldom";

if (typeof globalThis.DOMParser === "undefined") {
    (globalThis as unknown as { DOMParser: typeof XmldomDOMParser }).DOMParser = XmldomDOMParser;
}
