Export a Paperboat station
==========================

The dock exports the model as a Paperboat station: **Download** a ``.pst``, or **Start** it as
``ssom_<model>`` on a free port (and **Stop** it). Both generate into ``<tmp>/ssom_<model>`` and run
``paperboat pack`` or ``paperboat start`` there, with the sibling ``../paperboat/bin/paperboat``, else
``$PAPERBOAT_BIN`` or ``paperboat`` on PATH. The first export of a model builds the engine, which takes
minutes.

In the station, the model's parameters (a ``parameter Real C1F``) are props of its Station object,
edited on the Station page; the Flow block carries only the inputs and outputs. See the
`Paperboat user manual <https://paperboatlab.github.io/paperboat/user_manual.html>`_ for running it.
