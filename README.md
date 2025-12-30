# RTSPMSPM_vulkan-sim

## Environment Setup
This project depends on [Vulkan-Sim](https://github.com/ubc-aamodt-group/vulkan-sim).  
Additionally, you need to download [Volk](https://github.com/zeux/volk) for Vulkan function loading.


## Existing Problem
When a value in the resulting matrix is accumulated from multiple hits, the current version only keeps the last access result.

### Fast Method:
- **PTX Parser**: Modify `ptx_parser.py` to bypass untranslated `deref_atomic` instruction errors. Changes are implemented in the `PTXShader` class.
- **Instruction Hard Conversion**: In `ptx_lower_instructions.py`, add hard conversion logic for precompiled NIR instructions (address hard-coded) to convert them into PTX atomic-add instructions supported by GPGPU-Sim. This resolves the current Any-Hit shader execution issue. Changes are located in `translate_atomicAdd(ptx_shader)` and `writeToFile`.
- **PTX Target Fix**: Modify `MESA_SHADER_ANY_HIT_2.ptx` `.target` to `sm_20` using `fix_ptx_target(file_path, new_target="sm_20")`.
- **Affected Files**:  
  - `/home/zjw/vulkan-sim-root/mesa-vulkan-sim/src/compiler/ptx/ptx_lower_instructions.py`  
  - `/home/zjw/vulkan-sim-root/mesa-vulkan-sim/src/compiler/ptx/ptx_parser.py`

<br>
当结果矩阵中的某个数据由多次命中的累加时，当前版本只能保存最后一次访问的结果。

### 快速方法

● PTX 解析器 (ptx_parser.py) 放行 untranslated deref_atomic instruction 报错，修改位于 class PTXShader。<br>
● 在 ptx_lower_instructions.py 增加对前端 NIR 已编译指令的硬转换逻辑（地址硬编码）（转换为gpgpu-sim支持的原子加操作的ptx指令），解决当前 Any-Hit shader 执行问题，修改位于 translate_atomicAdd(ptx_shader)，并修正 writeToFile。<br>
● 修改 MESA_SHADER_ANY_HIT_2.ptx 的 .target 为 sm_20，修改函数为 fix_ptx_target(file_path, new_target="sm_20")。<br>
● 涉及文件路径：/home/zjw/vulkan-sim-root/mesa-vulkan-sim/src/compiler/ptx/ptx_lower_instructions.py 和 /home/zjw/vulkan-sim-root/mesa-vulkan-sim/src/compiler/ptx/ptx_parser.py。<br>

## Building the Project
To compile the project, use the following command:

```bash
g++ -std=c++17 T4.cpp volk/volk/volk.c -Ivolk -o build_blas -lvulkan -ldl
```
You can modify the following GLSL shader files:

.rgen (ray generation)

.rahit (any-hit)

.rchit (closest-hit)

.rmiss (miss shader)

After modifying, compile them to SPIR-V using glslangValidator. For example:
```bash
glslangValidator -V --target-env vulkan1.3 raygen2.rgen -o raygen.spv
```



