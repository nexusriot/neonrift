Import("env")
import os
import hashlib

STAMP = os.path.join(env.subst("$BUILD_DIR"), "littlefs.sha256")

def sha_dir(path):
    h = hashlib.sha256()
    for root, _, files in os.walk(path):
        for fn in sorted(files):
            p = os.path.join(root, fn)
            h.update(os.path.relpath(p, path).encode("utf-8"))
            with open(p, "rb") as f:
                h.update(f.read())
    return h.hexdigest()

def after_upload(source, target, env):
    data_dir = os.path.join(env.subst("$PROJECT_DIR"), "data")
    if not os.path.isdir(data_dir):
        print(">>> Auto: no data/ dir, skipping uploadfs")
        return

    cur = sha_dir(data_dir)
    prev = None
    if os.path.isfile(STAMP):
        with open(STAMP, "r") as f:
            prev = f.read().strip()

#    if cur == prev:
#        print(">>> Auto: LittleFS unchanged, skipping uploadfs")
#        return

    pioenv = env.subst("$PIOENV")  # safer than env["PIOENV"]
    print(f">>> Auto: LittleFS changed, running uploadfs for env={pioenv}")

    rc = env.Execute(f"pio run -e {pioenv} -t uploadfs")
    if rc != 0:
        print(f">>> Auto: uploadfs FAILED (rc={rc}), NOT updating stamp")
        return

    os.makedirs(os.path.dirname(STAMP), exist_ok=True)
    with open(STAMP, "w") as f:
        f.write(cur)

env.AddPostAction("upload", after_upload)