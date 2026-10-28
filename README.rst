amdgpu-wddm fork
================

This branch, ``amdgpu-wddm/main``, is the Mesa source of the Vulkan driver of the Direct3D 11 and 12 routes ``amdgpu_wddm_radv.dll`` (x64 and x86) in the amdgpu-wddm tester release 0.7.216.105-tester.33: commit `cf46ce17 <https://github.com/D-Ogi/mesa-amdgpu-wddm/commit/cf46ce17e9a8888f53efb3a58e55f0989642071c>`_, plus this note. The same release builds its other Mesa files from these commits:

- the system Vulkan driver ``vulkan_radeon.dll`` (x64): `ad130cc2 <https://github.com/D-Ogi/mesa-amdgpu-wddm/commit/ad130cc22883fc80ff36926ac5eebfdfbaefcdbc>`_
- the system Vulkan driver ``vulkan_radeon.dll`` (x86): `20d7c8c9 <https://github.com/D-Ogi/mesa-amdgpu-wddm/commit/20d7c8c95669fe94122cb948c75037f98b2583e7>`_
- the OpenGL driver ``amdgpu_wddm_gl.dll`` (x64): `e0d424ff <https://github.com/D-Ogi/mesa-amdgpu-wddm/commit/e0d424ff909c3e11bae0297d655f9b2383de4a1b>`_
- the desktop GPU driver ``bc250d3d_zink.dll`` (x64): `411f70e4 <https://github.com/D-Ogi/mesa-amdgpu-wddm/commit/411f70e4d1a03b067285c05e311821720bab22f9>`_
- the desktop processor driver ``bc250d3d.dll`` (x64 and x86): `2057d4cf <https://github.com/D-Ogi/mesa-amdgpu-wddm/commit/2057d4cffc81ce573611ed84c0ab20a5bf4822da>`_
- the Vulkan driver ``amdgpu_wddm_radv.dll`` of the desktop route (x64): `07421fb8 <https://github.com/D-Ogi/mesa-amdgpu-wddm/commit/07421fb8f07292e226d5b7469cc0f4e7cc7a53f1>`_

The driver project is https://github.com/D-Ogi/amdgpu-wddm. The other ``amdgpu-wddm/*`` branches hold work in progress and older release lines. The upstream project is https://gitlab.freedesktop.org/mesa/mesa; its README follows unchanged.

----

`Mesa <https://mesa3d.org>`_ - The 3D Graphics Library
======================================================


Source
------

This repository lives at https://gitlab.freedesktop.org/mesa/mesa.
Other repositories are likely forks, and code found there is not supported.


Build & install
---------------

You can find more information in our documentation (`docs/install.rst
<https://docs.mesa3d.org/install.html>`_), but the recommended way is to use
Meson (`docs/meson.rst <https://docs.mesa3d.org/meson.html>`_):

.. code-block:: sh

  $ meson setup build
  $ ninja -C build/
  $ sudo ninja -C build/ install

Support
-------

Many Mesa devs hang on IRC; if you're not sure which channel is
appropriate, you should ask your question on `OFTC's #dri-devel
<irc://irc.oftc.net/dri-devel>`_, someone will redirect you if
necessary.
Remember that not everyone is in the same timezone as you, so it might
take a while before someone qualified sees your question.
To figure out who you're talking to, or which nick to ping for your
question, check out `Who's Who on IRC
<https://dri.freedesktop.org/wiki/WhosWho/>`_.

The next best option is to ask your question in an email to the
mailing lists: `mesa-dev\@lists.freedesktop.org
<https://lists.freedesktop.org/mailman/listinfo/mesa-dev>`_


Bug reports
-----------

If you think something isn't working properly, please file a bug report
(`docs/bugs.rst <https://docs.mesa3d.org/bugs.html>`_).


Contributing
------------

Contributions are welcome, and step-by-step instructions can be found in our
documentation (`docs/submittingpatches.rst
<https://docs.mesa3d.org/submittingpatches.html>`_).

Note that Mesa uses gitlab for patches submission, review and discussions.
