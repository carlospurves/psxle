from psxle import Console
import code
c = Console("../Games/kula.iso", debug=True)
c.run()
code.InteractiveConsole(locals=globals()).interact()