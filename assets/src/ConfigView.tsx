import { useState, useEffect } from "react";
import {
  type ConfigData,
  saveSettings,
  browseFile,
  closeDialog,
  onBrowseResult,
} from "./lib/bridge";
import { Button } from "./components/ui/button";
import { Input } from "./components/ui/input";
import { Label } from "./components/ui/label";

interface Props {
  config: ConfigData;
}

export default function ConfigView({ config }: Props) {
  const [chromePath, setChromePath] = useState(config.chromePath);
  const [debugPort, setDebugPort] = useState(String(config.debugPort));
  const [connectAddress, setConnectAddress] = useState(config.connectAddress);
  const [statusCheckInterval, setStatusCheckInterval] = useState(
    String(config.statusCheckInterval)
  );

  const [errors, setErrors] = useState<Record<string, string>>({});

  useEffect(() => {
    onBrowseResult((result) => {
      if (result.path) {
        setChromePath(result.path);
      }
    });
  }, []);

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

  const handleSave = () => {
    if (!validate()) return;

    saveSettings({
      chromePath,
      debugPort: parseInt(debugPort, 10),
      connectAddress: connectAddress || "127.0.0.1",
      statusCheckInterval: parseInt(statusCheckInterval, 10),
    });
  };

  return (
    <div className="p-4 space-y-3">
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

      <div className="flex justify-end gap-2 pt-2">
        <Button variant="outline" className="w-20" onClick={() => closeDialog()}>
          Cancel
        </Button>
        <Button className="w-20" onClick={handleSave}>Save</Button>
      </div>
    </div>
  );
}
