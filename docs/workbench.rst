The workbench
=============

The rail
--------

The left rail switches between **files**, the model's folder as packages and classes
(``package.order`` included), and **library**, the Modelica Standard Library browser. It opens on
**files**. Opening a ``.mo`` adds a tab above the stage; a model outside a package needs no package
at all, and a class inside a package is opened with its package loaded first.

The rail's **+** (or File › New Class…) opens the New Class sheet: name, kind, package, an optional
base class and a live skeleton. It writes the ``.mo`` and appends ``package.order`` when the class
lands in a package.

The stage
---------

The stage shows the model as a **diagram** (the canvas), as **text** (its ``.mo``) or, for a
``Modelica.Mechanics.MultiBody`` model that has been simulated, in **3d**: its shapes played back
against the run's time. Try ``bin/squero-om examples/multibody/DoublePendulum.mo``.

Checking a model
----------------

The **Model** menu runs ``checkModel`` and shows its messages as cards in the Console's
**messages** tab; **Show Flat Model** opens ``instantiateModel`` beside the generated C. The
Console's **console** tab streams omc's own log as it works.
