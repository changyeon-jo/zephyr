.. _snippet-video-sw-h264enc:

Video Software H.264 Encoder Snippet (video-sw-h264enc)
#######################################################

.. code-block:: console

   west build -S video-sw-h264enc [...]

Overview
********

This snippet instantiates a software H.264 encoder for test purposes. It is selected as the
``zephyr,videoenc`` :ref:`devicetree` chosen node.

The encoder produces valid Constrained Baseline streams that any decoder can read, but made of
uncompressed and skipped macroblocks only, so the output is large and the image is refreshed
progressively between key frames.

Requirements
************

No hardware support is required. :kconfig:option:`CONFIG_VIDEO_ENCODER_H264` must be enabled,
and :kconfig:option:`CONFIG_VIDEO_BUFFER_POOL_HEAP_SIZE` must fit one input frame and one output
frame, the latter being about 1.5 times the size of an NV12 frame of the same resolution.
