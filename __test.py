import os, sys, subprocess

CWD = os.path.dirname(__file__)
os.chdir(CWD)


def build_optcuts(config="Release"):
    cpu = max(1, os.cpu_count() - 1)
    print(f">>> use cpu core {cpu}")

    subprocess.run(
        f"cmake --build build --config {config} --target OptCuts_bin -j {cpu}",
        shell=True,
    )


def run_solver(headless=True, config="Release"):
    input_model = "input/bimba_i_f10000.obj"
    input_model = "input/benchmark/bishop_part.obj"  # 简单模型

    mode = 100 if headless else 10
    OPT_EXE = os.path.abspath(f"build/{config}/OptCuts_bin.exe")  # 需要绝对路径

    subprocess.run(f"{OPT_EXE} {mode} {input_model} 0.999 1 0 4.1 1 0", shell=True)


if __name__ == "__main__":
    build_optcuts()
    run_solver()


__doc__ = r"""
### argv[1]
progMode
0:optimization mode
1:diagnostic mode
2:mesh processing mode

10:offline optimization mode
100:headless mode

### argv[2]
mesh_path
### argv[3]
lambda_init = 0.999
### argv[4]
testID = std::stod(argv[4]);
### argv[5]
methodType = OptCuts::MethodType(std::stoi(argv[5]));
OptCuts::MT_OPTCUTS_NODUAL:"OptCuts_noDual"  
OptCuts::MT_OPTCUTS:"OptCuts"
OptCuts::MT_EBCUTS:"EBCuts"
OptCuts::MT_DISTMIN:

### argv[6]
upperBound = std::stod(argv[6]);

### argv[7]
bijectiveParam = std::stoi(argv[7]);
bijectivity:(bijectiveParam ? "ON" : "OFF")

### argv[8]
initCutOption = std::stoi(argv[8]);
0:"random 2-edge initial cut for closed surface"
1:"farthest 2-point initial cut for closed surface"
default:
"input initial cut option invalid, use default"
"random 2-edge initial cut for closed surface"

### Params
"10",
"xxx.obj",
"0.999",
"1",
"0",
"4.1",
"1",
"0"
"""
