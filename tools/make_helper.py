"""Steps behind the Makefile, so it needs no particular shell (cmd, PowerShell or Git Bash).

usage: python tools/make_helper.py <step>
  help       list the make targets
  deps       install what is missing (Git, Visual Studio Build Tools with C++) with
             winget, then clone PS2Recomp and PCSX2 at the pinned commits
  check      report every dependency; exit 1 if something required is missing
  build      configure if needed, then build WarriorsOrochi.exe and WarriorsOrochi-Setup.exe
  install    build, then refresh the player folder "Warriors Orochi" (keeps saves and settings)
  reinstall  after editing the code: update patches and build configuration, compile only
             what changed, replace the player folder's programs (keeps saves and settings)
  package    one zip with the program, the source and the logs, next to this folder
  run        open the Setup from the player folder
"""
from pathlib import Path
import getpass, glob, os, re, shutil, subprocess, sys, zipfile

PORT = Path(__file__).resolve().parents[1]
VENDOR = PORT / 'vendor/PS2Recomp'
PCSX2 = PORT / 'thirdparty/pcsx2'
BUILD = PORT / 'build-tools'
RELEASE = BUILD / 'ps2xRuntime/Release'
PLAYER = PORT / 'Warriors Orochi'
ISO = PORT.parent / 'Warriors Orochi (USA).iso'
PACKAGE = PORT.parent / 'Warriors Orochi Port - pacote completo.zip'
PS2RECOMP = ('https://github.com/ran-j/PS2Recomp.git', 'eb8688b2fd191129f103a39543002c005b33409c')
PCSX2_PIN = ('https://github.com/PCSX2/pcsx2.git', 'fd9d310ccbb6b8b62c976da8886a3c8fd3a10ff3')
PROGRAMS = ('WarriorsOrochi.exe', 'WarriorsOrochi-Setup.exe')
VSWHERE = Path(os.environ.get('ProgramFiles(x86)', r'C:\Program Files (x86)')) / 'Microsoft Visual Studio/Installer/vswhere.exe'

MAKE_HELP = """make targets:
  make deps       install what is missing (Git, Visual Studio Build Tools with C++) and download
                  PS2Recomp and PCSX2 at the versions the port uses
  make check      check that every dependency is present
  make generate   read the ISO and generate the game code (needs the ISO next to this folder)
  make build      compile the game and the Setup
  make install    build + refresh the "Warriors Orochi" player folder (keeps saves and settings)
  make reinstall  after editing the code (also new .cpp files, patches, VU1 config): update the
                  build configuration, compile only what changed and replace the game's
                  programs (keeps saves and settings; the build is reused, not redone)
  make test       run the regression tests
  make package    one zip with the program, the source and the logs, to send to someone
  make run        open the Setup
  make all        deps, check, generate, install and test"""

sys.path.insert(0, str(PORT / 'tools'))


def say(text):
    print(f'[make] {text}', flush=True)


def run(args, **kwargs):
    say(' '.join(str(a) for a in args))
    subprocess.run([str(a) for a in args], check=True, **kwargs)


def output(args):
    try:
        return subprocess.run([str(a) for a in args], capture_output=True, text=True).stdout.strip()
    except OSError:
        return ''


# ---------------------------------------------------------------- detection

def visual_studio():
    """Install path of a Visual Studio 2022 with the x64 C++ compiler, or ''."""
    if not VSWHERE.exists():
        return ''
    return output([VSWHERE, '-latest', '-products', '*', '-version', '[17.0,18.0)',
                   '-requires', 'Microsoft.VisualStudio.Component.VC.Tools.x86.x64', '-property', 'installationPath'])


def cmake_path():
    found = shutil.which('cmake')
    if found:
        return found
    vs = visual_studio()
    candidate = Path(vs) / 'Common7/IDE/CommonExtensions/Microsoft/CMake/CMake/bin/cmake.exe' if vs else None
    return str(candidate) if candidate and candidate.exists() else ''


def windows_sdk():
    roots = glob.glob(r'C:\Program Files (x86)\Windows Kits\10\Include\10.*')
    return sorted(roots)[-1].rsplit('\\', 1)[-1] if roots else ''


def head(repo):
    return output(['git', '-C', repo, 'rev-parse', 'HEAD'])


def pcsx2_patches_applied():
    patches = sorted((PORT / 'patches/pcsx2').glob('*.patch'))
    return all(subprocess.run(['git', '-C', str(PCSX2), 'apply', '--check', '-R', str(p)], capture_output=True).returncode == 0
               for p in patches), len(patches)


# The build's deepest file (a toml11 test inside build-tools/_deps) is about 185
# characters below this folder; Windows stops at 260, so git fails to check it out
# and the configure step fails at "Build step for toml11".
MAX_FOLDER_LENGTH = 60


def short_path_problem():
    length = len(str(PORT))
    if length <= MAX_FOLDER_LENGTH:
        return ''
    return (f'this folder path has {length} characters, the build needs at most {MAX_FOLDER_LENGTH}: '
            r'move the folder to a short path such as C:\WO (the ISO goes next to it, e.g. C:\Warriors Orochi (USA).iso)')


def pathcheck():
    problem = short_path_problem()
    if problem:
        sys.exit(f'[make] {problem}')


def checks():
    """(name, ok, detail, required) for every dependency."""
    rows = []
    problem = short_path_problem()
    rows.append(('Short folder path', not problem, problem or f'{len(str(PORT))} characters', True))
    v = sys.version_info
    rows.append(('Python 3.12+', v >= (3, 12), f'{v.major}.{v.minor}.{v.micro}', True))
    git = output(['git', '--version'])
    rows.append(('Git', bool(git), git or 'not found: make deps', True))
    vs = visual_studio()
    rows.append(('Visual Studio 2022 C++', bool(vs), vs or 'not found: make deps', True))
    cm = cmake_path()
    rows.append(('CMake', bool(cm), output([cm, '--version']).splitlines()[0] if cm else 'not found (comes with Visual Studio)', True))
    sdk = windows_sdk()
    rows.append(('Windows 10/11 SDK', bool(sdk), sdk or 'not found (comes with the Visual Studio C++ tools)', True))
    ps2 = head(VENDOR) if (VENDOR / '.git').exists() else ''
    rows.append(('PS2Recomp', ps2 == PS2RECOMP[1], ps2[:12] + (' (pinned)' if ps2 == PS2RECOMP[1] else ' (other commit)') if ps2 else 'missing: make deps', True))
    pc = head(PCSX2) if (PCSX2 / '.git').exists() else ''
    rows.append(('PCSX2', pc == PCSX2_PIN[1], pc[:12] + (' (pinned)' if pc == PCSX2_PIN[1] else ' (other commit)') if pc else 'missing: make deps', True))
    if pc:
        ok, count = pcsx2_patches_applied()
        rows.append(('PCSX2 port patches', ok, f'{count} applied' if ok else 'not applied: make deps', True))
    rows.append(('Game ISO', ISO.exists(), f'{ISO.name} next to this folder' if ISO.exists() else f'put "{ISO.name}" next to this folder', True))
    rows.append(('Generated game code', (PORT / 'generated').exists(), 'present' if (PORT / 'generated').exists() else 'missing: make generate', False))
    configured = (BUILD / 'ps2xRuntime/ps2EntryRunner.vcxproj').exists()
    rows.append(('Game build configured', configured, 'yes' if configured else 'not yet: make install configures it', False))
    rows.append(('Player folder', (PLAYER / PROGRAMS[0]).exists(), 'yes' if (PLAYER / PROGRAMS[0]).exists() else 'not yet: make install', False))
    return rows


# ---------------------------------------------------------------- steps

def check():
    rows = checks()
    width = max(len(r[0]) for r in rows)
    for name, ok, detail, required in rows:
        mark = 'OK' if ok else ('FALTA' if required else 'ainda nao')
        print(f'  {mark:9s} {name:{width}s}  {detail}')
    missing = [r[0] for r in rows if r[3] and not r[1]]
    if missing:
        sys.exit(f'[make] missing: {", ".join(missing)}')
    say('all required dependencies are present')


def winget_install(package, *extra):
    if not shutil.which('winget'):
        sys.exit(f'[make] winget is not available; install {package} by hand')
    run(['winget', 'install', '--id', package, '-e', '--accept-source-agreements', '--accept-package-agreements', *extra])


def clone(path, url, commit):
    if (path / '.git').exists():
        if head(path) != commit:
            run(['git', '-C', path, 'fetch', 'origin'])
            run(['git', '-C', path, 'checkout', commit])
        else:
            say(f'{path.relative_to(PORT)} already at {commit[:12]}')
        return
    path.parent.mkdir(parents=True, exist_ok=True)
    run(['git', 'clone', url, path])
    run(['git', '-C', path, 'checkout', commit])


def deps():
    if not output(['git', '--version']):
        winget_install('Git.Git')
        os.environ['PATH'] += os.pathsep + r'C:\Program Files\Git\cmd'
    if not visual_studio():
        say('installing Visual Studio 2022 Build Tools with C++ (large download; Windows may ask for permission)')
        winget_install('Microsoft.VisualStudio.2022.BuildTools', '--override',
                       '--quiet --wait --norestart --add Microsoft.VisualStudio.Workload.VCTools --includeRecommended')
    clone(VENDOR, *PS2RECOMP)
    clone(PCSX2, *PCSX2_PIN)
    for patch in sorted((PORT / 'patches/pcsx2').glob('*.patch')):
        applied = subprocess.run(['git', '-C', str(PCSX2), 'apply', '--check', '-R', str(patch)], capture_output=True).returncode == 0
        if not applied:
            run(['git', '-C', PCSX2, 'apply', patch])
    say('dependencies ready. FFmpeg, raylib and the other libraries are downloaded by the build itself, '
        'which also applies the PS2Recomp patches.')


def build():
    pathcheck()
    if not (PORT / 'generated').exists():
        sys.exit('[make] no generated game code yet: run "make generate" first (needs the ISO next to this folder)')
    # "make generate" leaves a tools-only configuration (no game project); the
    # game build needs the native one, configured once.
    if not (BUILD / 'ps2xRuntime/ps2EntryRunner.vcxproj').exists():
        run([sys.executable, PORT / 'tools/workflow.py', 'configure-native'], cwd=PORT)
    run([cmake_path() or 'cmake', '--build', BUILD, '--config', 'Release', '--target', 'ps2EntryRunner', 'orochi_setup', '--parallel'])
    for name in PROGRAMS:
        if not (RELEASE / name).exists():
            sys.exit(f'[make] {name} was not built')
    say(f'built: {RELEASE.relative_to(PORT)}')


def runtime_dlls():
    # The Visual C++ runtime next to the programs, so another PC needs no installer.
    vs = visual_studio()
    roots = glob.glob(os.path.join(vs, r'VC\Redist\MSVC\*\x64\Microsoft.VC143.CRT')) if vs else []
    if not roots:
        say('Visual C++ runtime DLLs not found; the player folder keeps the ones it has')
        return []
    root = Path(sorted(roots)[-1])
    return [root / name for name in ('msvcp140.dll', 'vcruntime140.dll', 'vcruntime140_1.dll') if (root / name).exists()]


def built_programs():
    return [item for item in RELEASE.iterdir() if item.suffix.lower() in ('.exe', '.dll') and item.name != 'orochi_ipu_check.exe']


def install(build_first=True):
    if build_first:
        build()
    PLAYER.mkdir(exist_ok=True)
    copied = 0
    for item in built_programs():
        shutil.copy2(item, PLAYER / item.name); copied += 1
    if (RELEASE / 'pcsx2-resources').exists():
        shutil.copytree(RELEASE / 'pcsx2-resources', PLAYER / 'pcsx2-resources', dirs_exist_ok=True)
    for dll in runtime_dlls():
        shutil.copy2(dll, PLAYER / dll.name); copied += 1
    shutil.copy2(PORT / 'assets/player/LEIA-ME.txt', PLAYER / 'LEIA-ME.txt')
    for name in ('LICENSE', 'NOTICES.md'):
        shutil.copy2(PORT / name, PLAYER / name)
    say(f'player folder ready ({copied} programs and libraries): {PLAYER}')


# The player folder: what "make reinstall" keeps (settings, saves and screenshots in
# data/, a disc image someone put there); the rest are programs it replaces.
KEEP_IN_PLAYER = {'data', 'orochi-settings.ini'}


def remove(path):
    """Delete a file or folder tree, read-only files included (git objects)."""
    def writable(function, name, *_):
        os.chmod(name, 0o666)
        function(name)
    if path.is_dir() and not path.is_symlink():
        shutil.rmtree(path, onexc=writable)
    elif path.exists() or path.is_symlink():
        os.chmod(path, 0o666)
        path.unlink()


def programs_running():
    listing = output(['tasklist', '/fo', 'csv', '/nh'])
    return sorted({name for name in PROGRAMS if f'"{name.lower()}"' in listing.lower()})


def refresh_pcsx2_patches():
    """Put PCSX2 back to the pinned commit plus this source's patches (they may have changed)."""
    patches = sorted((PORT / 'patches/pcsx2').glob('*.patch'))
    if pcsx2_patches_applied()[0]:
        say('PCSX2 already matches the port patches')
        return
    files = sorted({name for p in patches for name in
                    re.findall(r'^\+\+\+ b/(.+)$', p.read_text(encoding='utf8'), flags=re.M)})
    run(['git', '-C', PCSX2, 'checkout', PCSX2_PIN[1], '--', *files])
    for patch in patches:
        run(['git', '-C', PCSX2, 'apply', patch])


def reinstall():
    """Incremental reinstall after editing the code: bring the patches and the build
    configuration up to date (new .cpp files, VU1 translations, game patches), compile
    only what changed, then replace the player folder's programs with the new ones.
    Nothing else is deleted: the build, the generated game code, saves and settings stay."""
    pathcheck()
    if not (PORT / 'generated').exists():
        sys.exit('[make] no generated game code yet: run "make generate" first (needs the ISO next to this folder)')
    running = programs_running()
    if running:
        sys.exit(f'[make] close {" and ".join(running)} first')
    refresh_pcsx2_patches()
    # Re-run CMake on the existing build: it picks up new or removed source files and
    # the PS2Recomp patches; unchanged files keep their compiled objects.
    run([sys.executable, PORT / 'tools/workflow.py', 'configure-native'], cwd=PORT)
    build()
    old = [p for p in sorted(PLAYER.iterdir())
           if p.name.lower() not in KEEP_IN_PLAYER and p.suffix.lower() != '.iso'] if PLAYER.exists() else []
    for item in old:
        remove(item)
    say(f'replaced {len(old)} old program files in the player folder (data/ and settings kept)')
    install(build_first=False)


def personal_markers():
    user = getpass.getuser().lower()
    home = str(Path.home()).lower()
    return [m.encode() for m in {user, home, home.replace('\\', '/')} if len(m) >= 4]


def package():
    if not (PLAYER / PROGRAMS[0]).exists():
        sys.exit('[make] no player folder yet: run "make install"')
    markers = personal_markers()
    top = 'warriors-orochi-port'
    skipped, count = [], 0
    temporary = PACKAGE.with_name(PACKAGE.name + '.tmp')
    with zipfile.ZipFile(temporary, 'w', zipfile.ZIP_DEFLATED, compresslevel=9) as z:
        def add(path, name):
            nonlocal count
            data = path.read_bytes()
            if any(m in data.lower() for m in markers):
                skipped.append(name)
                return
            z.writestr(name, data)
            count += 1

        add(PORT / 'assets/package/LEIA-ME-PRIMEIRO.txt', f'{top}/LEIA-ME-PRIMEIRO.txt')
        # The program: no saves, settings, logs or files read from the disc (data/).
        for root, dirs, files in os.walk(PLAYER):
            if Path(root) == PLAYER:
                dirs[:] = [d for d in dirs if d.lower() != 'data']
            for f in files:
                if f.lower() in ('orochi-settings.ini', 'port.log') or f.lower().endswith(('.iso', '.pdb')):
                    continue
                p = Path(root) / f
                add(p, f'{top}/programa/{p.relative_to(PLAYER).as_posix()}')
        # The source: never the game, its generated code, builds or dependencies.
        skip_dirs = {'build-tools', 'local', 'generated', 'vendor', 'thirdparty', 'logs', 'relatorios',
                     'Warriors Orochi', '__pycache__', '.git'}
        for root, dirs, files in os.walk(PORT):
            dirs[:] = [d for d in dirs if d not in skip_dirs]
            for f in files:
                if os.path.splitext(f)[1].lower() in ('.iso', '.bin', '.elf', '.exe', '.dll', '.pdb', '.ilk', '.chd', '.pyc', '.tmp'):
                    continue
                p = Path(root) / f
                add(p, f'{top}/codigo-fonte/{p.relative_to(PORT).as_posix()}')
        add(PORT / 'assets/package/COMPILAR.txt', f'{top}/codigo-fonte/COMPILAR.txt')
        # Logs of this PC, for comparison.
        if (PLAYER / 'data/port.log').exists():
            add(PLAYER / 'data/port.log', f'{top}/logs/port-ultima-execucao.log')
        if (PORT / 'logs').exists():
            for log in sorted((PORT / 'logs').glob('*.log')):
                add(log, f'{top}/logs/{log.name}')
    os.replace(temporary, PACKAGE)
    say(f'{PACKAGE}: {count} files, {PACKAGE.stat().st_size >> 20} MB')
    if skipped:
        say(f'left out because they name this PC or its user: {", ".join(skipped)}')


def run_setup():
    setup = PLAYER / PROGRAMS[1]
    if not setup.exists():
        sys.exit('[make] no player folder yet: run "make install"')
    subprocess.Popen([str(setup)], cwd=PLAYER)


if __name__ == '__main__':
    steps = {'help': lambda: print(MAKE_HELP), 'pathcheck': pathcheck, 'deps': deps, 'check': check, 'build': build, 'install': install,
             'reinstall': reinstall, 'package': package, 'run': run_setup}
    if len(sys.argv) != 2 or sys.argv[1] not in steps:
        sys.exit(__doc__)
    try:
        steps[sys.argv[1]]()
    except PermissionError as error:
        sys.exit(f'[make] {error.filename or "a file"} is in use: close the game and the Setup, then run it again')
    except subprocess.CalledProcessError as error:
        sys.exit(f'[make] step failed ({error.returncode}): {" ".join(map(str, error.cmd))}')
