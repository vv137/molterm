import { execFile, exec } from "node:child_process";
import { readFile, copyFile, mkdtemp, rm } from "node:fs/promises";
import { tmpdir } from "node:os";
import { join } from "node:path";
import { promisify } from "node:util";

const execAsync = promisify(exec);

const DEFAULT_MOLTERM =
  process.env.MOLTERM_BIN || join(__dirname, "..", "..", "build", "molterm");

/** Root of the molterm git repo (two levels up from dist/). */
export const REPO_ROOT = join(__dirname, "..", "..");

/** Run a shell command in the repo root, returning stdout+stderr. */
export async function runShell(
  cmd: string,
  opts?: { cwd?: string; timeout?: number }
): Promise<MoltermResult> {
  const cwd = opts?.cwd ?? REPO_ROOT;
  const timeout = opts?.timeout ?? 120_000;
  try {
    const { stdout, stderr } = await execAsync(cmd, { cwd, timeout });
    return { ok: true, stdout: stdout.trimEnd(), stderr: stderr.trimEnd() };
  } catch (e: unknown) {
    const err = e as { stdout?: string; stderr?: string; message?: string };
    return {
      ok: false,
      stdout: (err.stdout || "").trimEnd(),
      stderr: (err.stderr || err.message || "").trimEnd(),
    };
  }
}

export interface MoltermResult {
  ok: boolean;
  stdout: string;
  stderr: string;
}

/**
 * Run a sequence of molterm commands in headless (--no-tui) mode.
 * Returns stdout/stderr from the process.
 */
export function runMolterm(
  commands: string[],
  opts?: { cwd?: string; timeout?: number }
): Promise<MoltermResult> {
  const script = commands.join("\n") + "\n";
  const timeout = opts?.timeout ?? 30_000;
  const cwd = opts?.cwd ?? process.cwd();

  return new Promise((resolve) => {
    const child = execFile(
      DEFAULT_MOLTERM,
      ["--script", "-", "--no-tui"],
      { cwd, timeout, maxBuffer: 10 * 1024 * 1024 },
      (error, stdout, stderr) => {
        resolve({
          ok: !error,
          stdout: stdout.trimEnd(),
          stderr: stderr.trimEnd(),
        });
      }
    );
    child.stdin!.write(script);
    child.stdin!.end();
  });
}

/**
 * Run commands and capture a screenshot. Returns the PNG as a Buffer.
 */
export async function runWithScreenshot(
  commands: string[],
  width = 800,
  height = 600,
  dpi = 150,
  savePath?: string
): Promise<{ result: MoltermResult; image: Buffer | null; path: string }> {
  const dir = await mkdtemp(join(tmpdir(), "molterm-mcp-"));
  const imgPath = join(dir, "output.png");

  const allCmds = [
    ...commands,
    `screenshot ${imgPath} ${width} ${height} ${dpi}`,
  ];

  const result = await runMolterm(allCmds, { cwd: dir });

  let image: Buffer | null = null;
  try {
    image = await readFile(imgPath);
    if (savePath) await copyFile(imgPath, savePath);
  } catch {
    // screenshot may have failed
  }

  const outPath = savePath || imgPath;

  // Clean up temp dir in background
  rm(dir, { recursive: true, force: true }).catch(() => {});

  return { result, image, path: outPath };
}
