module M = struct class a = object val x = 1 end end
class b = object inherit M.a end
