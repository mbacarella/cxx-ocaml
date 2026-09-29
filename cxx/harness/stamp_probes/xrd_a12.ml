module M = struct class a = object val x = 1 val y = 2 method m = 1 end end
class b = object inherit M.a end
