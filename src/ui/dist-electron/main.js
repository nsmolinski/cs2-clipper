import { ipcMain, BrowserWindow, app, globalShortcut } from "electron";
import { fileURLToPath } from "node:url";
import path from "node:path";
import { exec, spawn } from "child_process";
import fs from "fs";
const __dirname$1 = path.dirname(fileURLToPath(import.meta.url));
process.env.APP_ROOT = path.join(__dirname$1, "..");
const VITE_DEV_SERVER_URL = process.env["VITE_DEV_SERVER_URL"];
const MAIN_DIST = path.join(process.env.APP_ROOT, "dist-electron");
const RENDERER_DIST = path.join(process.env.APP_ROOT, "dist");
process.env.VITE_PUBLIC = VITE_DEV_SERVER_URL ? path.join(process.env.APP_ROOT, "public") : RENDERER_DIST;
let win;
let captureProcess = null;
let cs2Interval = null;
let isLaunched = false;
let clipsWatcher = null;
const clipsPath = "C:/CS2Recordings";
function setupClipsWatcher() {
  if (clipsWatcher) {
    clipsWatcher.close();
  }
  if (fs.existsSync(clipsPath)) {
    clipsWatcher = fs.watch(clipsPath, (_eventType, filename) => {
      if (filename && (filename.endsWith(".mp4") || filename.endsWith(".mkv"))) {
        win == null ? void 0 : win.webContents.send("clips-updated");
      }
    });
  }
}
function createWindow() {
  win = new BrowserWindow({
    width: 1280,
    height: 800,
    icon: path.join(process.env.VITE_PUBLIC, "cs2-clipper.ico"),
    frame: false,
    titleBarStyle: "hidden",
    webPreferences: {
      preload: path.join(__dirname$1, "preload.mjs"),
      webSecurity: !VITE_DEV_SERVER_URL
    }
  });
  win.webContents.on("did-finish-load", () => {
    win == null ? void 0 : win.webContents.send("main-process-message", (/* @__PURE__ */ new Date()).toLocaleString());
  });
  if (VITE_DEV_SERVER_URL) {
    win.loadURL(VITE_DEV_SERVER_URL);
  } else {
    win.loadFile(path.join(RENDERER_DIST, "index.html"));
  }
}
ipcMain.on("app-close", () => {
  if (captureProcess) {
    captureProcess.kill("SIGTERM");
    captureProcess = null;
  }
  if (cs2Interval) clearInterval(cs2Interval);
  BrowserWindow.getAllWindows().forEach((w) => w.close());
  app.quit();
});
ipcMain.on("app-minimize", () => {
  const win2 = BrowserWindow.getFocusedWindow();
  win2 == null ? void 0 : win2.minimize();
});
ipcMain.handle("app-maximize", () => {
  const win2 = BrowserWindow.getFocusedWindow();
  if (!win2) return;
  if (win2.isMaximized()) {
    win2.unmaximize();
  } else {
    win2.maximize();
  }
});
ipcMain.handle("app-is-maximized", (event) => {
  const win2 = BrowserWindow.fromWebContents(event.sender);
  return win2 == null ? void 0 : win2.isMaximized();
});
cs2Interval = setInterval(() => {
  exec("tasklist", (err, stdout) => {
    if (err) return;
    const found = stdout.toLowerCase().includes("cs2.exe");
    if (found !== isLaunched) {
      isLaunched = found;
      win == null ? void 0 : win.webContents.send("cs2-status", found);
    }
  });
}, 1e3);
ipcMain.handle("start-capture", async () => {
  const exePath = path.join(process.env.APP_ROOT, "..", "..", "x64", "Debug", "cs2-clipper.exe");
  const workDir = path.join(process.env.APP_ROOT, "..", "..");
  captureProcess = spawn(exePath, [], {
    stdio: ["pipe", "pipe", "pipe"],
    cwd: workDir
  });
  return true;
});
ipcMain.handle("save-clip", async () => {
  if (captureProcess && captureProcess.stdin && !captureProcess.killed) {
    const cmd = JSON.stringify({
      type: "cmd",
      id: Date.now().toString(),
      cmd: "save_clip"
    });
    captureProcess.stdin.write(cmd + "\n");
  }
  return true;
});
ipcMain.handle("stop-capture", async () => {
  if (captureProcess) {
    captureProcess.kill("SIGTERM");
    captureProcess = null;
  }
  return true;
});
ipcMain.handle("get-clips", async () => {
  try {
    const files = await fs.promises.readdir(clipsPath);
    return files.filter((f) => (f.endsWith(".mp4") || f.endsWith(".mkv")) && f.startsWith("cs2_recording")).map((f) => ({
      title: path.parse(f).name,
      path: path.join(clipsPath, f)
    }));
  } catch (_error) {
    return [];
  }
});
app.on("activate", () => {
  if (BrowserWindow.getAllWindows().length === 0) {
    createWindow();
  }
});
app.on("before-quit", () => {
  if (captureProcess && !captureProcess.killed) {
    captureProcess.kill("SIGTERM");
  }
});
app.whenReady().then(() => {
  createWindow();
  setupClipsWatcher();
  globalShortcut.register("Alt+F12", async () => {
    try {
      if (!captureProcess || captureProcess.killed) {
        const exePath = path.join(process.env.APP_ROOT, "..", "..", "x64", "Debug", "cs2-clipper.exe");
        const workDir = path.join(process.env.APP_ROOT, "..", "..");
        captureProcess = spawn(exePath, [], {
          stdio: ["pipe", "pipe", "pipe"],
          cwd: workDir
        });
      }
      if (captureProcess && captureProcess.stdin) {
        const cmd = {
          type: "cmd",
          id: Date.now().toString(),
          cmd: "save_clip"
        };
        captureProcess.stdin.write(JSON.stringify(cmd) + "\n");
      }
    } catch (_err) {
    }
  });
});
export {
  MAIN_DIST,
  RENDERER_DIST,
  VITE_DEV_SERVER_URL
};
