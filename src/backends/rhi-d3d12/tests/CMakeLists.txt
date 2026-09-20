cy_add_test(
    NAME rhi_d3d12
    KIND unit
    SOURCES test_d3d12_device.cpp
    DEPENDENCIES cy::rhi-d3d12 cy::rhi-null)
