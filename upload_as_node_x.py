Import("env")
import os, filecmp, functools

def upload():
    pio_exe = env.subst("$PYTHONEXE") + " -m platformio"

    # 2. Get the current environment name (e.g., ATtiny3224)
    current_env = env.subst("$PIOENV")

    # 3. Construct the full string
    full_cmd = f"{pio_exe} run --target upload --environment {current_env}"
    return env.Execute(full_cmd)

def set_node_id(node_id, source, target, env):
    if node_id == None:
        lines = [
            '#error "Compilation stopped: Upload from ATtiny3224 > Custom > Upload as Node x instead"'
        ]
    else:
        lines = [
            "#ifndef NODEID_H",
            "#define NODEID_H",
            f"#define NODE_ID {node_id}",
            "#endif"
        ]

    provisional = "src/attiny3224/nodeid.x"
    final = "src/attiny3224/nodeid.h"
    with open(provisional, "w") as fp:
        fp.writelines(f"{line}\n" for line in lines)

    if not os.path.exists(final):
        os.rename(provisional, final)
    elif not filecmp.cmp(provisional, final):
        os.remove(final)
        os.rename(provisional, final)
    else:
        os.remove(provisional)

    if node_id != None:
        result = upload()
        # set_node_id(None, None, None, None) # block normal uploads
        return result

TOTAL_NODES = 4

for i in range(1, TOTAL_NODES + 1):
    target_name = f"upload_node_{i}"
    callback = functools.partial(set_node_id, i)
    
    env.AddCustomTarget(
        name=target_name,
        dependencies=None,
        actions=[callback],
        title=f"Upload as Node {i}",
        description=f"Uploads the fw with node id set to {i}"
    )