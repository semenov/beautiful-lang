import subprocess, signal, sys, time
p = subprocess.Popen(sys.argv[1:], stdout=subprocess.PIPE, stderr=subprocess.STDOUT, restore_signals=True, preexec_fn=lambda: signal.signal(signal.SIGINT, signal.SIG_DFL))
time.sleep(1.5)
p.send_signal(signal.SIGINT)
out = p.communicate()[0].decode()
print("exit", p.returncode, "lines", len(out.splitlines()))
print("\n".join(out.splitlines()[:4]))
