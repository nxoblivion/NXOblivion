"""Fetches the third-party sources of nx/app (reNut's renderer and its tools) into nx/app/thirdparty.

They are not committed (nx/.gitignore). They come from ChanseyIsTheBest's katamari-nx (GPL-3.0) at the commit this
port is based on, exactly as that commit has them (git archive, so the checkout's working tree and HEAD do not
matter):

    plume               plume at reNut's version with UnleashedRecomp-NX's Switch patches and katamari-nx's deko3d
                        back end
    reblue-XenosRecomp  reNut's shader translator (with its DXC headers, zstd, smol-v, xxHash, fmt)
    uam                 the deko3d shader compiler (Switch)
    imgui               the Dear ImGui copy of reNut's overlay
    smaa                SMAA's shader and lookup textures (render/smaa.cpp, shaders/host/smaa.hlsli)

katamari-nx's copy of reblue-XenosRecomp lacks the DXC binaries (its .gitignore drops bin/ and lib/): they come from
renderbag/dxc-bin, the submodule upstream XenosRecomp names, at the commit whose inc/ and CMakeLists.txt match
katamari-nx's (x64 only).

Usage:
    python nx/tools/fetch_deps.py                    katamari-nx from out/ref/katamari-nx when present, else cloned
                                                     into nx/out/deps/katamari-nx
    python nx/tools/fetch_deps.py --katamari DIR     use (or clone into) DIR
    python nx/tools/fetch_deps.py --force            replace folders that are already there
"""
import argparse
import io
import os
import shutil
import stat
import subprocess
import sys
import tarfile

KATAMARI_URL = 'https://github.com/ChanseyIsTheBest/katamari-nx.git'
KATAMARI_COMMIT = 'dcd7e2907a7ce84d9cfcf28e657d96624aa69d45'
DEPS = ['plume', 'reblue-XenosRecomp', 'uam', 'imgui', 'smaa']

DXC_URL = 'https://github.com/renderbag/dxc-bin.git'
DXC_COMMIT = '12395e36295f950361ca842d8cc461492412facc'
DXC_FILES = ['bin/x64/dxc.exe', 'bin/x64/dxcompiler.dll', 'bin/x64/dxil.dll', 'bin/x64/dxc-linux',
             'lib/x64/dxcompiler.lib', 'lib/x64/libdxcompiler.so', 'lib/x64/libdxil.so']


def run(*args, cwd=None, capture=False):
    print('>', ' '.join(args))
    if capture:
        return subprocess.run(args, cwd=cwd, check=True, stdout=subprocess.PIPE).stdout
    subprocess.run(args, cwd=cwd, check=True)
    return None


def remove_readonly(func, path, _):
    os.chmod(path, stat.S_IWRITE)
    func(path)


def rmtree(path):
    if sys.version_info >= (3, 12):
        shutil.rmtree(path, onexc=remove_readonly)
    else:
        shutil.rmtree(path, onerror=remove_readonly)


def has_commit(repo, commit):
    return subprocess.run(['git', '-C', repo, 'cat-file', '-e', commit + '^{commit}'],
                          stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL).returncode == 0


def ensure_repo(path, url, commit):
    """A git repository at path that has commit (cloned or fetched as needed)."""
    if not os.path.isdir(os.path.join(path, '.git')):
        os.makedirs(os.path.dirname(path), exist_ok=True)
        run('git', 'clone', '--no-checkout', url, path)
    if not has_commit(path, commit):
        run('git', 'fetch', 'origin', commit, cwd=path)
    if not has_commit(path, commit):
        sys.exit(f'{path}: commit {commit} not found')


def extract(repo, commit, prefix, target):
    """Writes the files under prefix at commit into target (prefix stripped)."""
    data = run('git', '-C', repo, 'archive', '--format=tar', commit, prefix, capture=True)
    count = 0
    with tarfile.open(fileobj=io.BytesIO(data)) as tar:
        for member in tar.getmembers():
            rel = os.path.relpath(member.name, prefix)
            if rel == '.' or rel.startswith('..'):
                continue
            dst = os.path.join(target, rel)
            if member.isdir():
                os.makedirs(dst, exist_ok=True)
            elif member.issym():
                continue  # none in these folders; Windows checkouts cannot make them anyway
            elif member.isfile():
                os.makedirs(os.path.dirname(dst), exist_ok=True)
                with tar.extractfile(member) as src, open(dst, 'wb') as out:
                    shutil.copyfileobj(src, out)
                count += 1
    return count


# Oblivion's changes to the fetched sources: nx/patches/<dep>-*.patch (applied with git apply inside the dep's folder)
# and the files copied in beside them. reblue-XenosRecomp: version 0x0E shader containers (XDK 2417; feedingfrenzy-nx's
# branch, Oblivion's container_0e.h from tools/renut/hlsl).
PATCH_FILES = {
    'reblue-XenosRecomp': [('tools/renut/hlsl/container_0e.h', 'XenosRecomp/container_0e.h')],
}


def patch_dep(nx, repo_root, dep, target):
    for src, dst in PATCH_FILES.get(dep, []):
        shutil.copyfile(os.path.join(repo_root, src), os.path.join(target, dst))
        print(f'{dep}: {dst} from {src}')
    patches = os.path.join(nx, 'patches')
    for name in sorted(os.listdir(patches)) if os.path.isdir(patches) else []:
        if name.startswith(dep + '-') and name.endswith('.patch'):
            run('git', 'apply', '--whitespace=nowarn', os.path.join(patches, name), cwd=target)
            print(f'{dep}: applied {name}')


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument('--katamari', help='katamari-nx checkout to read (cloned there if missing)')
    parser.add_argument('--force', action='store_true', help='replace dependency folders that already exist')
    args = parser.parse_args()

    nx = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
    repo_root = os.path.dirname(nx)
    thirdparty = os.path.join(nx, 'app', 'thirdparty')
    deps_cache = os.path.join(nx, 'out', 'deps')

    if args.katamari:
        katamari = os.path.abspath(args.katamari)
    elif os.path.isdir(os.path.join(repo_root, 'out', 'ref', 'katamari-nx', '.git')):
        katamari = os.path.join(repo_root, 'out', 'ref', 'katamari-nx')
    else:
        katamari = os.path.join(deps_cache, 'katamari-nx')
    ensure_repo(katamari, KATAMARI_URL, KATAMARI_COMMIT)

    os.makedirs(thirdparty, exist_ok=True)
    for dep in DEPS:
        target = os.path.join(thirdparty, dep)
        if os.path.exists(target):
            if not args.force:
                print(f'{target} exists, kept (--force replaces it)')
                continue
            rmtree(target)
        n = extract(katamari, KATAMARI_COMMIT, f'app/thirdparty/{dep}', target)
        print(f'{dep}: {n} files from katamari-nx {KATAMARI_COMMIT[:8]}')
        patch_dep(nx, repo_root, dep, target)

    dxc_target = os.path.join(thirdparty, 'reblue-XenosRecomp', 'thirdparty', 'dxc-bin')
    missing = [f for f in DXC_FILES if not os.path.isfile(os.path.join(dxc_target, f))]
    if missing:
        dxc_repo = os.path.join(deps_cache, 'dxc-bin')
        ensure_repo(dxc_repo, DXC_URL, DXC_COMMIT)
        for rel in missing:
            data = run('git', '-C', dxc_repo, 'show', f'{DXC_COMMIT}:{rel}', capture=True)
            dst = os.path.join(dxc_target, rel)
            os.makedirs(os.path.dirname(dst), exist_ok=True)
            with open(dst, 'wb') as out:
                out.write(data)
        print(f'dxc-bin: {len(missing)} binaries from renderbag/dxc-bin {DXC_COMMIT[:8]}')
    print('done')


if __name__ == '__main__':
    main()
