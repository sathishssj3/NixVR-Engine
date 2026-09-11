import { app, ipcMain } from 'electron';
import * as fs from 'fs';
import * as path from 'path';
import { VRConfig } from '../src/types';
import { assertTrustedIpcSender, gamePathsMap, gameExeMap, validateGameId, validateConfig, safeGamePath } from './utils';

const defaultVRConfig: VRConfig = {
  motionAimSensitivity: 1.0,
  useRecommendedResolution: true,
  srgbCorrection: true,
  depthSubmission: false,
  rawInputMode: true,
  autoInjectOnLaunch: true,
};

const curatedProfiles: Record<string, Partial<VRConfig>> = {
  // Hogwarts Legacy (Steam / Epic)
  '990080': {
    engine: 'UnrealEngine4',
    api: 'DX12',
    reverseZ: true,
    rowMajorMatrices: true,
    matrixPrecision: 'Float32',
    motionAimSensitivity: 1.0,
    useRecommendedResolution: true,
    srgbCorrection: true,
    depthSubmission: true,
    rawInputMode: true,
    autoInjectOnLaunch: true,
  },
  'd0614ddf466b44a2a229a43a75db9efd': {
    engine: 'UnrealEngine4',
    api: 'DX12',
    reverseZ: true,
    rowMajorMatrices: true,
    matrixPrecision: 'Float32',
    motionAimSensitivity: 1.0,
    useRecommendedResolution: true,
    srgbCorrection: true,
    depthSubmission: true,
    rawInputMode: true,
    autoInjectOnLaunch: true,
  },
  // Cyberpunk 2077
  '1091500': {
    engine: 'Generic',
    api: 'DX12',
    reverseZ: true,
    rowMajorMatrices: true,
    matrixPrecision: 'Float32',
    motionAimSensitivity: 1.2,
    useRecommendedResolution: true,
    srgbCorrection: false,
    depthSubmission: true,
    rawInputMode: true,
    autoInjectOnLaunch: true,
  },
  // Elden Ring
  '1245620': {
    engine: 'Generic',
    api: 'DX12',
    reverseZ: true,
    rowMajorMatrices: true,
    matrixPrecision: 'Float32',
    motionAimSensitivity: 0.9,
    useRecommendedResolution: true,
    srgbCorrection: true,
    depthSubmission: false,
    rawInputMode: true,
    autoInjectOnLaunch: true,
  },
  // Atomic Heart
  '668580': {
    engine: 'UnrealEngine4',
    api: 'DX12',
    reverseZ: true,
    rowMajorMatrices: true,
    matrixPrecision: 'Float32',
    motionAimSensitivity: 1.1,
    useRecommendedResolution: true,
    srgbCorrection: true,
    depthSubmission: true,
    rawInputMode: true,
    autoInjectOnLaunch: true,
  },
  // Mortal Shell
  '1110910': {
    engine: 'UnrealEngine4',
    api: 'DX11',
    reverseZ: true,
    rowMajorMatrices: true,
    matrixPrecision: 'Float32',
    motionAimSensitivity: 1.0,
    useRecommendedResolution: true,
    srgbCorrection: true,
    depthSubmission: false,
    rawInputMode: true,
    autoInjectOnLaunch: true,
  },
  // Palworld
  '1623730': {
    engine: 'UnrealEngine5',
    api: 'DX12',
    reverseZ: true,
    rowMajorMatrices: true,
    matrixPrecision: 'Float32',
    motionAimSensitivity: 1.0,
    useRecommendedResolution: true,
    srgbCorrection: true,
    depthSubmission: true,
    rawInputMode: true,
    autoInjectOnLaunch: true,
  },
  // Sekiro: Shadows Die Twice
  '814380': {
    engine: 'Generic',
    api: 'DX11',
    reverseZ: false,
    rowMajorMatrices: false,
    matrixPrecision: 'Float32',
    motionAimSensitivity: 1.0,
    useRecommendedResolution: true,
    srgbCorrection: false,
    depthSubmission: false,
    rawInputMode: true,
    autoInjectOnLaunch: true,
  },
};

function loadProfilesFromDisk(): Record<string, Partial<VRConfig>> {
  const profiles: Record<string, Partial<VRConfig>> = { ...curatedProfiles };
  try {
    let userUpdatesProfiles = '';
    try {
      userUpdatesProfiles = path.join(app.getPath('userData'), 'updates', 'profiles');
    } catch {}

    // Check root profiles directory (development, packaged resources, and OTA updates)
    const candidateDirs = [
      ...(userUpdatesProfiles ? [userUpdatesProfiles] : []),
      path.resolve(__dirname, '../../../profiles'),
      path.resolve(__dirname, '../../profiles'),
      path.join(process.resourcesPath, 'profiles'),
    ];
    for (const pDir of candidateDirs) {
      if (fs.existsSync(pDir)) {
        const files = fs.readdirSync(pDir);
        for (const f of files) {
          if (f.endsWith('.json') && f !== 'schema.json') {
            try {
              const data = JSON.parse(fs.readFileSync(path.join(pDir, f), 'utf-8'));
              if (data && typeof data.id === 'string') {
                profiles[data.id] = {
                  motionAimSensitivity: typeof data.motionAimSensitivity === 'number' ? data.motionAimSensitivity : 1.0,
                  useRecommendedResolution: data.useRecommendedResolution !== false,
                  srgbCorrection: data.srgbCorrection !== false,
                  depthSubmission: Boolean(data.depthSubmission),
                  rawInputMode: data.rawInputMode !== false,
                  autoInjectOnLaunch: data.autoInjectOnLaunch !== false,
                  engine: typeof data.engine === 'string' ? data.engine : undefined,
                  api: typeof data.api === 'string' ? data.api : undefined,
                  reverseZ: typeof data.reverseZ === 'boolean' ? data.reverseZ : undefined,
                  rowMajorMatrices: typeof data.rowMajorMatrices === 'boolean' ? data.rowMajorMatrices : undefined,
                  matrixPrecision: typeof data.matrixPrecision === 'string' ? data.matrixPrecision : undefined,
                };
              }
            } catch {}
          }
        }
      }
    }
  } catch {}
  return profiles;
}

const activeProfiles = loadProfilesFromDisk();

ipcMain.handle('config:read', async (event, id: string): Promise<VRConfig> => {
  try {
    assertTrustedIpcSender(event);
    const validId = validateGameId(id);
    const installPath = gamePathsMap[validId];
    const registeredExe = gameExeMap[validId];
    let matchedProfile = activeProfiles[validId];
    if (!matchedProfile && validId.startsWith('custom_')) {
      const exeName = registeredExe ? path.basename(registeredExe, '.exe').toLowerCase() : '';
      const dirName = installPath ? path.basename(installPath).toLowerCase() : '';
      if (exeName.includes('sekiro') || dirName.includes('sekiro')) {
        matchedProfile = activeProfiles['814380'];
      } else {
        for (const [pId, pCfg] of Object.entries(activeProfiles)) {
          if (exeName && (pId.includes(exeName) || (pCfg as any).name?.toLowerCase().includes(exeName))) {
            matchedProfile = pCfg;
            break;
          }
        }
      }
    }
    const initialConfig = { ...defaultVRConfig, ...(matchedProfile || {}) };
    if (!installPath) return initialConfig;

    const targetDir = registeredExe ? path.dirname(registeredExe) : undefined;
    // Check target binary directory first, then root install directory
    const candidatePaths: string[] = [];
    if (targetDir) candidatePaths.push(path.join(targetDir, 'vrinject.json'));
    candidatePaths.push(safeGamePath(installPath, 'vrinject.json'));

    for (const cfgPath of candidatePaths) {
      if (fs.existsSync(cfgPath)) {
        const content = fs.readFileSync(cfgPath, 'utf-8');
        const parsed = JSON.parse(content);
        return validateConfig({ ...initialConfig, ...parsed });
      }
    }
    return initialConfig;
  } catch (e) {
    console.error('config:read error', e);
    return defaultVRConfig;
  }
});

ipcMain.handle('config:write', async (event, id: string, cfg: unknown) => {
  try {
    assertTrustedIpcSender(event);
    const cfgString = JSON.stringify(cfg);
    if (cfgString.length > 10240) throw new Error('Payload too large (max 10KB)');
    
    const validId = validateGameId(id);
    const validCfg = validateConfig(cfg);
    const installPath = gamePathsMap[validId];
    if (!installPath) return { success: false, error: 'Game path not found' };

    const registeredExe = gameExeMap[validId];
    const targetDir = registeredExe ? path.dirname(registeredExe) : undefined;

    // Synchronize config to both installPath and target shipping binary directory
    const dirsToSync = new Set<string>([installPath]);
    if (targetDir && fs.existsSync(targetDir)) dirsToSync.add(targetDir);
    for (const sub of ['Phoenix/Binaries/Win64', 'Chameleon/Binaries/Win64', 'Binaries/Win64']) {
      const subDir = path.join(installPath, sub);
      if (fs.existsSync(subDir)) dirsToSync.add(subDir);
    }

    const baseProfile = activeProfiles[validId] || {};
    const finalCfg: VRConfig = {
      ...validCfg,
      engine: validCfg.engine ?? baseProfile.engine,
      api: validCfg.api ?? baseProfile.api,
      reverseZ: validCfg.reverseZ ?? baseProfile.reverseZ,
      rowMajorMatrices: validCfg.rowMajorMatrices ?? baseProfile.rowMajorMatrices,
      matrixPrecision: validCfg.matrixPrecision ?? baseProfile.matrixPrecision,
    };

    for (const dir of dirsToSync) {
      const cfgPath = path.join(dir, 'vrinject.json');
      const tempPath = path.join(dir, `vrinject.${process.pid}.tmp`);
      fs.writeFileSync(tempPath, JSON.stringify(finalCfg, null, 2));
      fs.renameSync(tempPath, cfgPath);
      try {
        fs.chmodSync(cfgPath, 0o600); // S6.3: Owner rw only
      } catch {}
    }
    return { success: true };
  } catch (e: any) {
    return { success: false, error: e.message };
  }
});
