Getting started
===============

Start it from the Paperboat CLI:

.. code-block:: sh

   paperboat squero start squero-studio-om [Model.mo]   # http://localhost:59201
   paperboat squero stop squero-studio-om

It listens on 59201, or the next free port up to 59299.

Your first model
----------------

- Open a ``.mo`` file from the **files** rail, or File › New Model.
- Drag parts from the **library** onto the canvas and wire their ports; the source follows every
  edit.
- The dock under the canvas counts equations and unknowns; **Check** asks omc to check the model.
- The side pane shows the source and the scopes; Run › Simulate runs the model and plots it.

Select a part and press :kbd:`F1` to read its class's documentation, or type a class name
(``Resistor``) or a full path (``Modelica.Blocks.Continuous.PID``) in Help.

The workspace
-------------

Started without a model, the Studio opens its own folder, ``~/Documents/paperboat/squero/om``
(created if missing; ``SQUERO_OM_WORKSPACE`` overrides it): the first ``.mo`` in it, or a new
``Untitled.mo`` there. It never writes into the directory it was launched from.
