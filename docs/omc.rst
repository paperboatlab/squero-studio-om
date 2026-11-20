OpenModelica (omc)
==================

The Studio finds ``omc`` itself (``squero-om doctor`` says which, and checks that the MSL loads):
``$SQUERO_OMC_BIN``, else ``omc`` on PATH if its install can build FMUs, else podman (or docker)
running ``openmodelica/openmodelica:v1.27.1-minimal``, pulled on first use (``squero-om update-omc``
pulls it again). ``SQUERO_OMC=native|podman|docker`` forces one.

The container runs as you (rootless podman ``--userns=keep-id``, docker ``--user``) with your
``/tmp`` and ``$HOME`` at the same paths and no SELinux relabelling, so everything it writes
(stations, FMUs, ``~/.openmodelica/libraries``) is yours.

Windows
-------

Windows runs omc natively only: ``$SQUERO_OMC_BIN``, else ``omc`` on PATH, else
``%OPENMODELICAHOME%\bin\omc.exe``, which the OpenModelica installer sets
(``winget install OpenModelica.OpenModelica.Official``).
