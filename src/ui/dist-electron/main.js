import { ipcMain, app, BrowserWindow } from "electron";
import { createRequire } from "node:module";
import { fileURLToPath } from "node:url";
import path from "node:path";
createRequire(import.meta.url);
const __dirname$1 = path.dirname(fileURLToPath(import.meta.url));
process.env.APP_ROOT = path.join(__dirname$1, "..");
const VITE_DEV_SERVER_URL = process.env["VITE_DEV_SERVER_URL"];
const MAIN_DIST = path.join(process.env.APP_ROOT, "dist-electron");
const RENDERER_DIST = path.join(process.env.APP_ROOT, "dist");
process.env.VITE_PUBLIC = VITE_DEV_SERVER_URL ? path.join(process.env.APP_ROOT, "public") : RENDERER_DIST;
let win;
function createWindow() {
  win = new BrowserWindow({
    icon: path.join(process.env.VITE_PUBLIC, "cs2-clipper.ico"),
    frame: false,
    titleBarStyle: "hidden",
    webPreferences: {
      preload: path.join(__dirname$1, "preload.mjs")
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
app.on("activate", () => {
  if (BrowserWindow.getAllWindows().length === 0) {
    createWindow();
  }
});
app.whenReady().then(createWindow);
export {
  MAIN_DIST,
  RENDERER_DIST,
  VITE_DEV_SERVER_URL
};
