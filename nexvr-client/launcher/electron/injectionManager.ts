import { app, shell, ipcMain } from 'electron';
import * as path from 'path';
import * as fs from 'fs';
import * as child_process from 'child_process';
import * as util from 'util';
import { InjectResult } from '../src/types';
import {
  assertTrustedIpcSender,
  canonicalExistingPath,
  gamePathsMap,
  gameExeMap,
  resolveWithinRoot,
  safeGamePath,
  validateGameId,
} from './utils';
import { detectAntiCheat } from './libraryManager';

const execFileAsync = util.promisify(child_process.execFile);
const isDev = !app.isPackaged;

export let cancelInjectionFlag = false;
export let activeTargetExeName = '';
export let activeTargetPid = 0;
export let activeGameId = '';

const injectRateLimits: Record<string, number[]> = {};
let injectionInProgress = false;
let activeLogPath = '';

async function terminatePid(pid: number, force = false): Promise<void> {
  if (!Number.isSafeInteger(pid) || pid <= 0) return;
  const args = ['/PID', String(pid)];
  if (force) args.push('/F');
  await execFileAsync('taskkill.exe', args, {
    timeout: 3000,
    windowsHide: true,
  }).catch(() => {});
}

async function getProcessPath(pid: number): Promise<string> {
  if (!Number.isSafeInteger(pid) || pid <= 0) return '';
  try {
    const command = `try { (Get-Process -Id ${pid} -ErrorAction Stop).Path } catch { (Get-CimInstance Win32_Process -Filter "ProcessId = ${pid}" -ErrorAction SilentlyContinue).ExecutablePath }`;
    const { stdout } = await execFileAsync(
      'powershell.exe',
      ['-NoProfile', '-NonInteractive', '-Command', command],
      { encoding: 'utf-8', timeout: 3000, windowsHide: true }
    );
    return stdout.trim();
  } catch {
    return '';
  }
}

function stopActiveLogWatch(): void {
  if (activeLogPath) fs.unwatchFile(activeLogPath);
  activeLogPath = '';
}

ipcMain.handle('inject:cancel', async (event) => {
  assertTrustedIpcSender(event);
  cancelInjectionFlag = true;

  const pid = activeTargetPid;
  if (pid > 0) {
    await terminatePid(pid);
    setTimeout(() => void terminatePid(pid, true), 2000);
  }
});

ipcMain.handle('inject:deploy', async (event, id: string): Promise<InjectResult> => {
  try {
    assertTrustedIpcSender(event);
    const validId = validateGameId(id);

    if (injectionInProgress) {
      return { success: false, message: 'Another injection is already in progress.' };
    }
    injectionInProgress = true;

    const now = Date.now();
    const windowMs = 15 * 60 * 1000;
    injectRateLimits[validId] = (injectRateLimits[validId] || []).filter(t => now - t < windowMs);
    if (injectRateLimits[validId].length >= 5) {
      return { success: false, message: 'Rate limit exceeded.' };
    }
    injectRateLimits[validId].push(now);

    const registeredInstallPath = gamePathsMap[validId];
    if (!registeredInstallPath) return { success: false, message: 'Game path not found' };
    const installPath = canonicalExistingPath(registeredInstallPath, 'directory');

    // Anti-Cheat Guard: Block injection into protected games to prevent multiplayer account bans
    const ac = detectAntiCheat(installPath);
    if (ac.hasAntiCheat) {
      return {
        success: false,
        message: `Injection Blocked: ${ac.antiCheatName} detected. To protect your account from multiplayer bans, VR injection is disabled on this title.`
      };
    }
    const logPath = safeGamePath(installPath, 'vrinject.log');
    try {
      fs.writeFileSync(logPath, '');
    } catch {
      // Game directory might be write-protected for non-elevated token; elevated CLI will create/append to it.
    }
    activeLogPath = logPath;
    let lastSize = 0;

    fs.watchFile(logPath, { interval: 500 }, (curr) => {
      if (curr.size < lastSize) lastSize = 0;
      if (curr.size <= lastSize) return;

      try {
        const fd = fs.openSync(logPath, 'r');
        try {
          const length = Math.min(curr.size - lastSize, 1024 * 1024);
          const buf = Buffer.alloc(length);
          const bytesRead = fs.readSync(fd, buf, 0, length, lastSize);
          lastSize += bytesRead;
          for (const line of buf.subarray(0, bytesRead).toString('utf-8').split('\n')) {
            if (line.trim()) event.sender.send('log:line', line.trim().slice(0, 2000));
          }
        } finally {
          fs.closeSync(fd);
        }
      } catch {
        // The monitored process may rotate or temporarily lock the log.
      }
    });

    const localClientBin = path.join(__dirname, '../../build/bin');
    const rootBin = path.join(__dirname, '../../../build/bin');
    const binSourceDir = isDev ? (fs.existsSync(localClientBin) ? localClientBin : rootBin) : process.resourcesPath;
    const canonicalBinSourceDir = canonicalExistingPath(binSourceDir, 'directory');
    const otaCli = path.join(app.getPath('userData'), 'updates', 'vr-inject-cli.exe');
    const cliSource = (fs.existsSync(otaCli) && fs.statSync(otaCli).size > 10000)
      ? otaCli
      : canonicalExistingPath(
          resolveWithinRoot(canonicalBinSourceDir, 'vr-inject-cli.exe'),
          'file'
        );
    const shadersSource = resolveWithinRoot(canonicalBinSourceDir, 'shaders');
    const modelsSource = resolveWithinRoot(canonicalBinSourceDir, 'models');

    if (validId.startsWith('custom_') && gameExeMap[validId]) {
      const customExe = canonicalExistingPath(gameExeMap[validId], 'file');
      resolveWithinRoot(installPath, path.relative(installPath, customExe));
      // Use child_process.exec with Windows 'start' command to properly set CWD and handle ShellExecute
      // This bypasses Node's spawn EACCES limitation when launching games that require elevation or special permissions.
      const exeCwd = path.dirname(customExe);
      child_process.exec(`start "" /d "${exeCwd}" "${customExe}"`);
    } else if (/^\d+$/.test(validId)) {
      await shell.openExternal(`steam://rungameid/${validId}`);
    } else {
      await shell.openExternal(
        `com.epicgames.launcher://apps/${encodeURIComponent(validId)}?action=launch&silent=true`
      );
    }

    let targetExeName = '';
    let targetExeDir = installPath;
    const registeredExe = gameExeMap[validId];
    if (registeredExe) {
      const customExe = canonicalExistingPath(registeredExe, 'file');
      targetExeName = path.basename(customExe).toLowerCase();
      targetExeDir = path.dirname(customExe);
    } else {
      let largestSize = 0;
      let visitedEntries = 0;

      const scan = (dir: string, depth = 0) => {
        if (depth > 8 || visitedEntries >= 20000) return;
        try {
          const entries = fs.readdirSync(dir, { withFileTypes: true });
          for (const entry of entries) {
            if (++visitedEntries >= 20000) break;
            if (entry.isSymbolicLink()) continue;

            const fullPath = resolveWithinRoot(
              installPath,
              path.relative(installPath, path.join(dir, entry.name))
            );
            try {
              if (entry.isDirectory()) {
                scan(fullPath, depth + 1);
              } else if (entry.isFile() && entry.name.toLowerCase().endsWith('.exe')) {
                const lower = entry.name.toLowerCase();
                if (
                  lower.includes('launcher') ||
                  lower.includes('crashreporter') ||
                  lower.includes('crashhandler') ||
                  lower.includes('reporter') ||
                  lower.includes('anticheat') ||
                  lower.includes('eosbootstrapper') ||
                  lower.includes('start_protected_game')
                ) {
                  continue;
                }
                const size = fs.statSync(fullPath).size;
                if (size > largestSize) {
                  largestSize = size;
                  targetExeName = lower;
                  targetExeDir = dir;
                }
              }
            } catch {
              // Ignore inaccessible game subdirectories.
            }
          }
        } catch {
          // Ignore inaccessible game subdirectories.
        }
      };
      scan(installPath);
    }

    if (!targetExeName) {
      stopActiveLogWatch();
      return { success: false, message: 'Could not determine main executable name' };
    }

    try {
      const updatesDir = path.join(app.getPath('userData'), 'updates');
      const hotfixShaders = path.join(updatesDir, 'shaders');
      const activeShadersSource = fs.existsSync(hotfixShaders) ? hotfixShaders : shadersSource;

      const targetDirs = new Set<string>([targetExeDir, installPath]);
      for (const sub of ['Phoenix/Binaries/Win64', 'Chameleon/Binaries/Win64', 'Binaries/Win64']) {
        const subDir = path.join(installPath, sub);
        if (fs.existsSync(subDir)) targetDirs.add(subDir);
      }

      for (const d of targetDirs) {
        if (fs.existsSync(activeShadersSource)) {
          try {
            fs.cpSync(activeShadersSource, path.join(d, 'shaders'), {
              recursive: true,
              force: true,
              dereference: false,
            });
          } catch (e) {}
        }
        if (fs.existsSync(modelsSource)) {
          try {
            fs.cpSync(modelsSource, path.join(d, 'models'), {
              recursive: true,
              force: true,
              dereference: false,
            });
          } catch (e) {}
        }
      }

      // Copy ONNX and DirectML DLLs to prevent target process loader lock/freeze due to missing imports
      // Also copy vrinject.dll and openxr_loader.dll so the implicit Vulkan layer can pick it up BEFORE the game starts
      // Prioritize OTA hotfixed vrinject.dll from updatesDir if available!
      const dllsToCopy = ['onnxruntime.dll', 'DirectML.dll', 'vrinject.dll', 'openxr_loader.dll'];
      for (const dll of dllsToCopy) {
        const hotfixPath = path.join(updatesDir, dll);
        const srcPath = fs.existsSync(hotfixPath)
          ? hotfixPath
          : resolveWithinRoot(canonicalBinSourceDir, dll);

        if (fs.existsSync(srcPath)) {
          for (const d of targetDirs) {
            try {
              fs.copyFileSync(srcPath, path.join(d, dll));
              console.info(`Copied ${dll} to ${d} (source: ${srcPath === hotfixPath ? 'HOTFIX' : 'BUNDLED'})`);
            } catch (e) {
              console.warn(`Could not copy ${dll} to ${d} (already present or locked): ${e}`);
            }
          }
        }
      }

      // Ensure vrinject.json configuration is synchronized across all target binary directories
      const rootConfigPath = path.join(installPath, 'vrinject.json');
      let baseProfile: Record<string, any> = {};

      const profileDirs = [
        path.join(app.getPath('userData'), 'updates', 'profiles'),
        path.resolve(__dirname, '../../../profiles'),
        path.resolve(__dirname, '../../profiles'),
        path.join(process.resourcesPath, 'profiles'),
      ];
      for (const pDir of profileDirs) {
        try {
          if (fs.existsSync(pDir)) {
            for (const f of fs.readdirSync(pDir)) {
              if (f.startsWith(`${validId}_`) || f === `${validId}.json`) {
                const parsed = JSON.parse(fs.readFileSync(path.join(pDir, f), 'utf-8'));
                baseProfile = { ...baseProfile, ...parsed };
                break;
              }
            }
          }
        } catch {}
      }

      let activeConfig: Record<string, any> = { ...baseProfile };
      if (fs.existsSync(rootConfigPath)) {
        try {
          const cur = JSON.parse(fs.readFileSync(rootConfigPath, 'utf-8'));
          // Preserve user preferences while enforcing curated profile architectural invariants
          activeConfig = {
            ...cur,
            ...(baseProfile.engine ? { engine: baseProfile.engine } : {}),
            ...(baseProfile.api ? { api: baseProfile.api } : {}),
            ...(baseProfile.reverseZ !== undefined ? { reverseZ: baseProfile.reverseZ } : {}),
            ...(baseProfile.rowMajorMatrices !== undefined ? { rowMajorMatrices: baseProfile.rowMajorMatrices } : {}),
            ...(baseProfile.matrixPrecision ? { matrixPrecision: baseProfile.matrixPrecision } : {}),
          };
        } catch {}
      }

      if (Object.keys(activeConfig).length > 0) {
        try {
          fs.writeFileSync(rootConfigPath, JSON.stringify(activeConfig, null, 2), 'utf-8');
        } catch (e) {}

        for (const d of targetDirs) {
          const destCfg = path.join(d, 'vrinject.json');
          if (destCfg !== rootConfigPath) {
            try {
              fs.writeFileSync(destCfg, JSON.stringify(activeConfig, null, 2), 'utf-8');
            } catch (e) {}
          }
        }

        try {
          const stageCfg = path.join(updatesDir, 'vrinject.json');
          fs.writeFileSync(stageCfg, JSON.stringify(activeConfig, null, 2), 'utf-8');
        } catch (e) {}
      }
    } catch (error) {
      console.warn('Non-fatal error copying shader/model assets to target directory:', error);
    }

    activeTargetExeName = targetExeName;
    activeTargetPid = 0;
    activeGameId = validId;
    cancelInjectionFlag = false;

    event.sender.send('log:line', `[Injector] Waiting up to 120s for: ${targetExeName}`);

    let targetPid = 0;
    for (let attempts = 0; attempts < 240; attempts++) {
      if (cancelInjectionFlag) {
        stopActiveLogWatch();
        return { success: false, cancelled: true, message: 'Cancelled by user.' };
      }

      await new Promise(resolve => setTimeout(resolve, 500));
      try {
        const { stdout } = await execFileAsync(
          'tasklist.exe',
          ['/fi', `IMAGENAME eq ${targetExeName}`, '/fo', 'csv', '/nh'],
          { encoding: 'utf-8', timeout: 2000, killSignal: 'SIGKILL', windowsHide: true }
        );

        const candidates: Array<{ pid: number; memory: number }> = [];
        for (const line of stdout.split('\n')) {
          if (!line.trim() || line.startsWith('INFO:')) continue;
          const parts = line.split('","');
          if (parts.length < 5) continue;
          const exeName = parts[0].replace(/"/g, '').toLowerCase();
          const pid = Number.parseInt(parts[1].replace(/"/g, ''), 10);
          const memory = Number.parseInt(
            parts[4].replace(/"/g, '').replace(/,/g, '').replace(/\s*K\s*$/i, ''),
            10
          );
          // Filter out processes with invalid PID or negligible memory (e.g. zombie processes < 5MB)
          if (exeName === targetExeName && Number.isSafeInteger(pid) && memory > 5000) {
            candidates.push({ pid, memory: Number.isFinite(memory) ? memory : 0 });
          }
        }

        // Prefer shipping binary in subfolders (e.g. Phoenix/Binaries/Win64) or processes with genuine game memory
        let selectedCandidate: { pid: number; memory: number; path: string } | null = null;
        for (const candidate of candidates) {
          const processPath = await getProcessPath(candidate.pid).catch(() => '');
          if (!processPath) continue;

          const isInsideInstall = processPath.toLowerCase().startsWith(path.resolve(installPath).toLowerCase());
          if (!isInsideInstall) continue;

          const lowerPath = processPath.toLowerCase();
          const isShippingBinary = lowerPath.includes('binaries') || lowerPath.includes('shipping');
          
          // If this is the heavy shipping binary or has > 50MB memory, select it immediately!
          if (isShippingBinary || candidate.memory > 50000) {
            selectedCandidate = { ...candidate, path: processPath };
            break;
          }

          // Otherwise keep as fallback (in case it's an indie title without subfolders)
          if (!selectedCandidate) {
            selectedCandidate = { ...candidate, path: processPath };
          }
        }

        // Resilient Fallback: If getProcessPath was blocked by Windows token/security permissions
        // (e.g. UAC / admin token restrictions on Steam titles like Sekiro), we already verified that
        // tasklist.exe found targetExeName. Select the candidate with primary game memory.
        if (!selectedCandidate && candidates.length > 0) {
          const sorted = [...candidates].sort((a, b) => b.memory - a.memory);
          const best = sorted[0];
          if (best.memory > 30000 || attempts >= 20) {
            selectedCandidate = {
              pid: best.pid,
              memory: best.memory,
              path: path.join(targetExeDir, targetExeName),
            };
          }
        }

        if (selectedCandidate) {
          // If it's a small stub (< 50MB) and not in Binaries, wait up to 15 seconds to let the real game spawn
          const lowerPath = selectedCandidate.path.toLowerCase();
          const isShippingBinary = lowerPath.includes('binaries') || lowerPath.includes('shipping');
          if (!isShippingBinary && selectedCandidate.memory < 50000 && attempts < 30) {
            // Keep waiting for the real game process to spawn!
            continue;
          }

          targetPid = selectedCandidate.pid;
          activeTargetPid = selectedCandidate.pid;
          targetExeDir = path.dirname(selectedCandidate.path);
          break;
        }
        if (targetPid > 0) break;
      } catch {
        // Process may not have started yet.
      }
    }

    if (!targetPid) {
      stopActiveLogWatch();
      return { success: false, message: 'Game executable not found or closed immediately' };
    }

    const canonicalDll = resolveWithinRoot(canonicalBinSourceDir, 'vrinject.dll');
    const otaDll = path.join(app.getPath('userData'), 'updates', 'vrinject.dll');
    const sourceDll = (fs.existsSync(otaDll) && fs.statSync(otaDll).size > 100000)
      ? otaDll
      : canonicalDll;
    let dllTarget = resolveWithinRoot(targetExeDir, 'vrinject.dll');
    
    // Check if target directory DLL is up-to-date with the source binary.
    // If the game was running and locked vrinject.dll, copying may have failed.
    // Fall back to injecting sourceDll directly so SHA-256 verification succeeds.
    try {
      if (fs.existsSync(sourceDll)) {
        if (!fs.existsSync(dllTarget) || fs.statSync(dllTarget).mtimeMs < fs.statSync(sourceDll).mtimeMs) {
          try {
            fs.copyFileSync(sourceDll, dllTarget);
          } catch {
            dllTarget = sourceDll;
          }
        }
      }
    } catch {
      dllTarget = sourceDll;
    }

    const escapePs = (str: string) => str.replace(/'/g, "''");
    const updatesDir = path.join(app.getPath('userData'), 'updates');
    const copySources = [updatesDir, canonicalBinSourceDir].filter(d => fs.existsSync(d)).join(';');
    const effectiveCopySrc = copySources || canonicalBinSourceDir;
    const innerScript =
      `$env:NEXVR_AUTH_TOKEN = '${escapePs(process.env.NEXVR_AUTH_TOKEN || '')}'; ` +
      `& '${escapePs(cliSource)}' --pid ${targetPid} --dll '${escapePs(dllTarget)}' ` +
      `--copy-src '${escapePs(effectiveCopySrc)}' --copy-dst '${escapePs(targetExeDir)}' ` +
      `*>&1 | Out-File -LiteralPath '${escapePs(logPath)}' -Append -Encoding utf8; ` +
      `if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }`;
    const base64Inner = Buffer.from(innerScript, 'utf16le').toString('base64');
    const outerScript =
      `$p = Start-Process powershell -Verb RunAs -Wait -PassThru -WindowStyle Hidden ` +
      `-ArgumentList "-NoProfile", "-NonInteractive", "-ExecutionPolicy", "Bypass", ` +
      `"-EncodedCommand", "${base64Inner}"; ` +
      `if ($p.ExitCode -ne 0) { exit $p.ExitCode }`;
    const base64Outer = Buffer.from(outerScript, 'utf16le').toString('base64');

    return await new Promise<InjectResult>((resolve) => {
      let resolved = false;
      const finish = (result: InjectResult) => {
        if (resolved) return;
        resolved = true;
        clearTimeout(timeout);
        if (!result.success) stopActiveLogWatch();
        resolve(result);
      };

      const timeout = setTimeout(() => {
        finish({
          success: false,
          message: 'Injector timed out or was blocked by Anti-Cheat. (UAC timeout?)',
        });
      }, 180000);

      child_process.execFile(
        'powershell.exe',
        ['-NoProfile', '-NonInteractive', '-ExecutionPolicy', 'Bypass', '-EncodedCommand', base64Outer],
        {
          timeout: 180000,
          killSignal: 'SIGKILL',
          cwd: installPath,
          windowsHide: true,
          env: {
            ...process.env,
            NEXVR_AUTH_TOKEN: process.env.NEXVR_AUTH_TOKEN || '',
            NEXVR_CLI: cliSource,
            NEXVR_PID: String(targetPid),
            NEXVR_DLL: dllTarget,
            NEXVR_COPY_SRC: canonicalBinSourceDir,
            NEXVR_COPY_DST: targetExeDir,
            NEXVR_LOG: logPath,
          },
        },
        (error) => {
          if (error) {
            finish({ success: false, message: `CLI failed: ${error.message}` });
          } else {
            finish({ success: true, message: 'Deployed successfully', pid: targetPid });
          }
        }
      );
    });
  } catch (error: any) {
    stopActiveLogWatch();
    return { success: false, message: error?.message || String(error) };
  } finally {
    injectionInProgress = false;
  }
});

ipcMain.handle('inject:monitor', async (event, pid: number) => {
  assertTrustedIpcSender(event);
  if (!Number.isSafeInteger(pid) || pid <= 0 || pid !== activeTargetPid) {
    throw new Error('Invalid process monitor request');
  }

  // Max 43200 iterations (approx 24 hours at 2s per check)
  for (let i = 0; i < 43200; i++) {
    try {
      const { stdout } = await execFileAsync(
        'tasklist.exe',
        ['/fi', `PID eq ${pid}`, '/fo', 'csv', '/nh'],
        { encoding: 'utf-8', timeout: 2000, killSignal: 'SIGKILL', windowsHide: true }
      );
      if (!stdout.includes(`"${pid}"`)) break;
    } catch {
      break;
    }
    await new Promise(resolve => setTimeout(resolve, 2000));
  }

  stopActiveLogWatch();
  activeTargetPid = 0;
  activeTargetExeName = '';
  activeGameId = '';
});

// 1-Click "Disable VR / Play Flat" Uninstaller
ipcMain.handle('inject:uninstall', async (event, id: string) => {
  assertTrustedIpcSender(event);
  const validId = validateGameId(id);
  const registeredInstallPath = gamePathsMap[validId];
  if (!registeredInstallPath) return { success: false, message: 'Game path not found' };
  const installPath = canonicalExistingPath(registeredInstallPath, 'directory');

  try {
    let targetExeDir = installPath;
    const registeredExe = gameExeMap[validId];
    if (registeredExe) {
      targetExeDir = path.dirname(canonicalExistingPath(registeredExe, 'file'));
    } else {
      // Locate directory where vrinject.dll was deployed
      const scanForVrinject = (dir: string, depth = 0): string | null => {
        if (depth > 6) return null;
        try {
          const entries = fs.readdirSync(dir, { withFileTypes: true });
          for (const e of entries) {
            if (e.isFile() && e.name.toLowerCase() === 'vrinject.dll') return dir;
            if (e.isDirectory() && !e.name.toLowerCase().includes('support')) {
              const res = scanForVrinject(path.join(dir, e.name), depth + 1);
              if (res) return res;
            }
          }
        } catch {}
        return null;
      };
      const foundDir = scanForVrinject(installPath);
      if (foundDir) targetExeDir = foundDir;
    }

    // Remove deployed VR injection files
    const filesToRemove = ['vrinject.dll', 'onnxruntime.dll', 'DirectML.dll', 'openxr_loader.dll', 'vrinject.log'];
    let removedCount = 0;
    for (const f of filesToRemove) {
      const p = path.join(targetExeDir, f);
      if (fs.existsSync(p)) {
        try {
          fs.unlinkSync(p);
          removedCount++;
        } catch {}
      }
    }
    const shadersDir = path.join(targetExeDir, 'shaders');
    if (fs.existsSync(shadersDir)) {
      try {
        fs.rmSync(shadersDir, { recursive: true, force: true });
        removedCount++;
      } catch {}
    }

    console.info(`[Injector] Uninstalled VR mod from: ${targetExeDir} (${removedCount} files removed)`);
    return {
      success: true,
      message: 'VR Mod uninstalled successfully. Game is now restored to original flat screen mode.'
    };
  } catch (err: any) {
    return { success: false, message: `Failed to uninstall VR mod: ${err.message}` };
  }
});

