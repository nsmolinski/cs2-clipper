export {};

declare global {
  interface Window {
    api: {
      closeApp: () => void;
      minimizeApp: () => void;
      maximizeApp: () => Promise<void>;
      isMaximized: () => Promise<boolean>;
      onCS2Status: (callback: (state: boolean) => void) => void;
      startCapture: () => Promise<boolean>;
      stopCapture: () => Promise<boolean>;
      saveClip: () => Promise<boolean>;
    };
  }
}