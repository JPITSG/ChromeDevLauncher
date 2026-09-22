export interface ConfigData {
  chromePath: string;
  debugPort: number;
  connectAddress: string;
  statusCheckInterval: number;
  autoCheckForUpdates: boolean;
  updateCheckPending: boolean;
  updatePromptPending: boolean;
}

export interface InitData {
  view: "config";
  config: ConfigData;
  updateCompletedVersion: string;
}

export interface BrowseResult {
  path: string;
}

export interface UpdateResult {
  status:
    | "newer"
    | "same"
    | "older"
    | "cancelled"
    | "error"
    | "completed";
  title: string;
  message: string;
  currentVersion: string;
  remoteVersion: string;
  automatic: boolean;
}

export interface UpdateProgress {
  percent: number;
}

type InitCallback = (data: InitData) => void;
type BrowseResultCallback = (result: BrowseResult) => void;

let initCallback: InitCallback | null = null;
let browseResultCallback: BrowseResultCallback | null = null;
let updateResultCallback: ((result: UpdateResult) => void) | null = null;
let updateProgressCallback: ((progress: UpdateProgress) => void) | null = null;

declare global {
  interface Window {
    onInit: (data: InitData) => void;
    onBrowseResult: (result: BrowseResult) => void;
    onUpdateResult: (result: UpdateResult) => void;
    onUpdateProgress: (progress: UpdateProgress) => void;
    chrome?: {
      webview?: {
        postMessage: (s: string) => void;
      };
    };
  }
}

// Called by C via ExecuteScript.
window.onInit = (data: InitData) => {
  initCallback?.(data);
};

window.onBrowseResult = (result: BrowseResult) => {
  browseResultCallback?.(result);
};

window.onUpdateResult = (result: UpdateResult) => {
  updateResultCallback?.(result);
};

window.onUpdateProgress = (progress: UpdateProgress) => {
  updateProgressCallback?.(progress);
};

export function onInit(cb: InitCallback) {
  initCallback = cb;
}

export function onBrowseResult(cb: BrowseResultCallback) {
  browseResultCallback = cb;
  return () => {
    if (browseResultCallback === cb) browseResultCallback = null;
  };
}

export function onUpdateResult(cb: (result: UpdateResult) => void) {
  updateResultCallback = cb;
  return () => {
    if (updateResultCallback === cb) updateResultCallback = null;
  };
}

export function onUpdateProgress(cb: (progress: UpdateProgress) => void) {
  updateProgressCallback = cb;
  return () => {
    if (updateProgressCallback === cb) updateProgressCallback = null;
  };
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
    autoCheckForUpdates: config.autoCheckForUpdates,
  });
}

export function browseFile() {
  postMessage({ action: "browse" });
}

export function closeDialog() {
  postMessage({ action: "close" });
}

export function configReady(checkAutomatically = false) {
  postMessage({ action: "configReady", checkAutomatically });
}

export function checkForUpdate(automatic = false) {
  postMessage({ action: "checkUpdate", automatic });
}

export function cancelUpdateCheck() {
  postMessage({ action: "cancelUpdateCheck" });
}

export function installUpdate(reopenSettings = false) {
  postMessage({ action: "installUpdate", reopenSettings });
}

export function dismissUpdate() {
  postMessage({ action: "dismissUpdate" });
}

export function ignoreUpdateVersion(version: string) {
  postMessage({ action: "ignoreUpdateVersion", version });
}

export function dismissUpdateConfirmation() {
  postMessage({ action: "dismissUpdateConfirmation" });
}

export function reportHeight(height: number) {
  postMessage({ action: "resize", height });
}
