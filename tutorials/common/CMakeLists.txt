## Copyright 2009-2020 Intel Corporation
## SPDX-License-Identifier: Apache-2.0

ADD_SUBDIRECTORY(image)
ADD_SUBDIRECTORY(scenegraph)
ADD_SUBDIRECTORY(lights)
ADD_SUBDIRECTORY(texture)

IF (EMBREE_TUTORIALS OR (NOT EMBREE_BUILD_VERIFY))
  ADD_SUBDIRECTORY(tutorial)
  ADD_SUBDIRECTORY(imgui)
ENDIF()
