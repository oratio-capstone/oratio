# Gateway software

Python code that runs on the Raspberry Pi gateway, plus its setup scripts.

| Path | What it is |
| --- | --- |
| `requirements.txt` | Runtime dependencies (what the Pi runs) |
| `requirements-dev.txt` | Runtime + test tools (laptops and CI) |
| `pi-setup/` | Scripts that set up a fresh Pi, see `pi-setup/README.md` |

## Python environment

Each machine has its own virtual environment; it is never committed.

| Where | Location | Created by |
| --- | --- | --- |
| Pi | `~/oratio-venv` | `pi-setup/02-gateway.sh` |
| Laptop | `sw/.venv` | The commands below |

Python **3.11 or newer** is required (check with `python3 --version`).

From the repo root, on macOS or Linux:

```bash
python3 -m venv sw/.venv
source sw/.venv/bin/activate
pip install -r sw/requirements-dev.txt
```

On Windows (PowerShell):

```powershell
py -m venv sw\.venv
sw\.venv\Scripts\Activate.ps1
pip install -r sw\requirements-dev.txt
```

Activate the environment in each new terminal before running or testing gateway code. In VS Code,
pick `sw/.venv` with **Python: Select Interpreter**.

## Changing dependencies

Versions are pinned exactly (`==`) so the Pi, CI and every laptop run the same code. To add or
upgrade a package: change the pin in `requirements.txt`, run `pip install -r sw/requirements-dev.txt`,
test, and commit the change. On the Pi, `pi-setup/02-gateway.sh` installs the new pins.
