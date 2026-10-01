# Local changes to gpu/third_party/fsr-vulkan

The submodule points at commits on a local `bbport` branch that are not upstream
(FireBurn/FSR-Vulkan). After a fresh clone, apply them on top of the upstream commit
(`c64f093`):

    git -C gpu/third_party/fsr-vulkan checkout -b bbport c64f093
    git -C gpu/third_party/fsr-vulkan am ../../patches/fsr-vulkan/*.patch
