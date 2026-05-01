export {};

declare global {
  type UiClip = {
    title: string;
    path: string;
    thumbnail?: string;
    map?: string;
    date?: string;
    duration?: string;
    kills?: number;
    type?: string;
  };

  interface Window {
    api: {
      closeApp: () => void;
      minimizeApp: () => void;
      maximizeApp: () => Promise<void>;
      isMaximized: () => Promise<boolean>;
      onCS2Status: (callback: (state: boolean) => void) => () => void;
      onClipsUpdated: (callback: () => void) => () => void;
      startCapture: () => Promise<boolean>;
      stopCapture: () => Promise<boolean>;
      saveClip: () => Promise<boolean>;
      getClips: () => Promise<UiClip[]>;
    };
  }
}