import { app, BrowserWindow, ipcMain, globalShortcut } from 'electron'
import { createRequire } from 'node:module'
import { fileURLToPath } from 'node:url'
import path from 'node:path'
import { exec } from 'child_process'
import { spawn } from "child_process"
const require = createRequire(import.meta.url)
const __dirname = path.dirname(fileURLToPath(import.meta.url))

process.env.APP_ROOT = path.join(__dirname, '..')

export const VITE_DEV_SERVER_URL = process.env['VITE_DEV_SERVER_URL']
export const MAIN_DIST = path.join(process.env.APP_ROOT, 'dist-electron')
export const RENDERER_DIST = path.join(process.env.APP_ROOT, 'dist')

process.env.VITE_PUBLIC = VITE_DEV_SERVER_URL ? path.join(process.env.APP_ROOT, 'public') : RENDERER_DIST

let win: BrowserWindow | null
let captureProcess: any = null
let cs2Interval: NodeJS.Timeout | null = null
let isLaunched = false

function createWindow() {
  win = new BrowserWindow({
    width: 1280,
    height: 800,
    icon: path.join(process.env.VITE_PUBLIC, 'cs2-clipper.ico'),
    frame: false,
    titleBarStyle: 'hidden',
    webPreferences: {
      preload: path.join(__dirname, 'preload.mjs'),
    },
  })

  win.webContents.on('did-finish-load', () => {
    win?.webContents.send('main-process-message', (new Date).toLocaleString())
  })
  if (VITE_DEV_SERVER_URL) {
    win.loadURL(VITE_DEV_SERVER_URL)
  } else {
    win.loadFile(path.join(RENDERER_DIST, 'index.html'))
  }
}

ipcMain.on('app-close', () => {
  if (captureProcess) {
    captureProcess.kill("SIGTERM")
    captureProcess = null
  }
  if(cs2Interval) clearInterval(cs2Interval)

  BrowserWindow.getAllWindows().forEach(w => w.close())
  app.quit()
})
ipcMain.on('app-minimize', ()=> {
  const win = BrowserWindow.getFocusedWindow();
  win?.minimize()
})
ipcMain.handle('app-maximize', () => {
  const win = BrowserWindow.getFocusedWindow();
  if (!win) return;

  if (win.isMaximized()) {
    win.unmaximize();
  } else {
    win.maximize();
  }
});
ipcMain.handle('app-is-maximized', (event) => {
  const win = BrowserWindow.fromWebContents(event.sender);
  return win?.isMaximized();
})

cs2Interval = setInterval(() => {
  exec("tasklist", (err, stdout) => {
    if (err) return

    const found =
      stdout.toLowerCase().includes("cs2.exe")

    if (found !== isLaunched) {
      isLaunched = found
      win?.webContents.send("cs2-status", found)
    }
  })
}, 1000)

ipcMain.handle("start-capture", async () => {
  const exePath = path.join(process.env.APP_ROOT, '..', '..', 'x64', 'Debug', 'cs2-clipper.exe')
  const workDir = path.join(process.env.APP_ROOT, '..', '..')

  captureProcess = spawn(exePath, [], {
    stdio: ['pipe', 'pipe', 'pipe'],
    cwd: workDir
  })

  return true
})

ipcMain.handle("save-clip", async () => {
  if (captureProcess && captureProcess.stdin && !captureProcess.killed) {
    const cmd = JSON.stringify({
      type: "cmd",
      id: Date.now().toString(),
      cmd: "save_clip"
    })
    captureProcess.stdin.write(cmd + '\n')
  }
  return true
})

ipcMain.handle("stop-capture", async () => {
  if (captureProcess) {
    captureProcess.kill("SIGTERM")
    captureProcess = null
  }
  return true
})

app.on('activate', () => {
  if (BrowserWindow.getAllWindows().length === 0) {
    createWindow()
  }
})

app.on('before-quit', () => {
  if (captureProcess && !captureProcess.killed) {
    captureProcess.kill("SIGTERM")
  }
})

app.whenReady().then(() => {
  createWindow()
  
  globalShortcut.register('Alt+F12', async () => {
    try {
      if (!captureProcess || captureProcess.killed) {
        const exePath = path.join(process.env.APP_ROOT, '..', '..', 'x64', 'Debug', 'cs2-clipper.exe')
        const workDir = path.join(process.env.APP_ROOT, '..', '..')
        
        captureProcess = spawn(exePath, [], {
          stdio: ['pipe', 'pipe', 'pipe'],
          cwd: workDir
        })
      }
      
      if (captureProcess && captureProcess.stdin) {
        const cmd = {
          type: "cmd",
          id: Date.now().toString(),
          cmd: "save_clip"
        }
        captureProcess.stdin.write(JSON.stringify(cmd) + '\n')
      }
    } catch (_err) {
    }
  })
})
