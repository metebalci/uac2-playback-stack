# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Mete Balci
# Controller ports of the stacks the build selects (U2PS_DEVICE,
# U2PS_HOST). An ARM build builds the device port that U2PS_PORT names, and
# with DWC2 also the host port. The PC build builds only the ports'
# decision logic, which has no register access, for the tests.

if(NOT CMAKE_CROSSCOMPILING)
  add_subdirectory(dwc2/common)
  if(U2PS_DEVICE)
    add_subdirectory(dwc2/dcd)
    add_subdirectory(chipidea/dcd)
  endif()
  if(U2PS_HOST)
    add_subdirectory(dwc2/hcd)
  endif()
elseif(U2PS_PORT STREQUAL "dwc2")
  add_subdirectory(dwc2/common)
  if(U2PS_DEVICE)
    add_subdirectory(dwc2/dcd)
  endif()
  if(U2PS_HOST)
    add_subdirectory(dwc2/hcd)
  endif()
elseif(U2PS_PORT STREQUAL "chipidea" AND U2PS_DEVICE)
  add_subdirectory(chipidea/dcd)
endif()
