export interface ConfigData {
  chromePath: string;
  debugPort: number;
  connectAddress: string;
  statusCheckInterval: number;
}

export interface InitData {
  view: "config";
  config: ConfigData;
}

export interface BrowseResult {
  path: string;
}

type InitCallback = (data: InitData) => void;
type BrowseResultCallback = (result: BrowseResult) => void;

let initCallback: InitCallback | null = null;
let browseResultCallback: BrowseResultCallback | null = null;

// Extend window for C <-> JS bridge
declare global {
  interface Window {
    onInit: (data: InitData) => void;
    onBrowseResult: (result: BrowseResult) => void;
    chrome?: {
      webview?: {
        postMessage: (s: string) => void;
      };
    };
  }
}

// Called by C via ExecuteScript
window.onInit = (data: InitData) => {
  initCallback?.(data);
};

window.onBrowseResult = (result: BrowseResult) => {
  browseResultCallback?.(result);
};

export function onInit(cb: InitCallback) {
  initCallback = cb;
}

export function onBrowseResult(cb: BrowseResultCallback) {
  browseResultCallback = cb;
}

function postMessage(msg: Record<string, unknown>) {
  try {
    window.chrome?.webview?.postMessage(JSON.stringify(msg));
  } catch {
    console.log("postMessage (no WebView2):", msg);
  }
}

export function getInit() {
  postMessage({ action: "getInit" });
}

export function saveSettings(config: ConfigData) {
  postMessage({
    action: "saveSettings",
    chromePath: config.chromePath,
    debugPort: config.debugPort,
    connectAddress: config.connectAddress,
    statusCheckInterval: config.statusCheckInterval,
  });
}

export function browseFile() {
  postMessage({ action: "browse" });
}

export function closeDialog() {
  postMessage({ action: "close" });
}

export function reportHeight(height: number) {
  postMessage({ action: "resize", height });
}
