import os, signal, subprocess, sys, tempfile, time

tmp = tempfile.mkdtemp(prefix="termrep_")
base = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
fake = os.path.join(base, "iso", "tools", "fake_server.py")
root = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
appdir = os.path.join(root, "build")

port = int(sys.argv[1]) if len(sys.argv) > 1 else 19999
env = dict(os.environ)
env["SCENE_STORE_PORT"] = str(port)

fake_p = subprocess.Popen(
    [sys.executable, fake, str(port)],
    stdout=open(os.path.join(tmp, "fake.log"), "w"),
    stderr=subprocess.STDOUT,
)
time.sleep(1.5)

app = os.path.join(appdir, "iso_terminal.exe")
app_p = subprocess.Popen(
    [app],
    stdout=open(os.path.join(tmp, "app.log"), "w"),
    stderr=subprocess.STDOUT,
    env=env,
)

deadline = time.time() + 20
while time.time() < deadline:
    if app_p.poll() is not None:
        break
    time.sleep(0.5)

app_p.kill()
if os.name == "nt":
    subprocess.run(["taskkill", "/PID", str(app_p.pid), "/T", "/F"],
                   capture_output=True)
    time.sleep(1.5)
    subprocess.run(["taskkill", "/PID", str(app_p.pid), "/T", "/F"],
                   capture_output=True)
fake_p.send_signal(signal.CTRL_BREAK_EVENT if os.name == "nt" else signal.SIGTERM)
time.sleep(0.5)
try:
    fake_p.kill()
except Exception:
    pass
if os.name == "nt":
    # the win32 pty shell (cmd.exe) escapes taskkill /T; sweep it so the
    # dev machine is never left with stray processes (ISO uses fork/pty)
    time.sleep(1)
    subprocess.run(["powershell", "-Command",
        "Get-Process cmd -ErrorAction SilentlyContinue | Where-Object { $_.StartTime -gt (Get-Date).AddSeconds(-25) } | Stop-Process -Force"],
        capture_output=True)

print("=== app.log ===")
try:
    sys.stdout.write(open(os.path.join(tmp, "app.log")).read())
except Exception as e:
    print("no app log", e)
print("=== fake.log ===")
try:
    sys.stdout.write(open(os.path.join(tmp, "fake.log")).read())
except Exception as e:
    print("no fake log", e)