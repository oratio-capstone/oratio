import pathlib
import shutil
import subprocess
import sys
import tempfile
import re


def run_git(workspace, *arguments):
    return subprocess.run(
        ["git", "-C", str(workspace), *arguments],
        capture_output=True,
        text=True,
        check=False,
    )


def patch_files(patch_path):
    files = []

    for line in patch_path.read_text(encoding="utf-8").splitlines():
        if not line.startswith("+++ b/"):
            continue

        relative_path = pathlib.PurePosixPath(line[6:].split("\t")[0])

        if relative_path.is_absolute() or ".." in relative_path.parts:
            raise RuntimeError(f"Unexpected patch path: {relative_path}")

        files.append(pathlib.Path(*relative_path.parts))

    if not files:
        raise RuntimeError(f"No target files found in {patch_path}")

    return list(dict.fromkeys(files))


def forwarding_fix_is_present(target_root):
    source_path = target_root / "src" / "core" / "ipv4" / "ip4.c"
    source = source_path.read_text(encoding="utf-8")

    # Restrict the check to the forwarding function's NAPT section.
    function_start = source.find(
        "ip4_forward(struct pbuf *p, struct ip_hdr *iphdr, struct netif *inp)"
    )

    if function_start == -1:
        return False

    section_end = source.find(
        "/* Incrementally update the IP checksum.",
        function_start,
    )

    if section_end == -1:
        return False

    section = source[function_start:section_end]
    section = re.sub(r"/\*.*?\*/", "", section, flags=re.DOTALL)
    section = re.sub(r"\s+", "", section)

    expected = (
        "#ifESP_LWIP"
        "#ifIP_NAPT"
        "if(!netif->napt){"
        "if(ip_napt_forward(p,iphdr,inp,netif)!=ERR_OK)"
        "return;"
        "}"
        "#endif"
        "#endif"
    )

    return expected in section


def prepare_patch(patch_path, target_root, workspace):
    if patch_path.name == "ip4_forward.patch":
        if forwarding_fix_is_present(target_root):
            print(
                "[bridge-patches] Verified existing forwarding fix: ip4_forward.patch"
            )
            return []

    files = patch_files(patch_path)

    for relative_path in files:
        source = target_root / relative_path
        destination = workspace / relative_path

        if not source.is_file():
            raise RuntimeError(f"Missing patch target: {source}")

        destination.parent.mkdir(parents=True, exist_ok=True)
        shutil.copy2(source, destination)

    result = run_git(workspace, "init", "--quiet")

    if result.returncode != 0:
        raise RuntimeError(result.stderr)

    reverse_check = run_git(workspace, "apply", "--reverse", "--check", str(patch_path))

    if reverse_check.returncode == 0:
        print(f"[bridge-patches] Already applied: {patch_path.name}")
        return []

    check = run_git(workspace, "apply", "--check", "--verbose", str(patch_path))

    if check.returncode != 0 or "Skipped patch" in check.stderr:
        raise RuntimeError(
            f"Cannot apply {patch_path.name}:\n{check.stdout}{check.stderr}"
        )

    result = run_git(workspace, "apply", "--verbose", str(patch_path))

    if result.returncode != 0 or "Skipped patch" in result.stderr:
        raise RuntimeError(
            f"Patch failed: {patch_path.name}\n{result.stdout}{result.stderr}"
        )

    verify = run_git(workspace, "apply", "--reverse", "--check", str(patch_path))

    if verify.returncode != 0:
        raise RuntimeError(f"Patch verification failed: {patch_path.name}")

    print(f"[bridge-patches] Prepared: {patch_path.name}")

    return [
        (workspace / relative_path, target_root / relative_path)
        for relative_path in files
    ]


def main():
    framework = pathlib.Path(sys.argv[1]).resolve()
    bridge = pathlib.Path(sys.argv[2]).resolve()
    lwip = framework / "components" / "lwip" / "lwip"

    project_root = pathlib.Path(__file__).resolve().parents[1]

    patches = [
        (project_root / "patches" / "dhcp-idf-5.5.5.patch", lwip),
        (bridge / "patch" / "dhcpserver.patch", framework),
        (bridge / "patch" / "ip4_forward.patch", lwip),
        (bridge / "patch" / "ip4_napt.patch", lwip),
    ]

    pending_files = []

    with tempfile.TemporaryDirectory(prefix="oratio-bridge-patches-") as temporary:
        temporary_root = pathlib.Path(temporary)

        # Check every patch before changing framework files.
        for index, (patch_path, target_root) in enumerate(patches):
            workspace = temporary_root / str(index)
            workspace.mkdir()

            pending_files.extend(prepare_patch(patch_path, target_root, workspace))

        for patched_file, destination in pending_files:
            backup = destination.with_name(destination.name + ".oratio-before-patch")

            if not backup.exists():
                shutil.copy2(destination, backup)

            shutil.copy2(patched_file, destination)

        print("[bridge-patches] All required patches verified")


if __name__ == "__main__":
    try:
        main()
    except Exception as error:
        print(f"[bridge-patches] ERROR: {error}", file=sys.stderr)
        sys.exit(1)
