import { useCallback, useEffect, useLayoutEffect, useRef, useState } from "react";
import ConfigAlert from "./components/ConfigAlert";
import {
  type ConfigData,
  type UpdateResult,
  saveSettings,
  browseFile,
  closeDialog,
  onCloseRequested,
  checkForUpdate,
  cancelUpdateCheck,
  configReady,
  installUpdate,
  dismissUpdate,
  ignoreUpdateVersion,
  dismissUpdateConfirmation,
  onBrowseResult,
  onUpdateResult,
  onUpdateProgress,
} from "./lib/bridge";
import { Button } from "./components/ui/button";
import { Checkbox } from "./components/ui/checkbox";
import { Input } from "./components/ui/input";
import { Label } from "./components/ui/label";

interface Props {
  config: ConfigData;
  updateCompletedVersion: string;
}

export default function ConfigView({
  config,
  updateCompletedVersion,
}: Props) {
  const [chromePath, setChromePath] = useState(config.chromePath);
  const [debugPort, setDebugPort] = useState(String(config.debugPort));
  const [connectAddress, setConnectAddress] = useState(config.connectAddress);
  const [statusCheckInterval, setStatusCheckInterval] = useState(
    String(config.statusCheckInterval)
  );
  const [startWithWindows, setStartWithWindows] = useState(
    config.startWithWindows ?? false
  );
  const [autoCheckForUpdates, setAutoCheckForUpdates] = useState(
    config.autoCheckForUpdates ?? true
  );
  const [errors, setErrors] = useState<Record<string, string>>({});
  const [updateChecking, setUpdateChecking] = useState(
    config.updateCheckPending ?? false
  );
  const [updateCancelling, setUpdateCancelling] = useState(false);
  const [updateProgressPercent, setUpdateProgressPercent] = useState<
    number | null
  >(null);
  const [updateAlert, setUpdateAlert] = useState<UpdateResult | null>(() =>
    updateCompletedVersion
      ? {
          status: "completed",
          title: "Update complete",
          message: `Chrome Developer Launcher has been updated to version ${updateCompletedVersion}.`,
          currentVersion: "",
          remoteVersion: "",
          automatic: false,
        }
      : null
  );
  const [reopenSettings, setReopenSettings] = useState(false);
  const automaticUpdateStarted = useRef(false);
  const [closePrompt, setClosePrompt] = useState(false);
  const hasChanges =
    chromePath !== config.chromePath ||
    debugPort !== String(config.debugPort) ||
    connectAddress !== config.connectAddress ||
    statusCheckInterval !== String(config.statusCheckInterval) ||
    startWithWindows !== (config.startWithWindows ?? false) ||
    autoCheckForUpdates !== (config.autoCheckForUpdates ?? true);

  const handleRequestClose = useCallback(() => {
    if (hasChanges) {
      setClosePrompt(true);
    } else {
      closeDialog();
    }
  }, [hasChanges]);

  // Install before configReady, and keep native close requests in sync with edits.
  useLayoutEffect(() => onCloseRequested(handleRequestClose), [handleRequestClose]);

  useEffect(() => {
    const handleKeyDown = (event: KeyboardEvent) => {
      if (event.key === "Escape" && !closePrompt && !updateAlert) {
        event.preventDefault();
        handleRequestClose();
      }
    };
    document.addEventListener("keydown", handleKeyDown);
    return () => document.removeEventListener("keydown", handleKeyDown);
  }, [closePrompt, updateAlert, handleRequestClose]);

  useEffect(() => {
    const removeBrowseListener = onBrowseResult((result) => {
      if (result.path) setChromePath(result.path);
    });
    const removeResultListener = onUpdateResult((result) => {
      setReopenSettings(false);
      setUpdateChecking(false);
      setUpdateCancelling(false);
      setUpdateProgressPercent(null);
      if (result.status === "cancelled") {
        setUpdateAlert((current) =>
          result.automatic && current?.status === "completed" ? current : null
        );
      } else if (result.automatic && result.status !== "newer") {
        setUpdateAlert((current) =>
          current?.status === "completed" ? current : null
        );
      } else {
        setUpdateAlert(result);
      }
    });
    const removeProgressListener = onUpdateProgress((progress) => {
      setUpdateProgressPercent(
        Math.min(100, Math.max(0, Math.floor(progress.percent)))
      );
    });

    const shouldCheckAutomatically =
      config.autoCheckForUpdates &&
      !updateCompletedVersion &&
      !config.updateCheckPending &&
      !config.updatePromptPending &&
      !automaticUpdateStarted.current;
    if (shouldCheckAutomatically) {
      automaticUpdateStarted.current = true;
      setUpdateChecking(true);
    }
    configReady(shouldCheckAutomatically);

    return () => {
      removeBrowseListener();
      removeResultListener();
      removeProgressListener();
    };
  }, [
    config.autoCheckForUpdates,
    config.updateCheckPending,
    config.updatePromptPending,
    updateCompletedVersion,
  ]);

  const validate = (): boolean => {
    const newErrors: Record<string, string> = {};

    const port = parseInt(debugPort, 10);
    if (isNaN(port) || port < 1 || port > 65535) {
      newErrors.debugPort = "Port must be between 1 and 65535";
    }

    const interval = parseInt(statusCheckInterval, 10);
    if (isNaN(interval) || interval < 5) {
      newErrors.statusCheckInterval = "Interval must be at least 5 seconds";
    }

    setErrors(newErrors);
    const firstInvalidField = Object.keys(newErrors)[0];
    if (firstInvalidField) {
      setClosePrompt(false);
      requestAnimationFrame(() => document.getElementById(firstInvalidField)?.focus());
      return false;
    }
    return true;
  };

  function handleSave() {
    if (!validate()) return;

    saveSettings({
      chromePath,
      debugPort: parseInt(debugPort, 10),
      connectAddress: connectAddress || "127.0.0.1",
      statusCheckInterval: parseInt(statusCheckInterval, 10),
      startWithWindows,
      autoCheckForUpdates,
      updateCheckPending: config.updateCheckPending,
      updatePromptPending: config.updatePromptPending,
    });
  }

  function handleUpdate() {
    if (updateChecking) {
      setUpdateCancelling(true);
      cancelUpdateCheck();
      return;
    }
    setReopenSettings(false);
    setUpdateAlert(null);
    setUpdateChecking(true);
    setUpdateCancelling(false);
    setUpdateProgressPercent(null);
    checkForUpdate(false);
  }

  function handleInstallUpdate() {
    setUpdateChecking(true);
    setUpdateCancelling(false);
    setUpdateProgressPercent(null);
    installUpdate(reopenSettings);
  }

  function handleDismissUpdate() {
    if (updateAlert?.status === "completed") {
      dismissUpdateConfirmation();
    } else {
      dismissUpdate();
    }
    setReopenSettings(false);
    setUpdateAlert(null);
  }

  function handleIgnoreUpdateVersion() {
    if (!updateAlert?.remoteVersion) return;
    ignoreUpdateVersion(updateAlert.remoteVersion);
    setReopenSettings(false);
    setUpdateAlert(null);
  }

  return (
    <>
      <div inert={closePrompt || !!updateAlert} className="p-5 flex flex-col gap-3 max-w-md mx-auto text-xs">
        <div className="space-y-1">
          <Label htmlFor="chromePath">Chrome Executable Path</Label>
          <div className="flex gap-1">
            <Input
              id="chromePath"
              value={chromePath}
              onChange={(e) => setChromePath(e.target.value)}
              className="flex-1"
            />
            <Button variant="outline" size="sm" onClick={() => browseFile()}>
              ...
            </Button>
          </div>
        </div>

        <div className="space-y-1">
          <Label htmlFor="debugPort">Debug Port</Label>
          <Input
            id="debugPort"
            type="number"
            value={debugPort}
            onChange={(e) => setDebugPort(e.target.value)}
            min={1}
            max={65535}
          />
          {errors.debugPort && (
            <p className="text-red-500 text-xs">{errors.debugPort}</p>
          )}
        </div>

        <div className="space-y-1">
          <Label htmlFor="connectAddress">Chrome IP Address</Label>
          <Input
            id="connectAddress"
            value={connectAddress}
            onChange={(e) => setConnectAddress(e.target.value)}
            placeholder="127.0.0.1"
          />
        </div>

        <div className="space-y-1">
          <Label htmlFor="statusCheckInterval">Status Check Interval (seconds)</Label>
          <Input
            id="statusCheckInterval"
            type="number"
            value={statusCheckInterval}
            onChange={(e) => setStatusCheckInterval(e.target.value)}
            min={5}
          />
          {errors.statusCheckInterval && (
            <p className="text-red-500 text-xs">{errors.statusCheckInterval}</p>
          )}
        </div>

        <div className="flex items-start gap-2 pt-1">
          <Checkbox
            id="start-with-windows"
            aria-describedby="start-with-windows-description"
            className="mt-0.5"
            checked={startWithWindows}
            onChange={(e) => setStartWithWindows(e.target.checked)}
          />
          <div className="space-y-0.5">
            <Label htmlFor="start-with-windows" className="cursor-pointer">
              Start with Windows
            </Label>
            <p
              id="start-with-windows-description"
              className="text-neutral-500 text-[11px] leading-snug"
            >
              Launches in the tray when you sign in to Windows.
            </p>
          </div>
        </div>

        <div className="flex items-start gap-2 pt-1">
          <Checkbox
            id="autoCheckForUpdates"
            className="mt-0.5"
            checked={autoCheckForUpdates}
            onChange={(e) => setAutoCheckForUpdates(e.target.checked)}
          />
          <div className="space-y-0.5">
            <Label htmlFor="autoCheckForUpdates" className="cursor-pointer">
              Automatically check for updates
            </Label>
            <p className="text-neutral-500 text-[11px] leading-snug">
              Checks at startup, whenever this dialog opens, and every 60 minutes.
              Prompts only when a newer version is available.
            </p>
          </div>
        </div>

        <div className="flex items-center justify-between gap-3 pt-1">
          <span
            className="select-none whitespace-nowrap text-[11px] leading-none tabular-nums text-neutral-400"
            title="Application version"
          >
            v{__APP_VERSION__}
          </span>
          <div className="flex items-center gap-2">
            <Button
              variant={updateChecking ? "destructive" : "outline"}
              size="sm"
              className="min-w-[5rem] tabular-nums"
              disabled={updateCancelling}
              aria-label={
                updateChecking ? "Stop update check and download" : undefined
              }
              title={
                updateChecking ? "Stop update check and download" : undefined
              }
              onClick={handleUpdate}
            >
              {updateCancelling
                ? "Stopping..."
                : updateChecking
                  ? updateProgressPercent === null
                    ? "Checking..."
                    : `Checking (${updateProgressPercent}%)...`
                  : "Update"}
            </Button>
            <Button
              variant="outline"
              size="sm"
              className="min-w-[5rem]"
              onClick={handleRequestClose}
            >
              Cancel
            </Button>
            <Button size="sm" className="min-w-[5rem]" onClick={handleSave}>
              Save
            </Button>
          </div>
        </div>
      </div>

      {closePrompt ? (
        <ConfigAlert
          key="save"
          id="save-alert"
          title="Unsaved changes"
          message="Save changes before closing?"
          onEscape={() => setClosePrompt(false)}
        >
          <div className="flex justify-end gap-2">
            <Button variant="outline" size="sm" onClick={() => setClosePrompt(false)}>
              Keep editing
            </Button>
            <Button variant="outline" size="sm" onClick={closeDialog}>
              Discard
            </Button>
            <Button size="sm" onClick={handleSave}>
              Save
            </Button>
          </div>
        </ConfigAlert>
      ) : updateAlert && (
        <ConfigAlert
          key="update"
          id="update-alert"
          title={updateAlert.title}
          message={updateAlert.message}
        >
          {updateAlert.currentVersion && updateAlert.remoteVersion && (
            <dl className="grid grid-cols-[1fr_auto] gap-x-4 gap-y-1 rounded-md border border-neutral-200 bg-neutral-50 px-3 py-2 text-xs">
              <dt className="text-neutral-500">Current version</dt>
              <dd className="font-medium tabular-nums text-neutral-900">
                {updateAlert.currentVersion}
              </dd>
              <dt className="text-neutral-500">Remote version</dt>
              <dd className="font-medium tabular-nums text-neutral-900">
                {updateAlert.remoteVersion}
              </dd>
            </dl>
          )}
          {(updateAlert.status === "newer" || updateAlert.status === "same") && (
            <div className="flex items-center gap-2">
              <Checkbox
                id="reopenSettings"
                checked={reopenSettings}
                disabled={updateChecking}
                onChange={(e) => setReopenSettings(e.target.checked)}
              />
              <Label htmlFor="reopenSettings" className="cursor-pointer">
                Reopen settings after update
              </Label>
            </div>
          )}
          <div className="flex justify-end gap-2">
            {updateAlert.status === "newer" && updateAlert.automatic && (
              <Button
                variant="outline"
                size="sm"
                disabled={updateChecking}
                onClick={handleIgnoreUpdateVersion}
              >
                Ignore this version
              </Button>
            )}
            {(updateAlert.status === "newer" ||
              updateAlert.status === "same") && (
              <Button
                variant="outline"
                size="sm"
                autoFocus
                disabled={updateChecking}
                onClick={handleDismissUpdate}
              >
                Cancel
              </Button>
            )}
            <Button
              size="sm"
              autoFocus={
                updateAlert.status !== "newer" &&
                updateAlert.status !== "same"
              }
              disabled={updateChecking}
              onClick={
                updateAlert.status === "newer" ||
                updateAlert.status === "same"
                  ? handleInstallUpdate
                  : handleDismissUpdate
              }
            >
              {updateChecking
                ? "Starting..."
                : updateAlert.status === "same"
                  ? "Force update"
                  : updateAlert.status === "newer"
                    ? "Update"
                    : "OK"}
            </Button>
          </div>
        </ConfigAlert>
      )}
    </>
  );
}
