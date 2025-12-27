To ensure that the Vulkan-Sim environment is correctly installed and configured, we implement a minimal Vulkan compute demo as a sanity check.This demo verifies the complete execution path from the Vulkan API frontend to the GPGPU-Sim backend.

Successful execution of this demo indicates that:

- The Vulkan loader and ICD configuration are correct.
- Mesa correctly parses Vulkan commands and manages GPU resources.
- Vulkan-Sim successfully bridges Vulkan compute dispatches to GPGPU-Sim.
- GPGPU-Sim executes the PTX kernel and updates device memory as expected.