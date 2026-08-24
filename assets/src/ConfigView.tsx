import { useEffect, useRef, useState } from "react";
import {
  type ConfigData,
  type UpdateResult,
  saveSettings,
  browseFile,
  closeDialog,
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
  const [autoCheckForUpdates, setAutoCheckForUpdates] = useState(
    config.autoCheckForUpdates ?? true
  );
  const [errors, setErrors] = useState<Record<string, string>>({});
  const [updateChecking, setUpdateChecking] = useState(
    config.updateCheckPending ?? false
  );
  const [updateCancelling, setUpdateCancelling] = useState(false);
  const [updateSpeedKbps, setUpdateSpeedKbps] = useState<number | null>(null);
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
  const automaticUpdateStarted = useRef(false);

  useEffect(() => {
    const removeBrowseListener = onBrowseResult((result) => {
      if (result.path) setChromePath(result.path);
    });
    const removeResultListener = onUpdateResult((result) => {
      setUpdateChecking(false);
      setUpdateCancelling(false);
      setUpdateSpeedKbps(null);
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
      setUpdateSpeedKbps(Math.max(0, Math.round(progress.kilobytesPerSecond)));
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
    return Object.keys(newErrors).length === 0;
  };

  function handleSave() {
    if (!validate()) return;

    saveSettings({
      chromePath,
      debugPort: parseInt(debugPort, 10),
      connectAddress: connectAddress || "127.0.0.1",
      statusCheckInterval: parseInt(statusCheckInterval, 10),
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
    setUpdateAlert(null);
    setUpdateChecking(true);
    setUpdateCancelling(false);
    setUpdateSpeedKbps(null);
    checkForUpdate(false);
  }

  function handleInstallUpdate() {
    setUpdateChecking(true);
    setUpdateCancelling(false);
    setUpdateSpeedKbps(null);
    installUpdate();
  }

  function handleDismissUpdate() {
    if (updateAlert?.status === "completed") {
      dismissUpdateConfirmation();
    } else {
      dismissUpdate();
    }
    setUpdateAlert(null);
  }

  function handleIgnoreUpdateVersion() {
    if (!updateAlert?.remoteVersion) return;
    ignoreUpdateVersion(updateAlert.remoteVersion);
    setUpdateAlert(null);
  }

  return (
    <div className="p-5 flex flex-col gap-3 max-w-md mx-auto text-xs">
      <div className="space-y-1">
        <Label>Chrome Executable Path</Label>
        <div className="flex gap-1">
          <Input
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
        <Label>Debug Port</Label>
        <Input
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
        <Label>Chrome IP Address</Label>
        <Input
          value={connectAddress}
          onChange={(e) => setConnectAddress(e.target.value)}
          placeholder="127.0.0.1"
        />
      </div>

      <div className="space-y-1">
        <Label>Status Check Interval (seconds)</Label>
        <Input
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
            className="min-w-[5rem]"
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
                ? updateSpeedKbps === null
                  ? "Checking..."
                  : `Checking (${updateSpeedKbps.toLocaleString()} KB/s)...`
                : "Update"}
          </Button>
          <Button
            variant="outline"
            size="sm"
            className="min-w-[5rem]"
            onClick={() => closeDialog()}
          >
            Cancel
          </Button>
          <Button size="sm" className="min-w-[5rem]" onClick={handleSave}>
            Save
          </Button>
        </div>
      </div>

      {updateAlert && (
        <div className="fixed inset-0 z-50 flex items-center justify-center bg-black/35 p-4">
          <div
            role="alertdialog"
            aria-modal="true"
            aria-labelledby="update-alert-title"
            aria-describedby="update-alert-message"
            className="w-full max-w-sm space-y-3 rounded-lg border border-neutral-200 bg-white p-4 shadow-xl"
          >
            <div className="space-y-1">
              <h2 id="update-alert-title" className="text-sm font-semibold">
                {updateAlert.title}
              </h2>
              <p
                id="update-alert-message"
                className="text-xs leading-relaxed text-neutral-600"
              >
                {updateAlert.message}
              </p>
            </div>
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
          </div>
        </div>
      )}
    </div>
  );
}
