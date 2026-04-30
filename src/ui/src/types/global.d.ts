export {};

declare global {
  interface Window {
    api: {
      closeApp: () => void;
      minimizeApp: () => void;
      maximizeApp: () => Promise<void>;
      isMaximized: () => Promise<boolean>;
    };
  }
}